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
#include "android/virtualscene/VirtualSceneManager.h"

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

    mUsingEnvironmentScene = false; // set later, at capture start
    mName = name;

    LOG(INFO) << "Initialized camera with name: " << name;
}

RenderedCameraDevice::~RenderedCameraDevice() {
    stopCapturing();
}

int RenderedCameraDevice::startCapturing(uint32_t pixelFormat,
                                         int frameWidth,
                                         int frameHeight) {
    VLOG(camera) << "Start capturing at " << frameWidth << " x " << frameHeight;

    SceneConfig::Mode sceneMode = SceneConfig::Mode::Unknown;

    // "environment" means the camera is using the global environment
    // scene, defined in environment.ini file
    mUsingEnvironmentScene = (mName == "environment");
    if (mUsingEnvironmentScene) {
        mOwnedScene = nullptr;
        sceneMode = VirtualSceneManager::getSceneMode();

        VirtualSceneManager::setSceneControlsParameters(true);
        VirtualSceneManager::addSceneUser();
    } else {
        // Create and own the scene
        std::string sceneModeStr;
        std::string sceneFilename;
        const size_t sepPos =
                mName.find(camera_virtualscene_name_argument_separator());
        if (sepPos != std::string::npos) {
            sceneModeStr = mName.substr(0, sepPos);
            sceneFilename = mName.substr(sepPos + 1);
        } else {
            sceneModeStr = mName;
        }
        SceneConfig::Mode mode = SceneConfig::modeFromString(sceneModeStr);
        if (sceneFilename.empty()) {
            // Create with default content if a filename is not given
            sceneFilename = SceneConfig::defaultFilenameForMode(mode);
        }
        SceneConfig sceneConfig(mode, sceneFilename);
        mOwnedScene = ScenesManager::createScene(sceneConfig);

        if (mOwnedScene) {
            sceneMode = mOwnedScene->getSceneMode();
            mOwnedScene->loadUserResources();
        }
    }

    if (sceneMode == SceneConfig::Mode::Unknown) {
        LOG(ERROR) << "Camera scene could not be not initialized!";
        stopCapturing();
        return -1;
    }

    mSceneCamera.setAspectRatio(static_cast<float>(frameWidth) / frameHeight);

    mActiveView = std::make_unique<RendererView>();
    mActiveView->updateTarget(formatFromCameraFormat(pixelFormat), frameWidth,
                              frameHeight);

    return 0;
}

// Resets camera device after capturing.
// Since new capture request may require different frame dimensions we must
// reset camera device by reopening its handle. Otherwise attempts to set up new
// frame properties (different from the previous one) may fail.
void RenderedCameraDevice::stopCapturing() {
    mActiveView.reset();

    if (mUsingEnvironmentScene) {
        VirtualSceneManager::setSceneControlsParameters(false);
        VirtualSceneManager::removeSceneUser();
        mUsingEnvironmentScene = false;
    } else if (mOwnedScene) {
        mOwnedScene->unloadUserResources();
        ScenesManager::removeScene(mOwnedScene.get());
        mOwnedScene.reset();
    }
}

int RenderedCameraDevice::readFrame(ClientFrame* resultFrame,
                                    float rScale,
                                    float gScale,
                                    float bScale,
                                    float expComp,
                                    const char* direction,
                                    int orientation) {

    SceneConfig::Mode sceneMode = SceneConfig::Mode::Unknown;
    if (mUsingEnvironmentScene) {
        sceneMode = VirtualSceneManager::getSceneMode();
    } else if (mOwnedScene) {
        sceneMode = mOwnedScene->getSceneMode();
    }

    if (sceneMode == SceneConfig::Mode::Unknown) {
        LOG(ERROR) << "Virtual scene is not initialized!";
        return -1;
    }
    if (!mUsingEnvironmentScene) {
        if (!mOwnedScene) {
            LOG(ERROR) << "Virtual scene is not initialized!";
            return -1;
        }
        mOwnedScene->update();
    }

    // TODO(virtualscene-perf): update the view here to avoid resizing?
    // Update camera based on physical model and set view projection accordingly
    mSceneCamera.update();
    mActiveView->updateViewProjection(mSceneCamera.getViewProjection());

    int conversionResult = -1;
    auto onRenderComplete = [&]() {
        const std::vector<uint8_t>& fbData =
                mActiveView->getFramebufferLocked();

        uint32_t pixelFormat =
                cameraFormatFromFormat(mActiveView->getFormatLocked());

        // Do not rotate during the conversion if the view is already handling
        const bool viewHandlesRotation =
                SceneConfig::modeSupportViewRotations(sceneMode);
        const char* convertDirection = direction;
        int convertOrientation = orientation;
        if (viewHandlesRotation) {
            convertDirection = "front";
            convertOrientation = 1;
        } else {
            int rotation = 0;
            if (mUsingEnvironmentScene) {
                rotation = VirtualSceneManager::getSceneBaseRotationLocked();
            } else {
                rotation = mOwnedScene->getSceneRotation();
            }
            if (rotation) {
                // Apply the required base rotation to the image
                convertOrientation += rotation / 90;
                convertOrientation %= 4;
                if (convertOrientation < 0) {
                    convertOrientation += 4;
                }
            }
        }
        // Convert frame to the receiving buffers.
        conversionResult = convert_frame(
                fbData.data(), pixelFormat, fbData.size(),
                mActiveView->getWidthLocked(), mActiveView->getHeightLocked(),
                resultFrame, rScale, gScale, bScale, expComp, convertDirection,
                convertOrientation);
    };

    uint64_t frameTime = 0;
    bool renderResult = false;
    if (mUsingEnvironmentScene) {
        renderResult = VirtualSceneManager::renderView(
                mActiveView.get(), onRenderComplete, &frameTime);
    } else {
        renderResult =
                ScenesManager::renderView(mOwnedScene.get(), mActiveView.get(),
                                          onRenderComplete, &frameTime);
    }

    if (!renderResult) {
        LOG(ERROR) << "Virtual scene could not be rendered!";
        return -1;
    }

    // Set the frame time used in the render
    resultFrame->frame_time = static_cast<int64_t>(frameTime);

    return conversionResult;
}

}  // namespace virtualscene
}  // namespace android
