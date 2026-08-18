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

#include "MeshSceneObject.h"

#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"

#include <tiny_obj_loader.h>

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include "third_party/tinygltf/tiny_gltf.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <unordered_map>

#define E(...) derror(__VA_ARGS__)
#define W(...) dwarning(__VA_ARGS__)
#define D(...) dprint(__VA_ARGS__)

using android::base::PathUtils;
using android::base::System;
namespace fs = std::filesystem;

namespace android {
namespace ver {

namespace {
bool isGltfFile(const char* filename) {
    if (!filename) {
        return false;
    }
    std::string ext = fs::path(filename).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext == ".gltf" || ext == ".glb";
}
}  // namespace

MeshSceneObject::MeshSceneObject(Renderer& renderer) : SceneObject(renderer) {}

bool MeshSceneObject::canLoadObj(const char* filename) {
    const fs::path filePath(filename);
    const std::string resourcesDir =
            PathUtils::addTrailingDirSeparator(filePath.parent_path().string());

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;

    std::string err;
    const bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &err,
                                      filename, resourcesDir.c_str());
    if (!ret) {
        E("%s: Error loading obj %s: %s", __FUNCTION__, filename,
          err.empty() ? "<no message>" : err.c_str());
        return false;
    } else if (!err.empty()) {
        W("%s: Warnings loading obj %s: %s", __FUNCTION__, filename,
          err.c_str());
    }

    const size_t vertexCount = attrib.vertices.size() / 3;
    const size_t texcoordCount = attrib.texcoords.size() / 2;

    for (const tinyobj::shape_t& shape : shapes) {
        const tinyobj::mesh_t& mesh = shape.mesh;

        for (size_t i = 0; i < mesh.indices.size(); i++) {
            tinyobj::index_t index = mesh.indices[i];

            if (index.vertex_index < 0 || index.vertex_index >= vertexCount) {
                E("%s: Error parsing %s, invalid vertex index %d, expected "
                  "less than %d",
                  __FUNCTION__, filename, index.vertex_index, vertexCount);
                return false;
            }

            if (index.texcoord_index >= 0) {
                if (index.texcoord_index >= texcoordCount) {
                    E("%s: Error parsing %s, invalid texture coordinate index %d, expected "
                      "less than %d",
                      __FUNCTION__, filename, index.texcoord_index,
                      texcoordCount);
                    return false;
                }
            }
        }
    }

    return true;
}

bool MeshSceneObject::canLoadGltf(const char* filename) {
    if (!filename) {
        return false;
    }
    const fs::path filePath(filename);
    if (!fs::exists(filePath)) {
        return false;
    }

    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err;
    std::string warn;
    bool ret = false;

    std::string ext = filePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (ext == ".glb") {
        ret = loader.LoadBinaryFromFile(&model, &err, &warn, filename);
    } else {
        ret = loader.LoadASCIIFromFile(&model, &err, &warn, filename);
    }

    if (!ret) {
        E("%s: Error loading glTF %s: %s", __FUNCTION__, filename,
          err.empty() ? "<no message>" : err.c_str());
        return false;
    }

    if (model.meshes.empty()) {
        E("%s: No meshes found in glTF %s", __FUNCTION__, filename);
        return false;
    }

    for (const tinygltf::Mesh& mesh : model.meshes) {
        for (const tinygltf::Primitive& primitive : mesh.primitives) {
            auto posIt = primitive.attributes.find("POSITION");
            if (posIt == primitive.attributes.end()) {
                continue;
            }
            int posAccessorIdx = posIt->second;
            if (posAccessorIdx < 0 ||
                posAccessorIdx >= static_cast<int>(model.accessors.size())) {
                E("%s: Invalid position accessor index in glTF %s",
                  __FUNCTION__, filename);
                return false;
            }
            if (primitive.indices >= 0 &&
                primitive.indices >= static_cast<int>(model.accessors.size())) {
                E("%s: Invalid index accessor in glTF %s", __FUNCTION__,
                  filename);
                return false;
            }
        }
    }

    return true;
}

bool MeshSceneObject::canLoad(const char* filename) {
    if (!filename) {
        E("%s: Invalid input", __FUNCTION__);
        return false;
    }
    if (isGltfFile(filename)) {
        return canLoadGltf(filename);
    } else {
        return canLoadObj(filename);
    }
}

std::unique_ptr<MeshSceneObject> MeshSceneObject::loadObj(Renderer& renderer,
                                                          const char* filename) {
    const fs::path filePath(filename);
    const std::string resourcesDir =
            PathUtils::addTrailingDirSeparator(filePath.parent_path().string());

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;

    std::string err;
    const bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &err,
                                      filename, resourcesDir.c_str());
    if (!ret) {
        E("%s: Error loading obj %s: %s", __FUNCTION__, filename,
          err.empty() ? "<no message>" : err.c_str());
        return nullptr;
    } else if (!err.empty()) {
        W("%s: Warnings loading obj %s: %s", __FUNCTION__, filename,
          err.c_str());
    }

    std::unique_ptr<MeshSceneObject> result(new MeshSceneObject(renderer));

    const size_t vertexCount = attrib.vertices.size() / 3;
    const size_t texcoordCount = attrib.texcoords.size() / 2;

