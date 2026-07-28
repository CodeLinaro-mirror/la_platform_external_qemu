/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Renderer.h"
#include "RendererGLES.h"
#include "RendererVulkan.h"

#include <algorithm>
#include <cstring>

#include <libyuv.h>

#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"

using namespace android::base;

#define E(...) derror(__VA_ARGS__)
#define W(...) dwarning(__VA_ARGS__)
#define D(...) dprint(__VA_ARGS__)

static constexpr int kMaxViewDimension = 16384;

namespace android {
namespace ver {

/*******************************************************************************
 *                     ImageScaler routines
 ******************************************************************************/

ImageScaler::ImageScaler(int width, int height, uint8_t* buffer)
    : mFrameWidth(width), mFrameHeight(height), mOutputRgba(buffer) {}

bool ImageScaler::updateImage(int inputWidth,
                              int inputHeight,
                              const uint8_t* inputRgba,
                              ScaleMode mode) {
    if (!inputRgba || !mOutputRgba || inputWidth <= 0 || inputHeight <= 0 ||
        mFrameWidth <= 0 || mFrameHeight <= 0) {
        derror("%s: Invalid input dimensions or pointers", __func__);
        return false;
    }

    int result = -1;
    switch (mode) {
        case ScaleMode::AspectFitLetterbox:
            result = aspectFitLetterbox(inputWidth, inputHeight, inputRgba);
            break;
        case ScaleMode::AspectFitZoom:
            result = aspectFitZoom(inputWidth, inputHeight, inputRgba);
            break;
        case ScaleMode::ScaleToFill:
            result = scaleToFill(inputWidth, inputHeight, inputRgba);
            break;
        default:
            derror("%s: Unknown ScaleMode encountered", __func__);
            return false;
    }

    if (result != 0) {
        derror("%s: libyuv::ARGBScale failed! error: %d", __func__, result);
        return false;
    }

    return true;
}

int ImageScaler::aspectFitLetterbox(int inputWidth,
                                    int inputHeight,
                                    const uint8_t* inputRgba) {
    float scaleX = static_cast<float>(mFrameWidth) / inputWidth;
    float scaleY = static_cast<float>(mFrameHeight) / inputHeight;
    float scale = std::min(scaleX, scaleY);

    int targetWidth = static_cast<int>(inputWidth * scale);
    int targetHeight = static_cast<int>(inputHeight * scale);

    int offsetX = (mFrameWidth - targetWidth) / 2;
    int offsetY = (mFrameHeight - targetHeight) / 2;

    std::memset(mOutputRgba, 0, mFrameWidth * mFrameHeight * 4);

    int inputStride = inputWidth * 4;
    int outputStride = mFrameWidth * 4;

    uint8_t* dstPtr = mOutputRgba + (offsetY * outputStride) + (offsetX * 4);
    return libyuv::ARGBScale(inputRgba, inputStride, inputWidth, inputHeight,
                             dstPtr, outputStride, targetWidth, targetHeight,
                             libyuv::kFilterBilinear);
}

int ImageScaler::aspectFitZoom(int inputWidth,
                               int inputHeight,
                               const uint8_t* inputRgba) {
    float scaleX = static_cast<float>(mFrameWidth) / inputWidth;
    float scaleY = static_cast<float>(mFrameHeight) / inputHeight;
    float scale = std::max(scaleX, scaleY);

    int srcCropWidth = static_cast<int>(mFrameWidth / scale);
    int srcCropHeight = static_cast<int>(mFrameHeight / scale);

    int srcOffsetX = (inputWidth - srcCropWidth) / 2;
    int srcOffsetY = (inputHeight - srcCropHeight) / 2;

    int inputStride = inputWidth * 4;
    int outputStride = mFrameWidth * 4;

    const uint8_t* srcPtr =
            inputRgba + (srcOffsetY * inputStride) + (srcOffsetX * 4);
    return libyuv::ARGBScale(srcPtr, inputStride, srcCropWidth, srcCropHeight,
                             mOutputRgba, outputStride, mFrameWidth,
                             mFrameHeight, libyuv::kFilterBilinear);
}

int ImageScaler::scaleToFill(int inputWidth,
                             int inputHeight,
                             const uint8_t* inputRgba) {
    int inputStride = inputWidth * 4;
    int outputStride = mFrameWidth * 4;

    return libyuv::ARGBScale(inputRgba, inputStride, inputWidth, inputHeight,
                             mOutputRgba, outputStride, mFrameWidth,
                             mFrameHeight, libyuv::kFilterBilinear);
}

/*******************************************************************************
 *                     RendererView routines
 ******************************************************************************/

bool RendererView::updateTarget(VerImageFormat format,
                                int frameWidth,
                                int frameHeight) {
    std::lock_guard lock(mLock);
    if (frameWidth <= 0 || frameWidth > kMaxViewDimension || frameHeight <= 0 ||
        frameHeight > kMaxViewDimension) {
        derror("%s: rejecting out-of-range view dimensions %dx%d", __func__,
               frameWidth, frameHeight);
        return false;
    }
    if (mFormat == format && mFrameWidth == frameWidth &&
        mFrameHeight == frameHeight) {
        return true;
    }

    mFormat = format;
    mFrameWidth = frameWidth;
    mFrameHeight = frameHeight;
    mCache.invalidate();
    return true;
}

void RendererView::updateViewProjection(const glm::mat4& viewProj) {
    std::lock_guard lock(mLock);
    if (mViewProjection == viewProj) {
        return;
    }

    mViewProjection = viewProj;
    mCache.invalidate();
}

void RendererView::setBlurFactor(float factor) {
    std::lock_guard lock(mLock);
    if (mBlurFactor == factor) {
        return;
    }

    mBlurFactor = factor;
    mCache.invalidate();
}

void RendererView::preRenderLocked() {
    const size_t viewCacheSize =
            static_cast<size_t>(mFrameWidth) * mFrameHeight * 4;
    mCache.mFramebufferRGBA8.resize(viewCacheSize);

    const size_t scratchBufferNumItems =
            (static_cast<size_t>(mFrameWidth) * (mFrameHeight + 1) * 16) /
            sizeof(uint32_t);
    if (mCache.mBlurScratchBuffer.size() < scratchBufferNumItems) {
        mCache.mBlurScratchBuffer.resize(scratchBufferNumItems);
    }
}

void RendererView::postRenderLocked() {
    if (mBlurFactor > 0) {
        applyBlurInPlaceCPU(mFrameWidth, mFrameHeight,
                            mCache.mFramebufferRGBA8.data(),
                            mCache.mBlurScratchBuffer.data(), mBlurFactor);
    }
}

void RendererView::applyBlurInPlaceCPU(int width,
                                       int height,
                                       uint8_t* rgbaDataInOut,
                                       int32_t* scratchBuffer,
                                       float sigma) {
    if (sigma <= 0 || !scratchBuffer) {
        return;
    }

    const int blurRadius = std::min((int)sigma, 16);
    const int stride = width * 4;

    std::vector<uint8_t> tempData(width * height * 4);
    int result =
            libyuv::ARGBBlur(rgbaDataInOut, stride, tempData.data(), stride,
                             scratchBuffer, stride, width, height, blurRadius);
    if (result != 0) {
        derror("%s: libyuv::ARGBBlur failed for radius=%d (result = %d)",
               __func__, blurRadius, result);
        return;
    }
    memcpy(rgbaDataInOut, tempData.data(), tempData.size());
}

/*******************************************************************************
 *                     Renderer routines
 ******************************************************************************/

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

std::unique_ptr<Renderer> Renderer::create(
        const std::filesystem::path& vulkanBasePath) {
    std::string backend = System::get()->envGet("VER_RENDERER_BACKEND");

    if (backend == "vulkan") {
        dprint("VER: Forced Vulkan renderer via VER_RENDERER_BACKEND.");
        auto vulkanRenderer = RendererVulkan::create(vulkanBasePath);
        if (!vulkanRenderer) {
            derror("VER: Could not create forced Vulkan renderer.");
        }
        return vulkanRenderer;
    }

    if (backend == "gles") {
        dprint("VER: Forced GLES renderer via VER_RENDERER_BACKEND.");
        auto glesRenderer = RendererGLES::create(vulkanBasePath);
        if (!glesRenderer) {
            derror("VER: Could not create forced GLES renderer.");
        }
        return glesRenderer;
    }

    dprint("VER: Attempting to create Vulkan renderer.");
    auto vulkanRenderer = RendererVulkan::create(vulkanBasePath);
    if (vulkanRenderer) {
        dprint("VER: Successfully created Vulkan renderer.");
        return vulkanRenderer;
    }

    dprint("VER: Could not create Vulkan renderer, falling back to GLES renderer.");
    auto glesRenderer = RendererGLES::create(vulkanBasePath);
    if (glesRenderer) {
        dprint("VER: Successfully created GLES renderer.");
        return glesRenderer;
    }

    derror("VER: Could not create GLES renderer either.");
    return nullptr;
}

}  // namespace ver
}  // namespace android
