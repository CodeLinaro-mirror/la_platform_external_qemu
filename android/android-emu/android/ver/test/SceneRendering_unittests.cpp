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
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "OpenGLESDispatch/OpenGLDispatchLoader.h"
#include "aemu/base/Debug.h"
#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"
#include "android/loadpng.h"
#include "ver/virtual_environment_renderer.h"

using namespace android::base;
using namespace gfxstream::host::gl;  // For LazyLoadedGLESv2Dispatch

namespace {

// Threshold for golden image comparison (Sum of Squared Differences per pixel)
static constexpr double kCompareThreshold = 512.0;

class SceneRenderingTest : public ::testing::TestWithParam<std::string> {
protected:
    void SetUp() override {
        std::vector<std::filesystem::path> resourcePaths;
        std::string resourcesDir = PathUtils::join(
                System::get()->getProgramDirectory(), "resources");
        resourcePaths.push_back(resourcesDir);

        std::filesystem::path vulkanDir = PathUtils::join(
                System::get()->getProgramDirectory(), "lib64", "vulkan");

        // Initialize with lazy-loaded GL dispatch
        bool success = ver_initialize(
                resourcePaths, (const void*)LazyLoadedEGLDispatch::get(),
                (const void*)LazyLoadedGLESv2Dispatch::get(), vulkanDir);
        ASSERT_TRUE(success) << "Failed to initialize VER";
    }

