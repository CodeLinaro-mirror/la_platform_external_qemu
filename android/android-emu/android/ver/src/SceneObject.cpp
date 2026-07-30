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

#include "SceneObject.h"

#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"

#define E(...) derror(__VA_ARGS__)
#define W(...) dwarning(__VA_ARGS__)
#define D(...) dprint(__VA_ARGS__)

namespace android {
namespace ver {

SceneObject::SceneObject(Renderer& renderer) : mRenderer(renderer) {}

SceneObject::~SceneObject() {
    for (Renderable& renderable : mRenderables) {
        mRenderer.releaseMaterial(renderable.material);
        mRenderer.releaseMesh(renderable.mesh);
        mRenderer.releaseTexture(renderable.texture);
    }
}

void SceneObject::setTransform(const glm::mat4& transform) {
    mTransform = transform;
}

glm::mat4 SceneObject::getTransform() const {
    return mTransform;
}

const std::vector<Renderable>& SceneObject::getRenderables() const {
    return mRenderables;
}

bool SceneObject::isVisible() const {
    return mVisible;
}

void SceneObject::setTexture(int renderableIndex, Texture texture) {
    if (renderableIndex < 0 || renderableIndex >= mRenderables.size()) {
        E("%s: invalid parameters", __func__);
        return;
    }

    mRenderer.releaseTexture(mRenderables[renderableIndex].texture);
    mRenderables[renderableIndex].texture = mRenderer.duplicateTexture(texture);
}

bool SceneObject::getBoundingBox(glm::vec3* outMin, glm::vec3* outMax) const {
    if (!mHasBounds || !outMin || !outMax) {
        return false;
    }
    glm::vec3 corners[8] = {
        {mMinBounds.x, mMinBounds.y, mMinBounds.z},
        {mMaxBounds.x, mMinBounds.y, mMinBounds.z},
        {mMinBounds.x, mMaxBounds.y, mMinBounds.z},
        {mMaxBounds.x, mMaxBounds.y, mMinBounds.z},
        {mMinBounds.x, mMinBounds.y, mMaxBounds.z},
        {mMaxBounds.x, mMinBounds.y, mMaxBounds.z},
        {mMinBounds.x, mMaxBounds.y, mMaxBounds.z},
        {mMaxBounds.x, mMaxBounds.y, mMaxBounds.z},
    };
    glm::vec3 minP(std::numeric_limits<float>::max());
    glm::vec3 maxP(std::numeric_limits<float>::lowest());
    for (int i = 0; i < 8; ++i) {
        glm::vec4 transformed = mTransform * glm::vec4(corners[i], 1.0f);
        minP = glm::min(minP, glm::vec3(transformed));
        maxP = glm::max(maxP, glm::vec3(transformed));
    }
    *outMin = minP;
    *outMax = maxP;
    return true;
}

}  // namespace ver
}  // namespace android
