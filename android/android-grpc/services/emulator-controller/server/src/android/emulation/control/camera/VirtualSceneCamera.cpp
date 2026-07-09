
// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "android/emulation/control/camera/VirtualSceneCamera.h"

#include "aemu/base/memory/LazyInstance.h"  // for LazyInstance
#include "android/camera/camera-common.h"   // for kEnvironment
#include "android/camera/camera-service.h"  // for register_ca...
#include "android/physics/GlmHelpers.h"
#include "android/physics/PhysicalModel.h"
#include "android/virtualscene/VirtualSceneManager.h"
#include "android/virtualscene/WASDInputHandler.h"

using android::virtualscene::kMaxVerticalRotationDegrees;
using android::virtualscene::kMinVerticalRotationDegrees;
using android::virtualscene::kPixelsToRotationRadians;
using android::virtualscene::VirtualSceneManager;

namespace android {
namespace emulation {
namespace control {

static const float kAmbientMotionExtentMeters = 0.005f;

// mVirtualSceneConnected will be updated in three scenarios:
// 1, snapshot load 2, virtual scene camera connected 3, virtual scene camera
// disconnected.
VirtualSceneCamera::VirtualSceneCamera(const QAndroidSensorsAgent* sensorsAgent)
    : mModel(sensorsAgent), mConnected(false) {
    VirtualSceneManager::setSceneControlsChangeCallback([this](bool connected) {
        this->virtualSceneControlsChangeCallback(connected);
    });
}

VirtualSceneCamera::~VirtualSceneCamera() {
    // Reset the callback as it's not safe to run it anymore
    VirtualSceneManager::setSceneControlsChangeCallback(nullptr);
}

void VirtualSceneCamera::virtualSceneControlsChangeCallback(bool connected) {
    if (mConnected == connected) {
        return;
    }
    mConnected = connected;
    mEventWaiter.newEvent();
    fireEvent(connected);
    if (connected) {
        mModel.onEnable();
    } else {
        mModel.onDisable();
    }
}

bool VirtualSceneCamera::rotate(const glm::vec3 delta) {
    if (!mConnected)
        return false;

    mModel.updateRotation({delta.x, delta.y, 0.0f});
    return setVelocity(mBaseVelocity);
}

bool VirtualSceneCamera::setVelocity(const glm::vec3 absolute) {
    if (!mConnected)
        return false;
    if (mBaseVelocity != absolute)
        mBaseVelocity = absolute;

    mModel.updateVelocity(absolute);
    return true;
}

EventWaiter* VirtualSceneCamera::eventWaiter() {
    return &mEventWaiter;
}

}  // namespace control
}  // namespace emulation
}  // namespace android
