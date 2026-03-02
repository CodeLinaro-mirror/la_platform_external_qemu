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
#include "android/virtualscene/Scene.h"

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

    // "environment" means the camera will use the global environment
    // scene, defined in environment.ini, to render the frames
    // Otherwise, it'll create it's own scene
    mUseEnvironmentScene = (name == "environment");
}

RenderedCameraDevice::~RenderedCameraDevice() {
    stopCapturing();
}

int RenderedCameraDevice::startCapturing(uint32_t pixelFormat,
                                         int frameWidth,
                                         int frameHeight) {
    VLOG(camera) << "Start capturing at " << frameWidth << " x " << frameHeight;

    // TODO(virtualscene-manager): support multiple scenes
    if (!mUseEnvironmentScene) {
        // If the camera mode is not set to "environment", the camera
        // needs to create a scene that it'll own.
        SceneConfig::Mode mode = SceneConfig::Mode::Mesh3dScene;
        SceneConfig defaultSceneConfig(
                mode, SceneConfig::defaultFilenameForMode(mode));
        if (VirtualSceneManager::initialize(defaultSceneConfig)) {
            LOG(INFO) << "Initialized VirtualSceneManager for the camera";
        }
    }

    if (!VirtualSceneManager::isInitialized()) {
        LOG(ERROR) << "Virtual scene is not initialized!";
        stopCapturing();
        return -1;
    }

    mSceneCamera.setAspectRatio(static_cast<float>(frameWidth) / frameHeight);

    mActiveView = VirtualSceneManager::createView(
            formatFromCameraFormat(pixelFormat), frameWidth, frameHeight);

    VirtualSceneManager::setSceneControlsParameters(true);

    return 0;
}

// Resets camera device after capturing.
// Since new capture request may require different frame dimensions we must
// reset camera device by reopening its handle. Otherwise attempts to set up new
// frame properties (different from the previous one) may fail.
void RenderedCameraDevice::stopCapturing() {
    mActiveView.reset();
    VirtualSceneManager::setSceneControlsParameters(false);
}

int RenderedCameraDevice::readFrame(ClientFrame* resultFrame,
                                    float rScale,
                                    float gScale,
                                    float bScale,
                                    float expComp,
                                    const char* direction,
                                    int orientation) {
    // TODO(virtualscene-perf): update the view here to avoid resizing?
    // TODO(virtualscene-manager): should be updated once, externally
    VirtualSceneManager::update();

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

        uint32_t pixelFormat =
                cameraFormatFromFormat(mActiveView->getFormatLocked());
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
