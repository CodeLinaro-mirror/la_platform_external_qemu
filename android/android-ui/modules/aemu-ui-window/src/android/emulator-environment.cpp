/* Copyright (C) 2026 The Android Open Source Project
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

#include <memory>
#include <string>
#include <vector>

#include "aemu/base/logging/CLog.h"
#include "android/android.h"
#include "android/avd/info.h"
#include "android/avd/keys.h"
#include "android/avd/util.h"
#include "android/console.h"
#include "android/hw-sensors.h"
#include "android/network/globals.h"
#include "android/ui-emu-agent.h"
#include "android/utils/debug.h"
#include "android/utils/path.h"
#include "host-common/display_agent.h"
#include "host-common/hw-config-helper.h"
#include "host-common/opengles.h"
#include "host-common/vm_operations.h"
#include "host-common/window_agent.h"

#include "android/virtualscene/Renderer.h"
#include "android/virtualscene/Scene.h"
#include "android/virtualscene/SceneCamera.h"
#include "android/virtualscene/VirtualSceneManager.h"

using android::virtualscene::RendererView;
using android::virtualscene::SceneCamera;
using android::virtualscene::SceneConfig;
using android::virtualscene::VirtualSceneManager;
using android::virtualscene::BackgroundUpdateService;

static bool emulatorSetupEnvironment(const AvdInfo* avdInfo,
                                     const bool transparentDisplay) {
    if (!avdInfo) {
        derror("%s: Invalid AVD info", __func__);
        return false;
    }

    AndroidHwConfig* hwCfg = getConsoleAgents() && getConsoleAgents()->settings
                                     ? getConsoleAgents()->settings->hw()
                                     : nullptr;

    if (!hwCfg) {
        derror("%s: Invalid AVD config", __func__);
        return false;
    }
    const std::string hwCameraBack = hwCfg->hw_camera_back;
    const std::string hwCameraFront = hwCfg->hw_camera_front;
    dprint("%s: cameraBack:%s cameraFront:%s", __func__, hwCameraBack.c_str(),
           hwCameraFront.c_str());

    // Check if the camera is set to 'environment' or 'virtualscene'
    const bool cameraUsesEnvironment = (hwCameraBack == "environment") ||
                                       (hwCameraFront == "environment") ||
                                       (hwCameraBack == "virtualscene");
    const bool backgroundUsesEnvironment = transparentDisplay;
    const bool environmentRequired =
            cameraUsesEnvironment || backgroundUsesEnvironment;

    if (!environmentRequired) {
        dinfo("%s: Environment scene is not required", __func__);
        return true;
    }

    struct ScopeTimer {
        ScopeTimer() { mStartTime = get_uptime_ms(); }
        ~ScopeTimer() {
            const uint64_t scopeTime = get_uptime_ms() - mStartTime;
            dprint("emulatorSetupEnvironment: took %llu ms", __func__, scopeTime);
        }
        int64_t mStartTime;
    } timer;

    // Environment is required, set it up
    CIniFile* environmentIni = avdInfo_getEnvironmentIni(avdInfo);
    if (!environmentIni) {
        // Not having an environment file is unexpected if it's not in
        // 'virtualscene' mode, defaults will be used
        if (hwCameraBack != "virtualscene") {
            dwarning("%s: No environment config is provided", __func__);
        } else {
            dinfo("%s: No environment config is provided", __func__);
        }
    }

    SceneConfig::Mode sceneMode = SceneConfig::Mode::Unknown;
    std::string sceneFilename;
    const float defaultBackgroundBlur = 5.0f;
    float backgroundBlur = defaultBackgroundBlur;

    if (environmentIni) {
        std::string backgroundImageFilename = iniFile_getString(
                environmentIni, "background.image.filename", "");
        std::string backgroundVideoFilename = iniFile_getString(
                environmentIni, "background.video.filename", "");
        std::string backgroundSceneFilename = iniFile_getString(
                environmentIni, "background.scene.filename", "");
        if (!backgroundImageFilename.empty()) {
            sceneMode = SceneConfig::Mode::ImageFile;
            sceneFilename = backgroundImageFilename;
        } else if (!backgroundVideoFilename.empty()) {
            sceneMode = SceneConfig::Mode::VideoPlayback;
            sceneFilename = backgroundVideoFilename;
        } else if (!backgroundSceneFilename.empty()) {
            sceneMode = SceneConfig::Mode::Mesh3dScene;
            sceneFilename = backgroundSceneFilename;
        }

        // Update blur amount from config, if given
        backgroundBlur = (float)iniFile_getDouble(
                environmentIni, "background.blurAmount", defaultBackgroundBlur);
    }

    if (sceneMode == SceneConfig::Mode::Unknown || sceneFilename.empty()) {
        dinfo("%s: Using default virtual scene contents for the environment.",
              __func__);
        sceneMode = SceneConfig::Mode::Mesh3dScene;
        sceneFilename = SceneConfig::defaultFilenameForMode(sceneMode);
    }

    // Initialize virtual scene and background view
    SceneConfig sceneConfig(sceneMode, sceneFilename);
    if (!VirtualSceneManager::initialize(sceneConfig)) {
        derror("%s: Cannot initialize virtual scene for the environment",
               __func__);
        return false;
    }

    if (backgroundUsesEnvironment) {
        dinfo("%s: Setting up screen background view", __func__);
        const int displayWidth = hwCfg->hw_lcd_width;
        const int displayHeight = hwCfg->hw_lcd_height;

        if (!BackgroundUpdateService::start(displayWidth, displayHeight,
                                            backgroundBlur)) {
            derror("%s: Cannot initialize background update service", __func__);
            return false;
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
