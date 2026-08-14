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
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "OpenGLESDispatch/OpenGLDispatchLoader.h"
#include "Renderer.h"
#include "aemu/base/Debug.h"
#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"
#include "TestUtils.h"

using namespace android::base;
using namespace android::ver;
using namespace gfxstream::host::gl;  // For LazyLoadedGLESv2Dispatch

namespace {

class SceneRenderingTest
    : public ::testing::TestWithParam<std::tuple<std::string, std::string>> {
protected:
    void SetUp() override {
        setupTestLibraryPaths();
        const auto& [backend, modeName] = GetParam();
        System::get()->envSet("VER_RENDERER_BACKEND", backend);

        std::vector<std::filesystem::path> resourcePaths;
        std::string resourcesDir = PathUtils::join(
                System::get()->getProgramDirectory(), "resources");
        resourcePaths.push_back(resourcesDir);
        resourcePaths.push_back(System::get()->getProgramDirectory());

        std::filesystem::path vulkanDir = PathUtils::join(
                System::get()->getProgramDirectory(), "lib64", "vulkan");

        // Enable flags for upcoming features
        System::get()->envSet("ANDROID_EMU_ENABLE_STREETVIEW", "1");
        System::get()->envSet("ANDROID_EMU_ENABLE_VIDEO360", "1");

        // Set test parameters for streetview mode to avoid dependency on
        // internet connection
        System::get()->envSet("ANDROID_EMU_STREETVIEW_IMAGE_PATH",
                              "testdata/streetview.jpg");

        // Initialize with lazy-loaded GL dispatch
        bool success = ver_initialize(
                resourcePaths, (const void*)LazyLoadedEGLDispatch::get(),
                (const void*)LazyLoadedGLESv2Dispatch::get(), vulkanDir);
        ASSERT_TRUE(success)
                << "Failed to initialize VER with backend: " << backend;
    }

    void TearDown() override {
        ver_cleanup();
        System::get()->envSet("VER_RENDERER_BACKEND", "");
    }
};

TEST_P(SceneRenderingTest, RenderSceneMode) {
    const auto& [backend, modeName] = GetParam();
    VerSceneConfig::Mode mode = VerSceneConfig::modeFromString(modeName);

    // Fail on invalid modes
    ASSERT_NE(mode, VerSceneConfig::Mode::Unknown);

    VerSceneConfig config(mode, VerSceneConfig::defaultArgumentForMode(mode));
    VerSceneHandle scene = ver_create_scene(config);
    ASSERT_NE(scene, (VerSceneHandle)VER_INVALID_HANDLE)
            << "Failed to create scene for mode: " << modeName;

    ver_scene_load_user_resources(scene, []() {});
    ver_scene_update(scene, true);

    VerRenderViewHandle view = ver_create_render_view();
    ASSERT_NE(view, (VerRenderViewHandle)VER_INVALID_HANDLE);

    int width = 640;
    int height = 480;
    ASSERT_TRUE(ver_render_view_set_dimensions(view, width, height));

    // View projection from the initial SceneCamera values
    const float viewProj[16] = {1.500f,  0.000f,  0.000f,  0.000f,   //
                                0.000f,  -1.993f, -0.083f, -0.083f,  //
                                -0.000f, 0.166f,  -0.999f, -0.997f,  //
                                -0.090f, -0.060f, -0.202f, -0.002f};

    ver_render_view_set_view_projection(view, viewProj);

    bool rendered = ver_render_view(scene, view, []() {}, nullptr);
    if (!rendered) {
        // Some modes might fail if GL is not available or if resources are
        // missing. We log it and fail the test.
        ver_destroy_render_view(view);
        ver_destroy_scene(scene);
        FAIL() << "Rendering failed for mode: " << modeName;
    }

    const uint8_t* fbData = nullptr;
    uint64_t fbSize = 0;
    ver_render_view_get_framebuffer(view, &fbData, &fbSize);
    ASSERT_NE(fbData, nullptr);
    ASSERT_EQ(fbSize, (uint64_t)width * height * 4);

    verifyOrCompareWithGolden(fbData, width, height,
                              "scene_" + modeName + "_golden.png",
                              "scene_" + modeName);

    ver_destroy_render_view(view);
    ver_destroy_scene(scene);
}

INSTANTIATE_TEST_SUITE_P(SceneModes,
                         SceneRenderingTest,
                         ::testing::Combine(::testing::Values("vulkan", "gles"),
                                            ::testing::Values("mesh3d",
                                                              "videofile",
                                                              "imagefile",
                                                              "color",
                                                              "image360",
                                                              "streetview",
                                                              "video360")));

TEST(SceneRenderingTestSimple, InvalidDimensions) {
    VerRenderViewHandle view = ver_create_render_view();
    ASSERT_NE(view, (VerRenderViewHandle)VER_INVALID_HANDLE);

    int32_t width = -1;
    int32_t height = -1;

    // Default dimensions should be 0
    ver_render_view_get_dimensions(view, &width, &height);
    EXPECT_EQ(width, 0);
    EXPECT_EQ(height, 0);

    // Set valid dimensions - should return true
    EXPECT_TRUE(ver_render_view_set_dimensions(view, 640, 480));
    ver_render_view_get_dimensions(view, &width, &height);
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 480);

