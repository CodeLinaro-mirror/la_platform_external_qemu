// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

/*
 * Defines MeshSceneObject, which represents a SceneObject loaded from an .obj
 * file.
 */

#include "SceneObject.h"

namespace android {
namespace ver {

class MeshSceneObject : public SceneObject {
    MeshSceneObject(const MeshSceneObject& other) = delete;
    MeshSceneObject& operator=(const MeshSceneObject& other) = delete;

protected:
    MeshSceneObject(Renderer& renderer);

public:
    // Loads an object mesh from an .obj or .gltf/.glb file.
    //
    // |renderer| - Renderer context.
    // |filename| - Filename to load.
    //
    // Returns a MeshSceneObject instance if the object could be loaded or null
    // if there was an error.
    static std::unique_ptr<MeshSceneObject> load(Renderer& renderer,
                                                 const char* filename);

    // Checks if the .obj or .gltf/.glb file can be loaded, without using a renderer
    //
    // |filename| - Filename to load.
    //
    // Returns true if the file can be loaded.
    static bool canLoad(const char* filename);

    // Creates a unit sphere mesh object
    //
    // |renderer| - Renderer context.
    //
    // Returns a MeshSceneObject instance of the sphere object
    static std::unique_ptr<MeshSceneObject> createSphere(Renderer& renderer);

    void setAnimationTime(float timeSec) override;

private:
    struct GltfNodeData {
        std::string name;
        int parent = -1;
        std::vector<int> children;
        glm::vec3 translation = glm::vec3(0.0f);
        glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale = glm::vec3(1.0f);
        glm::mat4 matrix = glm::mat4(1.0f);
        bool hasMatrix = false;
    };

    struct GltfAnimChannel {
        int targetNode = -1;
        std::string targetPath;
        std::vector<float> keyTimes;
        std::vector<glm::vec3> keyTranslations;
        std::vector<glm::quat> keyRotations;
        std::vector<glm::vec3> keyScales;
    };

    struct GltfAnimData {
        float minTime = 0.0f;
        float maxTime = 0.0f;
        std::vector<GltfAnimChannel> channels;
    };

    struct GltfSkinData {
        std::vector<int> joints;
        std::vector<glm::mat4> inverseBindMatrices;
    };

    struct GltfSkinnedPrimitive {
        size_t renderableIndex = 0;
        std::vector<VertexPositionUV> baseVertices;
        std::vector<glm::uvec4> joints;
        std::vector<glm::vec4> weights;
        int nodeIdx = -1;
        int skinIdx = -1;
    };

    std::vector<GltfNodeData> mGltfNodes;
    std::vector<GltfSkinData> mGltfSkins;
    std::vector<GltfAnimData> mGltfAnimations;
    std::vector<GltfSkinnedPrimitive> mGltfSkinnedPrimitives;

    static bool canLoadObj(const char* filename);
    static bool canLoadGltf(const char* filename);
    static std::unique_ptr<MeshSceneObject> loadObj(Renderer& renderer,
                                                    const char* filename);
    static std::unique_ptr<MeshSceneObject> loadGltf(Renderer& renderer,
                                                     const char* filename);
};

}  // namespace ver
}  // namespace android