    for (const tinyobj::shape_t& shape : shapes) {
        const tinyobj::mesh_t& mesh = shape.mesh;

        std::vector<VertexPositionUV> vertices;
        std::unordered_map<VertexPositionUV, uint32_t, VertexPositionUVHash>
                existingVertexToIndex;
        std::vector<uint32_t> indices;
        Texture texture;

        bool useCheckerboardMaterial = false;

        if (!mesh.material_ids.empty()) {
            const int material_id = mesh.material_ids[0];
            if (material_id >= 0 && material_id < materials.size()) {
                if (strstr(materials[material_id].diffuse_texname.c_str(),
                           "TV")) {
                    useCheckerboardMaterial = true;
                } else {
                    texture = renderer.loadTexture(
                            materials[material_id].diffuse_texname.c_str());
                }
            }
        }

        for (size_t i = 0; i < mesh.indices.size(); i++) {
            tinyobj::index_t index = mesh.indices[i];
            VertexPositionUV vertex;

            if (index.vertex_index < 0 || index.vertex_index >= vertexCount) {
                E("%s: Error parsing %s, invalid vertex index %d, expected "
                  "less than %d",
                  __FUNCTION__, filename, index.vertex_index, vertexCount);
                return nullptr;
            }

            vertex.pos = glm::vec3(attrib.vertices[3 * index.vertex_index],
                                   attrib.vertices[3 * index.vertex_index + 1],
                                   attrib.vertices[3 * index.vertex_index + 2]);

            if (index.texcoord_index >= 0) {
                if (index.texcoord_index >= texcoordCount) {
                    E("%s: Error parsing %s, invalid texture coordinate index %d, expected "
                      "less than %d",
                      __FUNCTION__, filename, index.texcoord_index,
                      texcoordCount);
                    return nullptr;
                }

                vertex.uv = glm::vec2(
                        attrib.texcoords[2 * index.texcoord_index],
                        attrib.texcoords[2 * index.texcoord_index + 1]);
            }

            auto existingEntry = existingVertexToIndex.find(vertex);
            if (existingEntry != existingVertexToIndex.end()) {
                indices.push_back(existingEntry->second);
            } else {
                vertices.push_back(vertex);

                const uint32_t index = vertices.size() - 1;
                indices.push_back(index);

                existingVertexToIndex[vertex] = index;
            }
        }

        D("%s: Creating mesh with %d vertices, %d indices", __FUNCTION__,
          vertices.size(), indices.size());

        glm::vec3 minP(std::numeric_limits<float>::max());
        glm::vec3 maxP(std::numeric_limits<float>::lowest());
        bool hasVerts = false;
        for (const auto& v : vertices) {
            minP = glm::min(minP, v.pos);
            maxP = glm::max(maxP, v.pos);
            hasVerts = true;
        }
        if (hasVerts) {
            result->mMinBounds = glm::min(result->mMinBounds, minP);
            result->mMaxBounds = glm::max(result->mMaxBounds, maxP);
            result->mHasBounds = true;
        }

        Renderable renderable;
        renderable.material = useCheckerboardMaterial
                                      ? renderer.createMaterialCheckerboard()
                                      : renderer.createMaterialTextured();
        renderable.mesh = renderer.createMesh(vertices, indices);
        renderable.texture = texture;

        result->mRenderables.emplace_back(std::move(renderable));
    }

    return result;
}