    // Set invalid negative dimensions - should return false and be rejected
    EXPECT_FALSE(ver_render_view_set_dimensions(view, -10, 480));
    ver_render_view_get_dimensions(view, &width, &height);
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 480);

    // Set invalid too large dimensions - should return false and be rejected
    EXPECT_FALSE(ver_render_view_set_dimensions(view, 20000, 480));
    ver_render_view_get_dimensions(view, &width, &height);
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 480);

    // Set invalid height - should return false and be rejected
    EXPECT_FALSE(ver_render_view_set_dimensions(view, 640, -1));
    ver_render_view_get_dimensions(view, &width, &height);
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 480);

    // Set invalid too large height - should return false and be rejected
    EXPECT_FALSE(ver_render_view_set_dimensions(view, 640, 20000));
    ver_render_view_get_dimensions(view, &width, &height);
    EXPECT_EQ(width, 640);
    EXPECT_EQ(height, 480);

    ver_destroy_render_view(view);
}

TEST(SceneRenderingTestSimple, BackendSelectionEnvVar) {
    setupTestLibraryPaths();
    std::vector<std::filesystem::path> resourcePaths;
    std::string resourcesDir = PathUtils::join(
            android::base::System::get()->getProgramDirectory(), "resources");
    resourcePaths.push_back(resourcesDir);

    std::filesystem::path vulkanDir =
            PathUtils::join(android::base::System::get()->getProgramDirectory(),
                            "lib64", "vulkan");

    // Test Vulkan forcing
    android::base::System::get()->envSet("VER_RENDERER_BACKEND", "vulkan");
    EXPECT_TRUE(ver_initialize(
            resourcePaths, (const void*)LazyLoadedEGLDispatch::get(),
            (const void*)LazyLoadedGLESv2Dispatch::get(), vulkanDir));
    ver_cleanup();

    // Test GLES forcing
    android::base::System::get()->envSet("VER_RENDERER_BACKEND", "gles");
    EXPECT_TRUE(ver_initialize(
            resourcePaths, (const void*)LazyLoadedEGLDispatch::get(),
            (const void*)LazyLoadedGLESv2Dispatch::get(), vulkanDir));
    ver_cleanup();

    // Test Auto mode
    android::base::System::get()->envSet("VER_RENDERER_BACKEND", "auto");
    EXPECT_TRUE(ver_initialize(
            resourcePaths, (const void*)LazyLoadedEGLDispatch::get(),
            (const void*)LazyLoadedGLESv2Dispatch::get(), vulkanDir));
    ver_cleanup();

    android::base::System::get()->envSet("VER_RENDERER_BACKEND", "");
}

class PosterSideBySideTest : public ::testing::TestWithParam<std::string> {
protected:
    void SetUp() override {
        setupTestLibraryPaths();
        const std::string& backend = GetParam();
        System::get()->envSet("VER_RENDERER_BACKEND", backend);
    }
    void TearDown() override {
        ver_cleanup();
        System::get()->envSet("VER_RENDERER_BACKEND", "");
    }
};

