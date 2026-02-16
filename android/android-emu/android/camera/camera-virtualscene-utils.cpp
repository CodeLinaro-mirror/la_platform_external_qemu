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

#include "android/camera/camera-virtualscene.h"
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
    // scene, defined in environment.ini file
    mUseEnvironmentScene = (name == "environment");
    mName = name;
    mScene = nullptr;  // Set at startCapturing

    LOG(INFO) << "Initialized camera with name: " << name;
}

RenderedCameraDevice::~RenderedCameraDevice() {
    stopCapturing();
}

int RenderedCameraDevice::startCapturing(uint32_t pixelFormat,
                                         int frameWidth,
                                         int frameHeight) {
    VLOG(camera) << "Start capturing at " << frameWidth << " x " << frameHeight;

    if (mUseEnvironmentScene) {
        mScene = VirtualSceneManager::addSceneUser();
    }else{
        // Create and own the scene
        std::string sceneMode;
        std::string sceneFilename;
        const size_t sepPos =
                mName.find(camera_virtualscene_name_argument_separator());
        if (sepPos != std::string::npos) {
            sceneMode = mName.substr(0, sepPos);
            sceneFilename = mName.substr(sepPos + 1);
        } else {
            sceneMode = mName;
        }
        SceneConfig::Mode mode = SceneConfig::modeFromString(sceneMode);
        if (sceneFilename.empty()) {
            // Create with default content if a filename is not given
            sceneFilename = SceneConfig::defaultFilenameForMode(mode);
        }
        SceneConfig sceneConfig(mode, sceneFilename);
        mScene = ScenesManager::createScene(mName, sceneConfig);
    }

    if (!mScene) {
        LOG(ERROR) << "Camera scene could not be not initialized!";
        stopCapturing();
        return -1;
    }

    mSceneCamera.setAspectRatio(static_cast<float>(frameWidth) / frameHeight);

    mActiveView = std::make_unique<RendererView>();
    mActiveView->updateTarget(formatFromCameraFormat(pixelFormat), frameWidth,
                              frameHeight);

    if (mUseEnvironmentScene) {
        VirtualSceneManager::setSceneControlsParameters(true);
    }

    return 0;
}

// Resets camera device after capturing.
// Since new capture request may require different frame dimensions we must
// reset camera device by reopening its handle. Otherwise attempts to set up new
// frame properties (different from the previous one) may fail.
void RenderedCameraDevice::stopCapturing() {
    mActiveView.reset();

    if (!mScene) {
        return;
    }

    if (mUseEnvironmentScene) {
        VirtualSceneManager::setSceneControlsParameters(false);
        VirtualSceneManager::removeSceneUser();
    } else {
        ScenesManager::removeScene(mScene.get());
    }
    mScene.reset();
}

int RenderedCameraDevice::readFrame(ClientFrame* resultFrame,
                                    float rScale,
                                    float gScale,
                                    float bScale,
                                    float expComp,
                                    const char* direction,
                                    int orientation) {
    if (!mScene) {
        LOG(ERROR) << "Virtual scene is not initialized!";
        return -1;
    }
    // TODO(virtualscene-perf): update the view here to avoid resizing?
    if (mUseEnvironmentScene) {
        // TODO(virtualscene-manager): update externally
        VirtualSceneManager::update();
    } else {
        mScene->update();
    }

    // Update camera based on physical model and set view projection accordingly
    mSceneCamera.update();
    mActiveView->updateViewProjection(mSceneCamera.getViewProjection());

    resultFrame->frame_time = mSceneCamera.getTimestamp();

    float renderTime = 0.0f;
    if (!mUseEnvironmentScene || VirtualSceneManager::getAnimationState()) {
        renderTime = resultFrame->frame_time / 1000000000.0f;
    }

    int conversionResult = -1;
    auto onRenderComplete = [&]() {
        const std::vector<uint8_t>& fbData =
                mActiveView->getFramebufferLocked();

        uint32_t pixelFormat =
                cameraFormatFromFormat(mActiveView->getFormatLocked());

        // Do not rotate during the conversion if the view is already handling
        const bool viewHandlesRotation =
                SceneConfig::modeSupportViewRotations(mScene->getSceneMode());
        const char* convertDirection = direction;
        int convertOrientation = orientation;
        if (viewHandlesRotation) {
            convertDirection = "front";
            convertOrientation = 1;
        }
        // Convert frame to the receiving buffers.
        conversionResult = convert_frame(
                fbData.data(), pixelFormat, fbData.size(),
                mActiveView->getWidthLocked(), mActiveView->getHeightLocked(),
                resultFrame, rScale, gScale, bScale, expComp, convertDirection,
                convertOrientation);
    };

    bool renderResult = false;
    if (mUseEnvironmentScene) {
        renderResult = VirtualSceneManager::renderView(
                mActiveView.get(), renderTime, onRenderComplete);
    } else {
        renderResult = ScenesManager::renderView(
                mScene.get(), mActiveView.get(), renderTime, onRenderComplete);
    }

    if (!renderResult) {
        LOG(ERROR) << "Virtual scene could not be rendered!";
        return -1;
    }

    return conversionResult;
}

}  // namespace virtualscene
}  // namespace android