std::unique_ptr<MeshSceneObject> MeshSceneObject::loadGltf(Renderer& renderer,
                                                          const char* filename) {
    const fs::path filePath(filename);
    const std::string resourcesDir =
            PathUtils::addTrailingDirSeparator(filePath.parent_path().string());

    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err;
    std::string warn;
    bool ret = false;

    std::string ext = filePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (ext == ".glb") {
        ret = loader.LoadBinaryFromFile(&model, &err, &warn, filename);
    } else {
        ret = loader.LoadASCIIFromFile(&model, &err, &warn, filename);
    }

    if (!ret) {
        E("%s: Error loading glTF %s: %s", __FUNCTION__, filename,
          err.empty() ? "<no message>" : err.c_str());
        return nullptr;
    } else if (!warn.empty()) {
        W("%s: Warnings loading glTF %s: %s", __FUNCTION__, filename,
          warn.c_str());
    }

    std::unique_ptr<MeshSceneObject> result(new MeshSceneObject(renderer));

    // 1. Parse Nodes
    result->mGltfNodes.resize(model.nodes.size());
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const auto& n = model.nodes[i];
        auto& nodeData = result->mGltfNodes[i];
        nodeData.name = n.name;
        nodeData.children = n.children;
        if (n.matrix.size() == 16) {
            nodeData.hasMatrix = true;
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    nodeData.matrix[col][row] = static_cast<float>(n.matrix[col * 4 + row]);
                }
            }
        } else {
            nodeData.hasMatrix = false;
            if (n.translation.size() == 3) {
                nodeData.translation = glm::vec3(n.translation[0], n.translation[1], n.translation[2]);
            }
            if (n.rotation.size() == 4) {
                nodeData.rotation = glm::quat(static_cast<float>(n.rotation[3]),
                                             static_cast<float>(n.rotation[0]),
                                             static_cast<float>(n.rotation[1]),
                                             static_cast<float>(n.rotation[2]));
            }
            if (n.scale.size() == 3) {
                nodeData.scale = glm::vec3(n.scale[0], n.scale[1], n.scale[2]);
            }
        }
    }
    for (size_t i = 0; i < result->mGltfNodes.size(); ++i) {
        for (int c : result->mGltfNodes[i].children) {
            if (c >= 0 && c < static_cast<int>(result->mGltfNodes.size())) {
                result->mGltfNodes[c].parent = static_cast<int>(i);
            }
        }
    }

    // 2. Parse Skins
    for (const auto& skin : model.skins) {
        GltfSkinData skinData;
        skinData.joints = skin.joints;
        if (skin.inverseBindMatrices >= 0 &&
            skin.inverseBindMatrices < static_cast<int>(model.accessors.size())) {
            const auto& accessor = model.accessors[skin.inverseBindMatrices];
            if (accessor.bufferView >= 0 &&
                accessor.bufferView < static_cast<int>(model.bufferViews.size())) {
                const auto& bufferView = model.bufferViews[accessor.bufferView];
                if (bufferView.buffer >= 0 &&
                    bufferView.buffer < static_cast<int>(model.buffers.size())) {
                    const uint8_t* ptr = model.buffers[bufferView.buffer].data.data() +
                                         bufferView.byteOffset + accessor.byteOffset;
                    skinData.inverseBindMatrices.resize(accessor.count);
                    std::memcpy(skinData.inverseBindMatrices.data(), ptr, accessor.count * sizeof(glm::mat4));
                }
            }
        }
        result->mGltfSkins.push_back(std::move(skinData));
    }

    // 3. Parse Animations
    for (const auto& anim : model.animations) {
        GltfAnimData animData;
        float minT = std::numeric_limits<float>::max();
        float maxT = std::numeric_limits<float>::lowest();

        for (const auto& channel : anim.channels) {
            if (channel.target_node < 0 ||
                channel.target_node >= static_cast<int>(model.nodes.size()) ||
                channel.sampler < 0 ||
                channel.sampler >= static_cast<int>(anim.samplers.size())) {
                continue;
            }
            const auto& sampler = anim.samplers[channel.sampler];
            if (sampler.input < 0 || sampler.input >= static_cast<int>(model.accessors.size()) ||
                sampler.output < 0 || sampler.output >= static_cast<int>(model.accessors.size())) {
                continue;
            }

            const auto& timeAcc = model.accessors[sampler.input];
            const auto& valAcc = model.accessors[sampler.output];

            GltfAnimChannel animChan;
            animChan.targetNode = channel.target_node;
            animChan.targetPath = channel.target_path;

            // Read key times
            if (timeAcc.bufferView >= 0 && timeAcc.bufferView < static_cast<int>(model.bufferViews.size())) {
                const auto& bv = model.bufferViews[timeAcc.bufferView];
                const uint8_t* ptr = model.buffers[bv.buffer].data.data() + bv.byteOffset + timeAcc.byteOffset;
                animChan.keyTimes.resize(timeAcc.count);
                const float* fPtr = reinterpret_cast<const float*>(ptr);
                for (size_t k = 0; k < timeAcc.count; ++k) {
                    animChan.keyTimes[k] = fPtr[k];
                    minT = std::min(minT, fPtr[k]);
                    maxT = std::max(maxT, fPtr[k]);
                }
            }

            // Read key values
            if (valAcc.bufferView >= 0 && valAcc.bufferView < static_cast<int>(model.bufferViews.size())) {
                const auto& bv = model.bufferViews[valAcc.bufferView];
                const uint8_t* ptr = model.buffers[bv.buffer].data.data() + bv.byteOffset + valAcc.byteOffset;
                if (channel.target_path == "translation") {
                    animChan.keyTranslations.resize(valAcc.count);
                    const float* fPtr = reinterpret_cast<const float*>(ptr);
                    for (size_t k = 0; k < valAcc.count; ++k) {
                        animChan.keyTranslations[k] = glm::vec3(fPtr[k * 3], fPtr[k * 3 + 1], fPtr[k * 3 + 2]);
                    }
                } else if (channel.target_path == "rotation") {
                    animChan.keyRotations.resize(valAcc.count);
                    const float* fPtr = reinterpret_cast<const float*>(ptr);
                    for (size_t k = 0; k < valAcc.count; ++k) {
                        animChan.keyRotations[k] = glm::quat(fPtr[k * 4 + 3], fPtr[k * 4 + 0], fPtr[k * 4 + 1], fPtr[k * 4 + 2]);
                    }
                } else if (channel.target_path == "scale") {
                    animChan.keyScales.resize(valAcc.count);
                    const float* fPtr = reinterpret_cast<const float*>(ptr);
                    for (size_t k = 0; k < valAcc.count; ++k) {
                        animChan.keyScales[k] = glm::vec3(fPtr[k * 3], fPtr[k * 3 + 1], fPtr[k * 3 + 2]);
                    }
                }
            }
            animData.channels.push_back(std::move(animChan));
        }

        if (minT <= maxT) {
            animData.minTime = minT;
            animData.maxTime = maxT;
        }
        result->mGltfAnimations.push_back(std::move(animData));
    }

    auto processPrimitive = [&](const tinygltf::Primitive& primitive,
                                const glm::mat4& transform,
                                int nodeIdx) {
        auto posIt = primitive.attributes.find("POSITION");
        if (posIt == primitive.attributes.end()) {
            return;
        }
        int posAccessorIdx = posIt->second;
        if (posAccessorIdx < 0 ||
            posAccessorIdx >= static_cast<int>(model.accessors.size())) {
            E("%s: Invalid position accessor index in glTF %s", __FUNCTION__,
              filename);
            return;
        }

        const tinygltf::Accessor& posAccessor = model.accessors[posAccessorIdx];
        if (posAccessor.bufferView < 0 ||
            posAccessor.bufferView >= static_cast<int>(model.bufferViews.size())) {
            E("%s: Invalid position buffer view index in glTF %s", __FUNCTION__,
              filename);
            return;
        }
        const tinygltf::BufferView& posBufferView =
                model.bufferViews[posAccessor.bufferView];
        if (posBufferView.buffer < 0 ||
            posBufferView.buffer >= static_cast<int>(model.buffers.size())) {
            E("%s: Invalid position buffer index in glTF %s", __FUNCTION__,
              filename);
            return;
        }
        const tinygltf::Buffer& posBuffer = model.buffers[posBufferView.buffer];

        const uint8_t* posBytePtr = posBuffer.data.data() +
                                    posBufferView.byteOffset +
                                    posAccessor.byteOffset;
        size_t posStride = posBufferView.byteStride != 0
                                   ? posBufferView.byteStride
                                   : (sizeof(float) * 3);

        size_t vertexCount = posAccessor.count;

        bool hasUV = false;
        const uint8_t* uvBytePtr = nullptr;
        size_t uvStride = 0;
        int uvComponentType = -1;

        auto uvIt = primitive.attributes.find("TEXCOORD_0");
        if (uvIt != primitive.attributes.end()) {
            int uvAccessorIdx = uvIt->second;
            if (uvAccessorIdx >= 0 &&
                uvAccessorIdx < static_cast<int>(model.accessors.size())) {
                const tinygltf::Accessor& uvAccessor =
                        model.accessors[uvAccessorIdx];
                if (uvAccessor.bufferView >= 0 &&
                    uvAccessor.bufferView <
                            static_cast<int>(model.bufferViews.size())) {
                    const tinygltf::BufferView& uvBufferView =
                            model.bufferViews[uvAccessor.bufferView];
                    if (uvBufferView.buffer >= 0 &&
                        uvBufferView.buffer <
                                static_cast<int>(model.buffers.size())) {
                        const tinygltf::Buffer& uvBuffer =
                                model.buffers[uvBufferView.buffer];
                        uvBytePtr = uvBuffer.data.data() +
                                    uvBufferView.byteOffset +
                                    uvAccessor.byteOffset;
                        uvComponentType = uvAccessor.componentType;
                        int compSize = tinygltf::GetComponentSizeInBytes(
                                uvComponentType);
                        int numComp = tinygltf::GetNumComponentsInType(
                                uvAccessor.type);
                        uvStride = uvBufferView.byteStride != 0
                                           ? uvBufferView.byteStride
                                           : (compSize * numComp);
                        hasUV = true;
                    }
                }
            }
        }

        std::vector<VertexPositionUV> vertices;
        vertices.reserve(vertexCount);

        for (size_t i = 0; i < vertexCount; ++i) {
            VertexPositionUV vertex;

            const float* p = reinterpret_cast<const float*>(posBytePtr + i * posStride);
            glm::vec4 localPos(p[0], p[1], p[2], 1.0f);
            vertex.pos = glm::vec3(transform * localPos);

            if (hasUV && uvBytePtr) {
                const uint8_t* uvPtr = uvBytePtr + i * uvStride;
                if (uvComponentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                    const float* uvf = reinterpret_cast<const float*>(uvPtr);
                    vertex.uv = glm::vec2(uvf[0], 1.0f - uvf[1]);
                } else if (uvComponentType ==
                           TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                    vertex.uv = glm::vec2(uvPtr[0] / 255.0f,
                                          1.0f - (uvPtr[1] / 255.0f));
                } else if (uvComponentType ==
                           TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                    const uint16_t* uvs =
                            reinterpret_cast<const uint16_t*>(uvPtr);
                    vertex.uv = glm::vec2(uvs[0] / 65535.0f,
                                          1.0f - (uvs[1] / 65535.0f));
                } else {
                    vertex.uv = glm::vec2(0.0f, 0.0f);
                }
            } else {
                vertex.uv = glm::vec2(0.0f, 0.0f);
            }

            vertices.push_back(vertex);
        }

        bool hasJoints = false;
        const uint8_t* jointsBytePtr = nullptr;
        size_t jointsStride = 0;
        int jointsComponentType = -1;

        auto jointsIt = primitive.attributes.find("JOINTS_0");
        if (jointsIt != primitive.attributes.end()) {
            int jointsAccessorIdx = jointsIt->second;
            if (jointsAccessorIdx >= 0 &&
                jointsAccessorIdx < static_cast<int>(model.accessors.size())) {
                const tinygltf::Accessor& accessor = model.accessors[jointsAccessorIdx];
                if (accessor.bufferView >= 0 &&
                    accessor.bufferView < static_cast<int>(model.bufferViews.size())) {
                    const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
                    if (bufferView.buffer >= 0 &&
                        bufferView.buffer < static_cast<int>(model.buffers.size())) {
                        jointsBytePtr = model.buffers[bufferView.buffer].data.data() +
                                        bufferView.byteOffset + accessor.byteOffset;
                        jointsComponentType = accessor.componentType;
                        int compSize = tinygltf::GetComponentSizeInBytes(jointsComponentType);
                        int numComp = tinygltf::GetNumComponentsInType(accessor.type);
                        jointsStride = bufferView.byteStride != 0 ? bufferView.byteStride : (compSize * numComp);
                        hasJoints = true;
                    }
                }
            }
        }

        bool hasWeights = false;
        const uint8_t* weightsBytePtr = nullptr;
        size_t weightsStride = 0;
        int weightsComponentType = -1;

        auto weightsIt = primitive.attributes.find("WEIGHTS_0");
        if (weightsIt != primitive.attributes.end()) {
            int weightsAccessorIdx = weightsIt->second;
            if (weightsAccessorIdx >= 0 &&
                weightsAccessorIdx < static_cast<int>(model.accessors.size())) {
                const tinygltf::Accessor& accessor = model.accessors[weightsAccessorIdx];
                if (accessor.bufferView >= 0 &&
                    accessor.bufferView < static_cast<int>(model.bufferViews.size())) {
                    const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
                    if (bufferView.buffer >= 0 &&
                        bufferView.buffer < static_cast<int>(model.buffers.size())) {
                        weightsBytePtr = model.buffers[bufferView.buffer].data.data() +
                                         bufferView.byteOffset + accessor.byteOffset;
                        weightsComponentType = accessor.componentType;
                        int compSize = tinygltf::GetComponentSizeInBytes(weightsComponentType);
                        int numComp = tinygltf::GetNumComponentsInType(accessor.type);
                        weightsStride = bufferView.byteStride != 0 ? bufferView.byteStride : (compSize * numComp);
                        hasWeights = true;
                    }
                }
            }
        }

        std::vector<VertexPositionUV> unskinnedBaseVertices;
        std::vector<glm::uvec4> vertexJoints;
        std::vector<glm::vec4> vertexWeights;
        if (hasJoints && hasWeights) {
            unskinnedBaseVertices.reserve(vertexCount);
            vertexJoints.reserve(vertexCount);
            vertexWeights.reserve(vertexCount);

            for (size_t i = 0; i < vertexCount; ++i) {
                VertexPositionUV baseVert = vertices[i];
                const float* p = reinterpret_cast<const float*>(posBytePtr + i * posStride);
                baseVert.pos = glm::vec3(p[0], p[1], p[2]);
                unskinnedBaseVertices.push_back(baseVert);

                const uint8_t* jPtr = jointsBytePtr + i * jointsStride;
                glm::uvec4 j(0);
                if (jointsComponentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                    const uint16_t* ptr = reinterpret_cast<const uint16_t*>(jPtr);
                    j = glm::uvec4(ptr[0], ptr[1], ptr[2], ptr[3]);
                } else if (jointsComponentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                    j = glm::uvec4(jPtr[0], jPtr[1], jPtr[2], jPtr[3]);
                }
                vertexJoints.push_back(j);

                const uint8_t* wPtr = weightsBytePtr + i * weightsStride;
                glm::vec4 w(0.0f);
                if (weightsComponentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                    const float* ptr = reinterpret_cast<const float*>(wPtr);
                    w = glm::vec4(ptr[0], ptr[1], ptr[2], ptr[3]);
                } else if (weightsComponentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                    w = glm::vec4(wPtr[0] / 255.0f, wPtr[1] / 255.0f, wPtr[2] / 255.0f, wPtr[3] / 255.0f);
                } else if (weightsComponentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                    const uint16_t* ptr = reinterpret_cast<const uint16_t*>(wPtr);
                    w = glm::vec4(ptr[0] / 65535.0f, ptr[1] / 65535.0f, ptr[2] / 65535.0f, ptr[3] / 65535.0f);
                }
                vertexWeights.push_back(w);
            }
        }

        std::vector<uint32_t> indices;
        if (primitive.indices >= 0 &&
            primitive.indices < static_cast<int>(model.accessors.size())) {
            const tinygltf::Accessor& indexAccessor =
                    model.accessors[primitive.indices];
            if (indexAccessor.bufferView >= 0 &&
                indexAccessor.bufferView <
                        static_cast<int>(model.bufferViews.size())) {
                const tinygltf::BufferView& indexBufferView =
                        model.bufferViews[indexAccessor.bufferView];
                if (indexBufferView.buffer >= 0 &&
                    indexBufferView.buffer <
                            static_cast<int>(model.buffers.size())) {
                    const tinygltf::Buffer& indexBuffer =
                            model.buffers[indexBufferView.buffer];
                    const uint8_t* indexBytePtr =
                            indexBuffer.data.data() +
                            indexBufferView.byteOffset + indexAccessor.byteOffset;
                    size_t indexStride =
                            indexBufferView.byteStride != 0
                                    ? indexBufferView.byteStride
                                    : tinygltf::GetComponentSizeInBytes(
                                              indexAccessor.componentType);

                    indices.reserve(indexAccessor.count);
                    for (size_t j = 0; j < indexAccessor.count; ++j) {
                        uint32_t idx = 0;
                        const uint8_t* ptr = indexBytePtr + j * indexStride;
                        if (indexAccessor.componentType ==
                            TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                            idx = *reinterpret_cast<const uint16_t*>(ptr);
                        } else if (indexAccessor.componentType ==
                                   TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                            idx = *reinterpret_cast<const uint32_t*>(ptr);
                        } else if (indexAccessor.componentType ==
                                   TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                            idx = *reinterpret_cast<const uint8_t*>(ptr);
                        }
                        if (idx < vertexCount) {
                            indices.push_back(idx);
                        } else {
                            derror("VER: invalid index %d, vertex count %zu, when loading %s",
                                   idx, vertexCount, filename);
                            indices.push_back(0);
                        }
                    }
                }
            }
        }

        if (indices.empty()) {
            indices.reserve(vertexCount);
            for (size_t i = 0; i < vertexCount; ++i) {
                indices.push_back(static_cast<uint32_t>(i));
            }
        }

        D("%s: Creating glTF primitive mesh with %zu vertices, %zu indices",
          __FUNCTION__, vertices.size(), indices.size());

        Texture texture;
        bool useCheckerboardMaterial = false;

        if (primitive.material >= 0 &&
            primitive.material < static_cast<int>(model.materials.size())) {
            const tinygltf::Material& mat = model.materials[primitive.material];

            if (mat.name.find("TV") != std::string::npos) {
                useCheckerboardMaterial = true;
            }

            int texIndex = mat.pbrMetallicRoughness.baseColorTexture.index;
            if (texIndex >= 0 &&
                texIndex < static_cast<int>(model.textures.size())) {
                const tinygltf::Texture& gltfTex = model.textures[texIndex];
                if (gltfTex.source >= 0 &&
                    gltfTex.source < static_cast<int>(model.images.size())) {
                    const tinygltf::Image& gltfImg =
                            model.images[gltfTex.source];
                    if (gltfImg.uri.find("TV") != std::string::npos ||
                        gltfImg.name.find("TV") != std::string::npos) {
                        useCheckerboardMaterial = true;
                    }
                    if (!gltfImg.uri.empty() &&
                        gltfImg.uri.rfind("data:", 0) != 0) {
                        std::string texPath =
                                PathUtils::join(resourcesDir, gltfImg.uri);
                        texture = renderer.loadTexture(texPath.c_str());
                    } else if (!gltfImg.image.empty() && gltfImg.width > 0 &&
                               gltfImg.height > 0) {
                        int w = gltfImg.width;
                        int h = gltfImg.height;
                        int comp = gltfImg.component;
                        std::vector<uint8_t> rgbaData(w * h * 4);

                        for (int y = 0; y < h; ++y) {
                            const uint8_t* srcRow =
                                    gltfImg.image.data() + y * w * comp;
                            uint8_t* dstRow =
                                    rgbaData.data() + (h - 1 - y) * w * 4;
                            for (int x = 0; x < w; ++x) {
                                if (comp == 4) {
                                    dstRow[x * 4 + 0] = srcRow[x * 4 + 0];
                                    dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                                    dstRow[x * 4 + 2] = srcRow[x * 4 + 2];
                                    dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
                                } else if (comp == 3) {
                                    dstRow[x * 4 + 0] = srcRow[x * 3 + 0];
                                    dstRow[x * 4 + 1] = srcRow[x * 3 + 1];
                                    dstRow[x * 4 + 2] = srcRow[x * 3 + 2];
                                    dstRow[x * 4 + 3] = 255;
                                } else if (comp == 1) {
                                    dstRow[x * 4 + 0] = srcRow[x];
                                    dstRow[x * 4 + 1] = srcRow[x];
                                    dstRow[x * 4 + 2] = srcRow[x];
                                    dstRow[x * 4 + 3] = 255;
                                }
                            }
                        }
                        texture = renderer.createTextureRGBA(rgbaData.data(), w,
                                                             h);
                    }
                }
            }
        }

        if (!texture.isValid() && !useCheckerboardMaterial) {
            uint8_t rgba[4] = {255, 255, 255, 255};
            if (primitive.material >= 0 &&
                primitive.material < static_cast<int>(model.materials.size())) {
                const tinygltf::Material& mat = model.materials[primitive.material];
                if (mat.pbrMetallicRoughness.baseColorFactor.size() == 4) {
                    rgba[0] = static_cast<uint8_t>(
                            std::clamp(mat.pbrMetallicRoughness.baseColorFactor[0] * 255.0, 0.0, 255.0));
                    rgba[1] = static_cast<uint8_t>(
                            std::clamp(mat.pbrMetallicRoughness.baseColorFactor[1] * 255.0, 0.0, 255.0));
                    rgba[2] = static_cast<uint8_t>(
                            std::clamp(mat.pbrMetallicRoughness.baseColorFactor[2] * 255.0, 0.0, 255.0));
                    rgba[3] = static_cast<uint8_t>(
                            std::clamp(mat.pbrMetallicRoughness.baseColorFactor[3] * 255.0, 0.0, 255.0));
                }
            }
            texture = renderer.createTextureRGBA(rgba, 1, 1);
        }

        glm::vec3 primMin(std::numeric_limits<float>::max());
        glm::vec3 primMax(std::numeric_limits<float>::lowest());
        bool primHasVerts = false;
        for (const auto& v : vertices) {
            primMin = glm::min(primMin, v.pos);
            primMax = glm::max(primMax, v.pos);
            primHasVerts = true;
        }
        if (primHasVerts) {
            result->mMinBounds = glm::min(result->mMinBounds, primMin);
            result->mMaxBounds = glm::max(result->mMaxBounds, primMax);
            result->mHasBounds = true;
        }

        Renderable renderable;
        renderable.material = useCheckerboardMaterial
                                      ? renderer.createMaterialCheckerboard()
                                      : renderer.createMaterialTextured();
        renderable.mesh = renderer.createMesh(vertices, indices);
        renderable.texture = texture;

        result->mRenderables.emplace_back(std::move(renderable));

        if (nodeIdx >= 0 && nodeIdx < static_cast<int>(model.nodes.size())) {
            const auto& node = model.nodes[nodeIdx];
            if (node.skin >= 0 && node.skin < static_cast<int>(model.skins.size()) &&
                !unskinnedBaseVertices.empty() && !vertexJoints.empty() && !vertexWeights.empty()) {
                GltfSkinnedPrimitive skinnedPrim;
                skinnedPrim.renderableIndex = result->mRenderables.size() - 1;
                skinnedPrim.baseVertices = unskinnedBaseVertices;
                skinnedPrim.joints = vertexJoints;
                skinnedPrim.weights = vertexWeights;
                skinnedPrim.nodeIdx = nodeIdx;
                skinnedPrim.skinIdx = node.skin;
                result->mGltfSkinnedPrimitives.push_back(std::move(skinnedPrim));
            }
        }
    };

    std::function<void(int, const glm::mat4&)> processNode =
            [&](int nodeIdx, const glm::mat4& parentTransform) {
                if (nodeIdx < 0 ||
                    nodeIdx >= static_cast<int>(model.nodes.size())) {
                    return;
                }
                const tinygltf::Node& node = model.nodes[nodeIdx];

                glm::mat4 localTransform(1.0f);
                if (node.matrix.size() == 16) {
                    for (int col = 0; col < 4; ++col) {
                        for (int row = 0; row < 4; ++row) {
                            localTransform[col][row] = static_cast<float>(
                                    node.matrix[col * 4 + row]);
                        }
                    }
                } else {
                    glm::vec3 translation(0.0f);
                    if (node.translation.size() == 3) {
                        translation = glm::vec3(node.translation[0],
                                                node.translation[1],
                                                node.translation[2]);
                    }
                    glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
                    if (node.rotation.size() == 4) {
                        rotation = glm::quat(
                                static_cast<float>(node.rotation[3]),  // w
                                static_cast<float>(node.rotation[0]),  // x
                                static_cast<float>(node.rotation[1]),  // y
                                static_cast<float>(node.rotation[2])); // z
                    }
                    glm::vec3 scale(1.0f);
                    if (node.scale.size() == 3) {
                        scale = glm::vec3(node.scale[0], node.scale[1],
                                          node.scale[2]);
                    }
                    localTransform =
                            glm::translate(glm::mat4(1.0f), translation) *
                            glm::mat4_cast(rotation) *
                            glm::scale(glm::mat4(1.0f), scale);
                }

                glm::mat4 currentTransform = parentTransform * localTransform;

                if (node.mesh >= 0 &&
                    node.mesh < static_cast<int>(model.meshes.size())) {
                    const tinygltf::Mesh& mesh = model.meshes[node.mesh];
                    for (const tinygltf::Primitive& primitive :
                         mesh.primitives) {
                        processPrimitive(primitive, currentTransform, nodeIdx);
                    }
                }

                for (int childIdx : node.children) {
                    processNode(childIdx, currentTransform);
                }
            };

    int activeScene = model.defaultScene >= 0 ? model.defaultScene : 0;
    if (activeScene >= 0 &&
        activeScene < static_cast<int>(model.scenes.size())) {
        for (int nodeIdx : model.scenes[activeScene].nodes) {
            processNode(nodeIdx, glm::mat4(1.0f));
        }
    } else if (!model.nodes.empty()) {
        std::vector<bool> isChild(model.nodes.size(), false);
        for (const auto& n : model.nodes) {
            for (int c : n.children) {
                if (c >= 0 && c < static_cast<int>(isChild.size())) {
                    isChild[c] = true;
                }
            }
        }
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            if (!isChild[i]) {
                processNode(static_cast<int>(i), glm::mat4(1.0f));
            }
        }
    } else if (!model.meshes.empty()) {
        for (const auto& mesh : model.meshes) {
            for (const auto& primitive : mesh.primitives) {
                processPrimitive(primitive, glm::mat4(1.0f), -1);
            }
        }
    }

    return result;
}

