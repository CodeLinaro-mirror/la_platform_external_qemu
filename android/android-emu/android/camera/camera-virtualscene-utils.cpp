/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "android/camera/camera-virtualscene-utils.h"
#include "android/virtualscene/Renderer.h"

#define VIRTUALSCENE_PIXEL_FORMAT V4L2_PIX_FMT_RGB32

#ifdef _WIN32
#undef ERROR
#endif

using namespace gfxstream::host::gl;

namespace android {
namespace virtualscene {

static RendererView::Format formatFromCameraFormat(uint32_t cameraPixelFormat) {
    if (cameraPixelFormat == V4L2_PIX_FMT_RGB32) {
        return RendererView::Format::RGBA8;
    }
    derror("Unsupported camera format for virtual scene views %lu",
           cameraPixelFormat);
    return RendererView::Format::RGBA8;
}

static uint32_t cameraFormatFromFormat(RendererView::Format format) {
    if (format == RendererView::Format::RGBA8) {
        return V4L2_PIX_FMT_RGB32;
    }
    derror("Unknown view format %lu", (uint32_t)format);
    return 0;
}

RenderedCameraDevice::RenderedCameraDevice(std::string_view name) {
    mHeader.opaque = this;
    if (name == "virtualscene") {
        mSceneMode = SceneMode::VirtualScene;
    }
    else if (name == "videoplayback") {
        mSceneMode = SceneMode::VideoPlayback;
    }
    else {
        mSceneMode = SceneMode::Unknown;
    }
}

RenderedCameraDevice::~RenderedCameraDevice() {
    stopCapturing();
}

int RenderedCameraDevice::startCapturing(uint32_t pixelFormat, int frameWidth, int frameHeight) {
    VLOG(camera) << "Start capturing at " << frameWidth << " x " << frameHeight;

    if (mSceneMode == SceneMode::VirtualScene) {
        if (!VirtualSceneManager::isInitialized()) {
            // TODO(virtualscene): support multiple scenes and initialize
            // 'environment' scene always from the environment service
            const bool cameraUsesEnvironment = false;
            if (!cameraUsesEnvironment && VirtualSceneManager::initialize()) {
                LOG(INFO) << "Initialized VirtualSceneManager";
            } else {
                LOG(ERROR) << "Virtual scene is not initialized!";
                stopCapturing();
                return -1;
            }
        }
        VirtualSceneManager::showSceneControls(true);

        mSceneCamera.setAspectRatio(static_cast<float>(frameWidth) / frameHeight);
    }
    else if (mSceneMode == SceneMode::VideoPlayback) {
        //TODO(virtualscene): create video playback scene and create a view from it
    }

    mActiveView = std::make_unique<RendererView>();
    mActiveView->updateTarget(formatFromCameraFormat(pixelFormat), frameWidth, frameHeight);

    return 0;
}

// Resets camera device after capturing.
// Since new capture request may require different frame dimensions we must
// reset camera device by reopening its handle. Otherwise attempts to set up new
// frame properties (different from the previous one) may fail.
void RenderedCameraDevice::stopCapturing() {
    mActiveView.reset();
    VirtualSceneManager::showSceneControls(false);
}

int RenderedCameraDevice::readFrame(ClientFrame* resultFrame,
                                    float rScale,
                                    float gScale,
                                    float bScale,
                                    float expComp,
                                    const char* direction,
                                    int orientation) {
    if (mSceneMode == SceneMode::VideoPlayback) {
        // TODO(virtualscene): create video playback scene and render a view
        static float animTime = 0;
        animTime += 0.01f;
        if (animTime > 1) {
            animTime = 0;
        }

        std::lock_guard lock(mActiveView->mLock);
        mActiveView->preRenderLocked();
        const int dummyVideoWidth = mActiveView->getWidthLocked();
        const int dummyVideoHeight = mActiveView->getHeightLocked();
        const int stride = dummyVideoWidth * 4;
        std::vector<uint8_t>& fbData = mActiveView->getFramebufferLocked();
        if(fbData.size() < dummyVideoWidth * dummyVideoHeight * 4) {
            // preRenderLocked failed
            return -1;
        }
        for (int y = 0; y < dummyVideoHeight; y++) {
            for (int x = 0; x < dummyVideoWidth; x++) {
                // Render a procedural animation
                uint8_t& r = fbData[(y * dummyVideoWidth + x)*4 + 0];
                uint8_t& g = fbData[(y * dummyVideoWidth + x)*4 + 1];
                uint8_t& b = fbData[(y * dummyVideoWidth + x)*4 + 2];
                uint8_t& a = fbData[(y * dummyVideoWidth + x)*4 + 3];

                float u = (x / (float)dummyVideoWidth) * 10.0;
                float v = (y / (float)dummyVideoHeight) * 10.0;
                float local_u = u - floor(u);
                float local_v = v - floor(v);
                float dist = abs(local_u - 0.5) + abs(local_v - 0.5);
                float threshold =
                        0.1 + 0.4 * (0.5 + 0.5 * sin(animTime * 6.283));
                float mask = (dist < threshold) ? 1.0 : 0.0;

                r = (uint8_t)(mask * 100);
                g = (uint8_t)(mask * 200);
                b = (uint8_t)(mask * 255);
                a = 255;
            }
        }
        mActiveView->postRenderLocked();

        uint32_t pixelFormat =
                cameraFormatFromFormat(mActiveView->getFormatLocked());
        return convert_frame(fbData.data(), pixelFormat, fbData.size(),
                             dummyVideoWidth, dummyVideoHeight, resultFrame,
                             rScale, gScale, bScale, expComp, "front", 1);
    }

    // TODO(virtualscene): create the view here based on the target resolution to avoid resizing?
    VirtualSceneManager::update();  // TODO(virtualscene): should be updated once, externally

    // Update camera based on physical model and set view projection accordingly
    mSceneCamera.update();
    mActiveView->updateViewProjection(mSceneCamera.getViewProjection());

    resultFrame->frame_time = mSceneCamera.getTimestamp();

    float renderTime = 0.0f;
    if (VirtualSceneManager::getAnimationState()) {
        renderTime = resultFrame->frame_time / 1000000000.0f;
    }

    int conversionResult = -1;
    VirtualSceneManager::renderView(mActiveView.get(), renderTime, [&]() {
        const std::vector<uint8_t>& fbData =
                mActiveView->getFramebufferLocked();

        uint32_t pixelFormat = cameraFormatFromFormat(mActiveView->getFormatLocked());
        conversionResult = convert_frame(
                fbData.data(), pixelFormat, fbData.size(),
                mActiveView->getWidthLocked(), mActiveView->getHeightLocked(),
                resultFrame, rScale, gScale, bScale, expComp, "front", 1);
    });

    // Convert frame to the receiving buffers.
    return conversionResult;
}

}  // namespace virtualscene
}  // namespace android
