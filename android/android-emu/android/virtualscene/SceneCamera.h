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

#pragma once

/*
 * Defines SceneCamera, which controls the position of the virtual camera in the
 * 3D scene and produces view/projection matrices based on calibration.
 */

#include "aemu/base/Compiler.h"
#include "android/utils/compiler.h"

#include <glm/glm.hpp>

namespace android {
namespace virtualscene {

class SceneCamera {
    DISALLOW_COPY_AND_ASSIGN(SceneCamera);

public:
    SceneCamera();
    ~SceneCamera();

    // Set the aspect ratio of the camera projection.
    // |aspectRatio| - w/h of the camera frame.
    void setAspectRatio(float aspectRatio);

    // Update the scene camera based on the current physical state.
    void update(bool supportsPosition);

    // Get the view projection matrix for the current frame.
    glm::mat4 getViewProjection() const;

    // Get the view matrix for the current frame.
    const glm::mat4& getView() const;

    // Get the projection matrix for the current frame.
    const glm::mat4& getProjection() const;

    // Set extra rotation to be applied on top of the physical mode rotation
    // Can be used to change camera direction.
    void setExtraRotationEulerDegrees(
            const glm::vec3& extraRotationEulerDegrees) {
        mExtraRotationEulerDegrees = extraRotationEulerDegrees;
    }

private:
    glm::mat4 mProjection = glm::mat4();
    glm::mat4 mCameraFromSensors = glm::mat4();
    glm::mat4 mViewFromWorld = glm::mat4();
    glm::vec3 mExtraRotationEulerDegrees = glm::vec3();
};

}  // namespace virtualscene
}  // namespace android