std::unique_ptr<MeshSceneObject> MeshSceneObject::load(Renderer& renderer,
                                                       const char* filename) {
    if (!filename) {
        E("%s: Invalid input", __FUNCTION__);
        return nullptr;
    }
    if (isGltfFile(filename)) {
        return loadGltf(renderer, filename);
    } else {
        return loadObj(renderer, filename);
    }
}

std::unique_ptr<MeshSceneObject> MeshSceneObject::createSphere(
        Renderer& renderer) {
    // Number of segments horizontally and vertically
    const int segments = 64;

    // Generate vertices
    std::vector<VertexPositionUV> vertices;
    vertices.reserve((segments + 1) * (segments + 1));
    for (int i = 0; i <= segments; ++i) {
        float v_coord = (float)i / segments;
        float phi = v_coord * M_PI;

        for (int j = 0; j <= segments; ++j) {
            float u_coord = (float)j / segments;
            float theta = (u_coord + 0.25f) * 2.0f * M_PI;  // Longitude

            VertexPositionUV v;
            v.pos.x = std::sin(phi) * std::cos(theta);
            v.pos.y = std::cos(phi);
            v.pos.z = std::sin(phi) * std::sin(theta);
            v.uv.x = u_coord;
            v.uv.y = 1.0f - v_coord;

            vertices.push_back(v);
        }
    }

    // Generate faces
    std::vector<uint32_t> indices;
    indices.reserve(segments * segments * 2);
    for (int i = 0; i < segments; ++i) {
        int row1 = i * (segments + 1);
        int row2 = (i + 1) * (segments + 1);

        for (int j = 0; j < segments; ++j) {
            int p1 = row1 + j;
            int p2 = p1 + 1;
            int p3 = row2 + j;
            int p4 = p3 + 1;

            // two triangles per segment
            indices.push_back(p1);
            indices.push_back(p3);
            indices.push_back(p2);

            indices.push_back(p2);
            indices.push_back(p3);
            indices.push_back(p4);
        }
    }

    // Generate renderable
    Renderable renderable;
    renderable.material = renderer.createMaterialTextured();
    renderable.mesh = renderer.createMesh(vertices, indices);

    // Generate scene object
    std::unique_ptr<MeshSceneObject> result(new MeshSceneObject(renderer));
    result->mRenderables.emplace_back(std::move(renderable));

    return result;
}