TEST_P(PosterSideBySideTest, PosterSideBySide) {
    const std::string& backend = GetParam();
    std::string testdataDir =
            PathUtils::join(System::get()->getProgramDirectory(), "testdata");
    std::string objPath = PathUtils::join(testdataDir, "poster_test.obj");
    std::string postersPath =
            PathUtils::join(testdataDir, "poster_test.posters");

    std::vector<std::filesystem::path> resourcePaths;
    std::string resourcesDir =
            PathUtils::join(System::get()->getProgramDirectory(), "resources");
    resourcePaths.push_back(resourcesDir);
    resourcePaths.push_back(testdataDir);

    std::filesystem::path vulkanDir = PathUtils::join(
            System::get()->getProgramDirectory(), "lib64", "vulkan");

    bool success = ver_initialize(
            resourcePaths, (const void*)LazyLoadedEGLDispatch::get(),
            (const void*)LazyLoadedGLESv2Dispatch::get(), vulkanDir);
    ASSERT_TRUE(success) << "Failed to initialize VER with backend: "
                         << backend;
    VerSceneConfig config(VerSceneConfig::Mode::Mesh3D, "poster_test.obj");
    VerSceneHandle scene = ver_create_scene(config);
    ASSERT_NE(scene, (VerSceneHandle)VER_INVALID_HANDLE);

    std::ifstream in(postersPath);
    std::string str;
    std::vector<android::ver::PosterInfo> posters;
    android::ver::PosterInfo poster;
    while (in >> str) {
        if (str == "poster") {
            if (!poster.name.empty()) {
                posters.push_back(poster);
            }
            poster = android::ver::PosterInfo();
            in >> poster.name;
        } else if (str == "position") {
            in >> poster.position.x >> poster.position.y >> poster.position.z;
        } else if (str == "rotation") {
            glm::vec3 euler;
            in >> euler.x >> euler.y >> euler.z;
            float pitch = glm::radians(euler.x);
            float yaw = glm::radians(euler.y);
            float roll = glm::radians(euler.z);
            glm::quat qX = glm::angleAxis(pitch, glm::vec3(1, 0, 0));
            glm::quat qY = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
            glm::quat qZ = glm::angleAxis(roll, glm::vec3(0, 0, 1));
            poster.rotation = qX * qY * qZ;
        } else if (str == "size") {
            in >> poster.size.x >> poster.size.y;
        }
    }
    if (!poster.name.empty()) {
        posters.push_back(poster);
    }

    ver_scene_load_user_resources(scene, [&]() {
        ASSERT_EQ(posters.size(), 2u);
        for (const auto& posterInfo : posters) {
            bool created = ver_scene_create_poster_location(scene, posterInfo);
            ASSERT_TRUE(created);
        }

        std::string img1Path =
                PathUtils::join(testdataDir, "256x256_android.png");
        std::string img2Path = PathUtils::join(testdataDir, "jpeg_rgb24.jpg");

        ver_scene_load_poster(scene, "poster_left", img1Path.c_str(), 1.0f);
        ver_scene_load_poster(scene, "poster_right", img2Path.c_str(), 1.0f);
    });

    ver_scene_update(scene, true);

    VerRenderViewHandle view = ver_create_render_view();
    ASSERT_NE(view, (VerRenderViewHandle)VER_INVALID_HANDLE);

    int width = 640;
    int height = 480;
    ASSERT_TRUE(ver_render_view_set_dimensions(view, width, height));

    const float viewProj[16] = {1.500f,  0.000f,  0.000f,  0.000f,   //
                                0.000f,  -1.993f, -0.083f, -0.083f,  //
                                -0.000f, 0.166f,  -0.999f, -0.997f,  //
                                -0.090f, -0.060f, -0.202f, -0.002f};
    ver_render_view_set_view_projection(view, viewProj);

    bool rendered = ver_render_view(scene, view, []() {}, nullptr);
    ASSERT_TRUE(rendered);

    const uint8_t* fbData = nullptr;
    uint64_t fbSize = 0;
    ver_render_view_get_framebuffer(view, &fbData, &fbSize);
    ASSERT_NE(fbData, nullptr);
    ASSERT_EQ(fbSize, (uint64_t)width * height * 4);

    // Allow background loader thread to load textures and process queue
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ver_scene_update(scene, true);
    rendered = ver_render_view(scene, view, []() {}, nullptr);
    ASSERT_TRUE(rendered);

    // Render frame 3 so that replaced textures are drawn
    ver_scene_update(scene, true);
    rendered = ver_render_view(scene, view, []() {}, nullptr);
    ASSERT_TRUE(rendered);

    ver_render_view_get_framebuffer(view, &fbData, &fbSize);
    ASSERT_NE(fbData, nullptr);
    ASSERT_EQ(fbSize, (uint64_t)width * height * 4);

    verifyOrCompareWithGolden(fbData, width, height,
                              "scene_poster_side_by_side_golden.png",
                              "scene_poster_side_by_side");

    ver_destroy_render_view(view);
    ver_destroy_scene(scene);
}
INSTANTIATE_TEST_SUITE_P(PosterSideBySide,
                         PosterSideBySideTest,
                         ::testing::Values("vulkan", "gles"));

}  // namespace
