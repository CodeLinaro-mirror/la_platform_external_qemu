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

#pragma once

#include <memory>

#include "aemu/base/Log.h"
#include "aemu/base/memory/LazyInstance.h"
#include "android/camera/camera-format-converters.h"
#include "android/virtualscene/SceneCamera.h"
#include "android/virtualscene/VirtualSceneManager.h"
#include "emugl/common/OpenGLDispatchLoader.h"
#include "host-common/opengles.h"

namespace android {
namespace virtualscene {

class RendererView;

/*******************************************************************************
 *                     RenderedCameraDevice routines
 ******************************************************************************/

/*
 * Describes a connection to an actual camera device.
 */
class RenderedCameraDevice {
    DISALLOW_COPY_AND_ASSIGN(RenderedCameraDevice);

public:
    RenderedCameraDevice(std::string_view name);
    ~RenderedCameraDevice();

    CameraDevice* getCameraDevice() { return &mHeader; }

    int startCapturing(uint32_t pixelFormat, int frameWidth, int frameHeight);
    void stopCapturing();

    int readFrame(ClientFrame* resultFrame,
                  float rScale,
                  float gScale,
                  float bScale,
                  float expComp,
                  const char* direction,
                  int orientation);

private:
    // Common camera header.
    CameraDevice mHeader;

    SceneCamera mSceneCamera;
    std::unique_ptr<RendererView> mActiveView;
    bool mUseEnvironmentScene;
};

}  // namespace virtualscene
}  // namespace android
