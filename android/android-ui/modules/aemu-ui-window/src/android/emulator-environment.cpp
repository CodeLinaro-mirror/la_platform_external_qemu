/* Copyright (C) 2006-2016 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

#include "android/emulator-window.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "aemu/base/logging/CLog.h"
#include "android/android.h"
#include "android/avd/info.h"
#include "android/avd/keys.h"
#include "android/avd/util.h"
#include "android/console.h"
#include "android/hw-sensors.h"
#include "android/loadpng.h"
#include "android/network/globals.h"
#include "android/skin/keycode.h"
#include "android/skin/window.h"
#include "android/skin/winsys.h"
#include "android/ui-emu-agent.h"
#include "android/utils/debug.h"
#include "android/utils/path.h"
#include "android/virtualscene/Renderer.h"
#include "android/virtualscene/SceneCamera.h"
#include "android/virtualscene/VirtualSceneManager.h"
#include "host-common/display_agent.h"
#include "host-common/hw-config-helper.h"
#include "host-common/opengles.h"
#include "host-common/vm_operations.h"
#include "host-common/window_agent.h"

using android::virtualscene::RendererView;
using android::virtualscene::SceneCamera;
using android::virtualscene::VirtualSceneManager;

static bool load_background_image(CIniFile* environmentIni,
                                  const std::string& backgroundPath) {
    dinfo("%s: Setting up background image: %s", __func__, backgroundPath);

    // Read and decode this file
    uint32_t width = 0, height = 0;
    void* backgroundImageData =
            loadpng(backgroundPath.c_str(), &width, &height);
    if (!backgroundImageData) {
        derror("%s: Could not load background image: %s", __func__,
               backgroundPath.c_str());
        return false;
    }

    const uint32_t maxSizeSupported = 4096;
    if (width > maxSizeSupported || height > maxSizeSupported) {
        derror("%s: Background image is too big(%dx%d), maximum extent: %d",
               __func__, width, height, maxSizeSupported);
        free(backgroundImageData);
        return false;
    }

    // Apply blur in place, allow configuration to adjust the radius
    const double defaultBlurRadius = width * 0.01;
    const float blurValue = (float)iniFile_getDouble(
            environmentIni, "background.image.blurRadius", defaultBlurRadius);
    if (blurValue > 0) {
        const int64_t start = get_uptime_ms();
        RendererView::applyBlurInPlaceCPU(
                width, height, (uint8_t*)backgroundImageData, blurValue);
        const int64_t end = get_uptime_ms();
        const uint64_t blur_time = end - start;
        dinfo("%s: Image blurring took %llu ms, blur amount = %.2f\n", __func__,
              blur_time, blurValue);
    }

    // no cropping, provide all the pixels directly
    android_setOpenglesScreenBackground(width, height,
                                        (const uint8_t*)(backgroundImageData));

    free(backgroundImageData);

    return true;
}

static bool emulatorSetupEnvironment(const AvdInfo* avdInfo,
                                     const bool transparentDisplay) {
    if (!avdInfo) {
        derror("%s: Invalid AVD info", __func__);
        return false;
    }

    CIniFile* configIni = avdInfo_getConfigIni(avdInfo);
    if (!configIni) {
        derror("%s: Invalid AVD config", __func__);
        return false;
    }

    // Check if the camera is set to 'environment'
    std::string hwCameraBack = iniFile_getString(configIni, "hw.camera.back", "");
    const bool cameraUsesEnvironment = (hwCameraBack == "environment");
    const bool backgroundUsesEnvironment = transparentDisplay;
    const bool environmentRequired =
            cameraUsesEnvironment || backgroundUsesEnvironment;

    if (!environmentRequired) {
        dinfo("%s: Environment is not required", __func__);
        return true;
    }

    // Environment is required, set it up
    CIniFile* environmentIni = avdInfo_getEnvironmentIni(avdInfo);
    if (!environmentIni) {
        // If used/required, an environment file should be provided
        derror("%s: No environment config is provided", __func__);
        return false;
    }
    const char* avdBasePath = avdInfo_getContentPath(avdInfo);
    if (!avdBasePath) {
        derror("%s: Cannot find AVD path", __func__);
        return false;
    }

    // Initialize virtual scene and background view
    dinfo("%s: Initializing VirtualSceneManager", __func__);
    if (!VirtualSceneManager::initialize()) {
        LOG(ERROR) << "Cannot initialize virtual scene for the environment";
        return false;
    }

    if (backgroundUsesEnvironment) {
        dinfo("%s: Setting up screen background views", __func__);

        std::string backgroundImageFilename;
        {
            char* imageFilenameDup = iniFile_getString(
                    environmentIni, "background.image.filename", 0);
            if (imageFilenameDup) {
                backgroundImageFilename = imageFilenameDup;
                free(imageFilenameDup);
            }
        }

        if (!backgroundImageFilename.empty()) {
            // Load background image
            std::string backgroundPath = avdBasePath;
            backgroundPath.append(PATH_SEP);
            backgroundPath.append(backgroundImageFilename);
            if (!load_background_image(environmentIni, backgroundPath)) {
                return false;
            }
        } else {
            // Not using a static 2d image background, initialize for
            // 'virtualscene' mode instead
            // TODO(virtualscene): provide ways to create different scenes via
            // environment.ini, through 'background.scene.filename'
            const int displayWidth =
                    iniFile_getInteger(configIni, "hw.lcd.width", 512);
            const int displayheight =
                    iniFile_getInteger(configIni, "hw.lcd.height", 512);

            SceneCamera sceneCamera;
            sceneCamera.setAspectRatio(static_cast<float>(displayWidth) /
                                       displayheight);
            sceneCamera.update();

            // SceneCamera uses 90 degrees rotated views by default for
            // the camera rendering, rotate it back to correct for background
            glm::mat4 cameraView =
                    glm::rotate(sceneCamera.getView(), glm::radians(-90.0f),
                                glm::vec3(0.0f, 0.0f, 1.0f));
            glm::mat4 viewProjection = sceneCamera.getProjection() * cameraView;

            std::unique_ptr<RendererView> backgroundView =
                    std::make_unique<RendererView>();
            backgroundView->updateTarget(RendererView::Format::RGBA8,
                                         displayWidth, displayheight);
            backgroundView->updateViewProjection(viewProjection);
            backgroundView->setBlurFactor(2.0f);

            // TODO(virtualscene): this should be called regularly at ~30fps,
            // should be configurable through environment.ini based on scene
            // type
            VirtualSceneManager::update();

            VirtualSceneManager::renderView(
                    backgroundView.get(), 0.0f, [&backgroundView]() {
                        const std::vector<uint8_t>& fbData =
                                backgroundView->getFramebufferLocked();

                        android_setOpenglesScreenBackground(
                                backgroundView->getWidthLocked(),
                                backgroundView->getHeightLocked(),
                                fbData.data());
                    });
        }
    }

    return true;
}

extern "C" {

bool emulator_window_load_environment(const AvdInfo* avdInfo,
                                      const bool transparentDisplay) {
    return emulatorSetupEnvironment(avdInfo, transparentDisplay);
}
}
