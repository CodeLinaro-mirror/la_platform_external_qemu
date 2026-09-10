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

#pragma once

#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "OpenGLESDispatch/EGLDispatch.h"
#include "OpenGLESDispatch/GLESv2Dispatch.h"
#include "OpenGLESDispatch/OpenGLDispatchLoader.h"
#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"
#include "android/loadpng.h"
#include "ver/virtual_environment_renderer.h"

namespace android {
namespace ver {

inline void setupTestLibraryPaths() {
    using namespace ::android::base;
    std::string lib64dir = PathUtils::join(
            System::get()->getProgramDirectory(), "lib64");
    System::get()->addLibrarySearchDir(lib64dir);

#if defined(__APPLE__) || defined(__APPLE)
    const char* glesBackend = "gles_swangle";
#else
    const char* glesBackend = "gles_swiftshader";
#endif
    std::string glesSwiftshaderDir = PathUtils::join(lib64dir, glesBackend);
    System::get()->addLibrarySearchDir(glesSwiftshaderDir);

    std::string vulkanDir = PathUtils::join(lib64dir, "vulkan");
    System::get()->addLibrarySearchDir(vulkanDir);
}

inline GLint queryGlesMaxTextureSize() {
    using namespace gfxstream::host::gl;
    const EGLDispatch* egl = LazyLoadedEGLDispatch::get();
    const GLESv2Dispatch* gles2 = LazyLoadedGLESv2Dispatch::get();
    if (!egl || !gles2) {
        return 0;
    }

#ifndef __APPLE__
    if (egl->eglUseOsEglApi) {
        egl->eglUseOsEglApi(EGL_TRUE, EGL_FALSE);
    }
#endif

    EGLDisplay display = egl->eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        return 0;
    }

    EGLint eglMaj = 0;
    EGLint eglMin = 0;
    if (egl->eglInitialize(display, &eglMaj, &eglMin) == EGL_FALSE) {
        return 0;
    }

    const EGLint attribs[] = {EGL_SURFACE_TYPE,
                              EGL_PBUFFER_BIT,
                              EGL_RENDERABLE_TYPE,
                              EGL_OPENGL_ES2_BIT,
                              EGL_BLUE_SIZE,
                              8,
                              EGL_GREEN_SIZE,
                              8,
                              EGL_RED_SIZE,
                              8,
                              EGL_DEPTH_SIZE,
                              24,
                              EGL_NONE};
    EGLint numConfig = 0;
    EGLConfig eglConfig = nullptr;
    if (egl->eglChooseConfig(display, attribs, &eglConfig, 1, &numConfig) !=
                EGL_TRUE ||
        numConfig < 1) {
        return 0;
    }

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = egl->eglCreateContext(display, eglConfig,
                                               EGL_NO_CONTEXT, contextAttribs);
    if (context == EGL_NO_CONTEXT) {
        return 0;
    }

    const EGLint pbufferAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface surface =
            egl->eglCreatePbufferSurface(display, eglConfig, pbufferAttribs);
    if (surface == EGL_NO_SURFACE) {
        egl->eglDestroyContext(display, context);
        return 0;
    }

    GLint maxTextureSize = 0;
    if (egl->eglMakeCurrent(display, surface, surface, context) == EGL_TRUE) {
        gles2->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
        egl->eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                            EGL_NO_CONTEXT);
    }

    egl->eglDestroySurface(display, surface);
    egl->eglDestroyContext(display, context);
    return maxTextureSize;
}

#define SKIP_IF_GLES_UNAVAILABLE(backend)                                  \
    do {                                                                   \
        if ((backend) == "gles" && queryGlesMaxTextureSize() <= 0) {       \
            GTEST_SKIP() << "Skipping test: GL_MAX_TEXTURE_SIZE is 0 for " \
                            "GLES backend";                                \
        }                                                                  \
    } while (0)

// Threshold for golden image comparison (Sum of Squared Differences per pixel)
static constexpr double kCompareThreshold = 512.0;

inline double calculateSSD(const uint8_t* data1,
                           const uint8_t* data2,
                           size_t size) {
    double sum = 0.0;
    for (size_t i = 0; i < size; ++i) {
        double diff = static_cast<double>(data1[i]) - data2[i];
        sum += diff * diff;
    }
    return sum;
}

inline bool compareWithGolden(const uint8_t* actualData,
                              int width,
                              int height,
                              const std::string& goldenPath) {
    int goldenWidth, goldenHeight, goldenBpp;
    std::vector<uint8_t> goldenBuffer;

    if (!ver_texture_utils_load_png(goldenPath.c_str(), &goldenWidth,
                                    &goldenHeight, &goldenBpp,
                                    &goldenBuffer)) {
        fprintf(stderr, "Failed to load golden image: %s\n",
                goldenPath.c_str());
        return false;
    }

    if (width != goldenWidth || height != goldenHeight) {
        fprintf(stderr,
                "Dimensions mismatch: actual(%dx%d) vs golden(%dx%d)\n",
                width, height, goldenWidth, goldenHeight);
        return false;
    }

    size_t actualSize = width * height * 4;

    if (goldenBpp == 3) {
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

inline void verifyOrCompareWithGolden(const uint8_t* fbData,
                                      int width,
                                      int height,
                                      const std::string& goldenFilename,
                                      const std::string& testLabel) {
    using namespace ::android::base;
    std::string goldenPath = PathUtils::join(
            System::get()->getProgramDirectory(), "testdata", goldenFilename);

    if (!std::filesystem::exists(goldenPath)) {
        std::string outputPath =
                PathUtils::join(System::get()->getTempDir(), "ver_test_outputs",
                                testLabel + "_output.png");
        std::filesystem::create_directories(
                std::filesystem::path(outputPath).parent_path());
        savepng(outputPath.c_str(), 4, width, height, SKIN_ROTATION_0,
                const_cast<void*>(static_cast<const void*>(fbData)));
        FAIL() << "Golden image not found at: " << goldenPath
               << ". Output saved to: " << outputPath;
    }

    bool match = compareWithGolden(fbData, width, height, goldenPath);

    if (!match) {
        std::string outputPath =
                PathUtils::join(System::get()->getTempDir(), "ver_test_outputs",
                                testLabel + "_failed.png");
        std::filesystem::create_directories(
                std::filesystem::path(outputPath).parent_path());
        savepng(outputPath.c_str(), 4, width, height, SKIN_ROTATION_0,
                const_cast<void*>(static_cast<const void*>(fbData)));
        FAIL() << "Golden image comparison failed for: " << testLabel
               << ". Output saved to: " << outputPath;
    }
}

}  // namespace ver
}  // namespace android
