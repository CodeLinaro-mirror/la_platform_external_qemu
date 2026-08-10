/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include "OpenGLESDispatch/OpenGLDispatchLoader.h"
#include "aemu/base/Debug.h"
#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"
#include "android/loadpng.h"
#include "TestUtils.h"
#include "MeshSceneObject.h"
#include <glm/gtc/matrix_transform.hpp>

using namespace android::base;
using namespace android::ver;
using namespace gfxstream::host::gl;  // For LazyLoadedGLESv2Dispatch

namespace {

TEST(MeshSceneObjectTest, LoadGltf) {
    std::string tempGltfPath =
            PathUtils::join(System::get()->getTempDir(), "test_mesh.gltf");
    std::ofstream out(tempGltfPath);
    out << R"({
  "asset": { "version": "2.0" },
  "scenes": [ { "nodes": [0] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ {
    "primitives": [ {
      "attributes": { "POSITION": 0 },
      "indices": 1
    } ]
  } ],
  "accessors": [
    {
      "bufferView": 0,
      "componentType": 5126,
      "count": 3,
      "type": "VEC3",
      "max": [1.0, 1.0, 0.0],
      "min": [0.0, 0.0, 0.0]
    },
    {
      "bufferView": 1,
      "componentType": 5123,
      "count": 3,
      "type": "SCALAR"
    }
  ],
  "bufferViews": [
    {
      "buffer": 0,
      "byteOffset": 0,
      "byteLength": 36
    },
    {
      "buffer": 0,
      "byteOffset": 36,
      "byteLength": 6
    }
  ],
  "buffers": [
    {
      "byteLength": 42,
      "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"
    }
  ]
})";
    out.close();

    EXPECT_TRUE(android::ver::MeshSceneObject::canLoad(tempGltfPath.c_str()));
    std::filesystem::remove(tempGltfPath);
}

class SceneRenderingGltfTest
    : public ::testing::TestWithParam<
          std::tuple<std::string, std::tuple<std::string, std::string, bool>>> {
protected:
    void SetUp() override {
        setupTestLibraryPaths();
        const auto& [backend, modelInfo] = GetParam();
        System::get()->envSet("VER_RENDERER_BACKEND", backend);

        std::vector<std::filesystem::path> resourcePaths;
        std::string resourcesDir = PathUtils::join(
                System::get()->getProgramDirectory(), "resources");
        resourcePaths.push_back(resourcesDir);
        std::string testdataDir = PathUtils::join(
                System::get()->getProgramDirectory(), "testdata");
        resourcePaths.push_back(testdataDir);

        std::filesystem::path vulkanDir = PathUtils::join(
                System::get()->getProgramDirectory(), "lib64", "vulkan");

        bool success = ver_initialize(
                resourcePaths, (const void*)LazyLoadedEGLDispatch::get(),
                (const void*)LazyLoadedGLESv2Dispatch::get(), vulkanDir);
        ASSERT_TRUE(success) << "Failed to initialize VER with backend: " << backend;
    }

    void TearDown() override {
        ver_cleanup();
        System::get()->envSet("VER_RENDERER_BACKEND", "");
    }
};

TEST_P(SceneRenderingGltfTest, RenderGltfModel) {
    auto [backend, modelInfo] = GetParam();
    auto [testName, modelPath, isAnimated] = modelInfo;

    VerSceneConfig config(VerSceneConfig::Mode::Mesh3D, modelPath);
    VerSceneHandle scene = ver_create_scene(config);
    ASSERT_NE(scene, (VerSceneHandle)VER_INVALID_HANDLE)
            << "Failed to create scene for model: " << modelPath;

    ver_scene_load_user_resources(scene, []() {});

    VerRenderViewHandle view = ver_create_render_view();
    ASSERT_NE(view, (VerRenderViewHandle)VER_INVALID_HANDLE);

    int width = 640;
    int height = 480;
    ASSERT_TRUE(ver_render_view_set_dimensions(view, width, height));

    float minX, minY, minZ, maxX, maxY, maxZ;
    ASSERT_TRUE(ver_scene_get_bounding_box(scene, &minX, &minY, &minZ, &maxX, &maxY, &maxZ))
            << "Failed to get bounding box for scene: " << modelPath;

    glm::vec3 center((minX + maxX) * 0.5f, (minY + maxY) * 0.5f, (minZ + maxZ) * 0.5f);
    glm::vec3 extents(maxX - minX, maxY - minY, maxZ - minZ);
    float radius = glm::length(extents) * 0.5f;
    if (radius < 0.001f) radius = 1.0f;

    float fovY = glm::radians(35.0f);
    float distance = (radius / std::sin(fovY * 0.5f)) * 0.5f;

    glm::vec3 eye = center + glm::vec3(distance, radius * 2.3f, distance);
    glm::vec3 up(0.0f, 1.0f, 0.0f);

    glm::mat4 viewMat = glm::lookAt(eye, center, up);
    glm::mat4 projMat = glm::perspective(fovY, static_cast<float>(width) / height, 0.1f, distance + radius * 10.0f);
    projMat[1][1] = -projMat[1][1];  // Camera stack Y-flip

    glm::mat4 viewProjMat = projMat * viewMat;
    ver_render_view_set_view_projection(view, &viewProjMat[0][0]);

    auto renderAndCompare = [&](uint64_t timeUs, const std::string& goldenSuffix) {
        ver_scene_set_frame_time_us(scene, timeUs);
        ver_scene_update(scene, false);

        bool rendered = ver_render_view(scene, view, []() {}, nullptr);
        if (!rendered) {
            FAIL() << "Rendering failed for model: " << modelPath;
        }

        const uint8_t* fbData = nullptr;
        uint64_t fbSize = 0;
        ver_render_view_get_framebuffer(view, &fbData, &fbSize);
        ASSERT_NE(fbData, nullptr);
        ASSERT_EQ(fbSize, static_cast<uint64_t>(width * height * 4));

        std::string goldenName = "gltf_" + testName + goldenSuffix + "_golden.png";
        verifyOrCompareWithGolden(fbData, width, height, goldenName,
                                  "gltf_" + testName + goldenSuffix);
    };

    if (isAnimated) {
        renderAndCompare(0, "_0s");
        renderAndCompare(1000000, "_1s");
    } else {
        renderAndCompare(0, "");
    }

    ver_destroy_render_view(view);
    ver_destroy_scene(scene);
}

INSTANTIATE_TEST_SUITE_P(
    GltfModels,
    SceneRenderingGltfTest,
    ::testing::Combine(
        ::testing::Values("vulkan", "gles"),
        ::testing::Values(
            std::make_tuple("box", "Box.glb", false),
            std::make_tuple("box_textured", "BoxTextured.glb", false),
            std::make_tuple("duck", "Duck.gltf", false),
            std::make_tuple("simple_skin", "SimpleSkin.gltf", true)
        )
    )
);

}  // namespace
