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

static bool emulatorSetupEnvironment() {
    AndroidHwConfig* hwCfg = getConsoleAgents() && getConsoleAgents()->settings
                                     ? getConsoleAgents()->settings->hw()
                                     : nullptr;

    if (!hwCfg) {
        derror("%s: Invalid AVD config", __func__);
        return false;
    }

    int envWidth, envHeight;
    androidHwConfig_getScreenDimensions(hwCfg, &envWidth, &envHeight);
    int hwLcdWidth, hwLcdHeight;
    androidHwConfig_getLcdDimensions(hwCfg, &hwLcdWidth, &hwLcdHeight);
    dinfo("%s: Setting up screen background view and display layout at "
            "env:%dx%d, lcd:%dx%d",
            __func__, envWidth, envHeight, hwLcdWidth, hwLcdHeight);

    // Send layout parameters to the compositor when display position and size
    // should be adjusted, note that this should be done even when there are
    // errors with environment setup
    if (hwLcdWidth < envWidth && hwLcdHeight < envHeight) {
        // Center the display at it's original size
        int displayPosX = (envWidth - hwLcdWidth) / 2;
        int displayPosY = (envHeight - hwLcdHeight) / 2;
        android_setOpenglesDisplayLayout(envWidth, envHeight, displayPosX,
                                            displayPosY, hwLcdWidth,
                                            hwLcdHeight);
    }

    // Check if the camera is set to 'environment' or 'virtualscene'
    const std::string hwCameraBack = hwCfg->hw_camera_back;
    const std::string hwCameraFront = hwCfg->hw_camera_front;
    const bool transparentDisplay = hwCfg->hw_lcd_transparent;
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
            dprint("emulatorSetupEnvironment: took %llu ms", __func__,
                   scopeTime);
        }
        int64_t mStartTime;
    } timer;

    // Initialize virtual scene and background view
    if (!VirtualSceneManager::initialize(backgroundUsesEnvironment)) {
        derror("%s: Cannot initialize virtual scene for the environment",
               __func__);
        return false;
    }

    return true;
}

extern "C" {

bool emulator_window_load_environment() {
    return emulatorSetupEnvironment();
}
}