    void TearDown() override { ver_cleanup(); }
};

static double calculateSSD(const uint8_t* data1,
                           const uint8_t* data2,
                           size_t size) {
    double sum = 0.0;
    for (size_t i = 0; i < size; ++i) {
        double diff = static_cast<double>(data1[i]) - data2[i];
        sum += diff * diff;
    }
    return sum;
}

static bool compareWithGolden(const uint8_t* actualData,
                              int width,
                              int height,
                              const std::string& goldenPath) {
    int goldenWidth, goldenHeight, goldenBpp;
    std::vector<uint8_t> goldenBuffer;

    if (!ver_texture_utils_load_png(goldenPath.c_str(), &goldenWidth,
                                    &goldenHeight, &goldenBpp,
                                    &goldenBuffer)) {
        fprintf(stderr, "Failed to load golden image: %s",
                goldenPath.c_str());
        return false;
    }

    if (width != goldenWidth || height != goldenHeight) {
        fprintf(stderr,
                "Dimensions mismatch: actual(%dx%d) vs golden(%dx%d)",
                width, height, goldenWidth, goldenHeight);
        return false;
    }

    // VER framebuffer is RGBA8 (4 bytes per pixel)
    // TextureUtils load might return RGB24 or RGBA32.
    size_t actualSize = width * height * 4;

    if (goldenBpp == 3) {
        // Convert golden to RGBA for easier comparison
        std::vector<uint8_t> convertedGolden(actualSize);
        for (int i = 0; i < width * height; ++i) {
            convertedGolden[i * 4 + 0] = goldenBuffer[i * 3 + 0];
            convertedGolden[i * 4 + 1] = goldenBuffer[i * 3 + 1];
            convertedGolden[i * 4 + 2] = goldenBuffer[i * 3 + 2];
            convertedGolden[i * 4 + 3] = 255;
        }
        return calculateSSD(actualData, convertedGolden.data(),
                            actualSize) <= kCompareThreshold * actualSize;
    } else {
        return calculateSSD(actualData, goldenBuffer.data(), actualSize) <=
               kCompareThreshold * actualSize;
    }
}

TEST_P(SceneRenderingTest, RenderSceneMode) {
    std::string modeName = GetParam();
    VerSceneConfig::Mode mode = VerSceneConfig::modeFromString(modeName);

    // Fail on invalid modes
    ASSERT_NE(mode, VerSceneConfig::Mode::Unknown);

#ifndef __APPLE__
    // TODO(virtualscene-library): Fix software GLES initialization on linux&windows
    if (VerSceneConfig::modeRequiresRenderer(mode)) {
        GTEST_SKIP()
                << "GLES renderer is currently not supported on this platform for testing.";
        return;
    }
#endif
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

    std::string goldenPath =
            PathUtils::join(System::get()->getProgramDirectory(), "testdata",
                            "scene_" + modeName + "_golden.png");

    // If golden image doesn't exist, we might want to skip or fail.
    // Here we fail to ensure all modes have goldens.
    if (!std::filesystem::exists(goldenPath)) {
        std::string outputPath =
                PathUtils::join(System::get()->getTempDir(), "ver_test_outputs",
                                modeName + "_output.png");
        std::filesystem::create_directories(
                std::filesystem::path(outputPath).parent_path());
        savepng(outputPath.c_str(), 4, width, height, SKIN_ROTATION_0,
                (void*)fbData);
        FAIL() << "Golden image not found at: " << goldenPath
               << ". Output saved to: " << outputPath;
    }

    bool match = compareWithGolden(fbData, width, height, goldenPath);

    if (!match) {
        std::string outputPath =
                PathUtils::join(System::get()->getTempDir(), "ver_test_outputs",
                                "scene_" + modeName + "_failed.png");
        std::filesystem::create_directories(
                std::filesystem::path(outputPath).parent_path());
        savepng(outputPath.c_str(), 4, width, height, SKIN_ROTATION_0,
                (void*)fbData);
        FAIL() << "Golden image comparison failed for mode: " << modeName
               << ". Output saved to: " << outputPath;
    }

    ver_destroy_render_view(view);
    ver_destroy_scene(scene);
}

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

TEST(SceneRenderingTestSimple, PosterSideBySideTest) {
#ifndef __APPLE__
    {
        // TODO(virtualscene-library): Fix software GLES initialization on
        // linux&windows
        GTEST_SKIP()
                << "GLES renderer is currently not supported on this platform for testing.";
        return;
    }
#endif
    std::string testdataDir =
            PathUtils::join(System::get()->getProgramDirectory(), "testdata");
    std::string objPath = PathUtils::join(testdataDir, "poster_test.obj");
    std::string postersPath = PathUtils::join(testdataDir, "poster_test.posters");

    std::vector<std::filesystem::path> resourcePaths;
    std::string resourcesDir = PathUtils::join(
            System::get()->getProgramDirectory(), "resources");
    resourcePaths.push_back(resourcesDir);
    resourcePaths.push_back(testdataDir);

    std::filesystem::path vulkanDir = PathUtils::join(
            System::get()->getProgramDirectory(), "lib64", "vulkan");

    ver_initialize(resourcePaths, (const void*)LazyLoadedEGLDispatch::get(),
                   (const void*)LazyLoadedGLESv2Dispatch::get(), vulkanDir);

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

        std::string img1Path = PathUtils::join(testdataDir, "256x256_android.png");
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

    std::string goldenPath =
            PathUtils::join(System::get()->getProgramDirectory(), "testdata",
                            "scene_poster_side_by_side_golden.png");

    if (!std::filesystem::exists(goldenPath)) {
        std::string outputPath =
                PathUtils::join(System::get()->getTempDir(), "ver_test_outputs",
                                "scene_poster_side_by_side_output.png");
        std::filesystem::create_directories(
                std::filesystem::path(outputPath).parent_path());
        savepng(outputPath.c_str(), 4, width, height, SKIN_ROTATION_0,
                (void*)fbData);
        FAIL() << "Golden image not found at: " << goldenPath
               << ". Output saved to: " << outputPath;
    }

    bool match = compareWithGolden(fbData, width, height, goldenPath);

    if (!match) {
        std::string outputPath =
                PathUtils::join(System::get()->getTempDir(), "ver_test_outputs",
                                "scene_poster_side_by_side_failed.png");
        std::filesystem::create_directories(
                std::filesystem::path(outputPath).parent_path());
        savepng(outputPath.c_str(), 4, width, height, SKIN_ROTATION_0,
                (void*)fbData);
        FAIL() << "Golden image comparison failed for PosterSideBySideTest. Output saved to: "
               << outputPath;
    }

    ver_destroy_render_view(view);
    ver_destroy_scene(scene);
    ver_cleanup();
}

INSTANTIATE_TEST_SUITE_P(SceneModes,
                         SceneRenderingTest,
                         ::testing::Values("mesh3d",
                                           "videofile",
                                           "imagefile",
                                           "color",
                                           "image360"));

}  // namespace