void MeshSceneObject::setAnimationTime(float timeSec) {
    if (mGltfAnimations.empty() || mGltfSkinnedPrimitives.empty() || mGltfNodes.empty()) {
        return;
    }

    std::vector<GltfNodeData> nodes = mGltfNodes;

    // 1. Play the first animation by default
    const auto& anim = mGltfAnimations[0];
    float duration = anim.maxTime - anim.minTime;
    float animTime = anim.minTime;
    if (duration > 0.0001f) {
        animTime += std::fmod(timeSec, duration);
    }

    for (const auto& channel : anim.channels) {
        if (channel.targetNode < 0 || channel.targetNode >= static_cast<int>(nodes.size()) ||
            channel.keyTimes.empty()) {
            continue;
        }

        auto& node = nodes[channel.targetNode];
        node.hasMatrix = false;

        size_t k0 = 0;
        size_t k1 = 0;
        if (animTime <= channel.keyTimes.front()) {
            k0 = k1 = 0;
        } else if (animTime >= channel.keyTimes.back()) {
            k0 = k1 = channel.keyTimes.size() - 1;
        } else {
            for (size_t k = 0; k < channel.keyTimes.size() - 1; ++k) {
                if (animTime >= channel.keyTimes[k] && animTime <= channel.keyTimes[k + 1]) {
                    k0 = k;
                    k1 = k + 1;
                    break;
                }
            }
        }

        float factor = 0.0f;
        if (k0 != k1 && channel.keyTimes[k1] > channel.keyTimes[k0]) {
            factor = (animTime - channel.keyTimes[k0]) / (channel.keyTimes[k1] - channel.keyTimes[k0]);
        }

        if (channel.targetPath == "translation" && !channel.keyTranslations.empty()) {
            node.translation = glm::mix(channel.keyTranslations[k0], channel.keyTranslations[k1], factor);
        } else if (channel.targetPath == "rotation" && !channel.keyRotations.empty()) {
            node.rotation = glm::slerp(channel.keyRotations[k0], channel.keyRotations[k1], factor);
        } else if (channel.targetPath == "scale" && !channel.keyScales.empty()) {
            node.scale = glm::mix(channel.keyScales[k0], channel.keyScales[k1], factor);
        }
    }

    // 2. Compute Global Transforms for all nodes
    std::vector<glm::mat4> globalTransforms(nodes.size(), glm::mat4(1.0f));
    std::function<void(int, const glm::mat4&)> computeGlobal = [&](int nodeIdx, const glm::mat4& parentTransform) {
        if (nodeIdx < 0 || nodeIdx >= static_cast<int>(nodes.size())) return;
        const auto& n = nodes[nodeIdx];
        glm::mat4 local(1.0f);
        if (n.hasMatrix) {
            local = n.matrix;
        } else {
            local = glm::translate(glm::mat4(1.0f), n.translation) *
                    glm::mat4_cast(n.rotation) *
                    glm::scale(glm::mat4(1.0f), n.scale);
        }
        globalTransforms[nodeIdx] = parentTransform * local;
        for (int c : n.children) {
            computeGlobal(c, globalTransforms[nodeIdx]);
        }
    };

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].parent == -1) {
            computeGlobal(static_cast<int>(i), glm::mat4(1.0f));
        }
    }

    // 3. Compute Joint Matrices for each skin
    std::vector<std::vector<glm::mat4>> skinJointMatrices(mGltfSkins.size());
    for (size_t s = 0; s < mGltfSkins.size(); ++s) {
        const auto& skin = mGltfSkins[s];
        skinJointMatrices[s].resize(skin.joints.size());
        for (size_t j = 0; j < skin.joints.size(); ++j) {
            int jointNode = skin.joints[j];
            glm::mat4 invBind = (j < skin.inverseBindMatrices.size()) ? skin.inverseBindMatrices[j] : glm::mat4(1.0f);
            if (jointNode >= 0 && jointNode < static_cast<int>(globalTransforms.size())) {
                skinJointMatrices[s][j] = globalTransforms[jointNode] * invBind;
            } else {
                skinJointMatrices[s][j] = invBind;
            }
        }
    }

    // 4. Update Skinned Vertices
    for (const auto& prim : mGltfSkinnedPrimitives) {
        if (prim.renderableIndex >= mRenderables.size() || prim.skinIdx < 0 ||
            prim.skinIdx >= static_cast<int>(skinJointMatrices.size())) {
            continue;
        }

        const auto& jointMats = skinJointMatrices[prim.skinIdx];
        std::vector<VertexPositionUV> animatedVerts = prim.baseVertices;

        for (size_t i = 0; i < prim.baseVertices.size(); ++i) {
            glm::uvec4 j = prim.joints[i];
            glm::vec4 w = prim.weights[i];
            glm::vec4 basePos(prim.baseVertices[i].pos, 1.0f);

            glm::mat4 skinMat(0.0f);
            for (int k = 0; k < 4; ++k) {
                if (w[k] > 0.0f && j[k] < jointMats.size()) {
                    skinMat += w[k] * jointMats[j[k]];
                }
            }

            animatedVerts[i].pos = glm::vec3(skinMat * basePos);
        }

        mRenderer.updateMesh(mRenderables[prim.renderableIndex].mesh, animatedVerts.data(), animatedVerts.size());
    }
}

}  // namespace ver
}  // namespace android
