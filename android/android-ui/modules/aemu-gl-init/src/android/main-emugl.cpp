// Copyright 2016 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/main-emugl.h"

#include "aemu/base/memory/ScopedPtr.h"
#include "android/avd/util.h"
#include "android/console.h"
#include "android/opengl/gpuinfo.h"
#include "android/utils/debug.h"
#include "android/utils/string.h"
#include "host-common/FeatureControl.h"
#include "host-common/feature_control.h"
#include "host-common/opengles.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

using android::base::ScopedCPtr;
namespace fc = android::featurecontrol;

static const char DEFAULT_SOFTWARE_GPU_MODE[] = "lavapipe";

bool androidEmuglConfigInit(
        EmuglConfig* config,
        const char* gpuOption,
        const char* hwGpuModePtr,
        bool noWindow,
        enum WinsysPreferredGlesBackend uiPreferredBackend) {

    // Support old style gpu parameters for backwards compatibility
    if (gpuOption && !strcmp("swiftshader_indirect", gpuOption)) {
        gpuOption = "swiftshader";
    }
    if (gpuOption && !strcmp("swangle_indirect", gpuOption)) {
        gpuOption = "swangle";
    }

    std::string gpuChoice;
    if (gpuOption) {
        // It's enforced with -gpu option
        gpuChoice = gpuOption;
    } else if (uiPreferredBackend != WINSYS_GLESBACKEND_PREFERENCE_AUTO) {
        // Use UI preference
        switch (uiPreferredBackend) {
            case WINSYS_GLESBACKEND_PREFERENCE_ANGLE:
                gpuChoice = "swangle";
                break;
            case WINSYS_GLESBACKEND_PREFERENCE_SWIFTSHADER:
                gpuChoice = "swiftshader";
                break;
            case WINSYS_GLESBACKEND_PREFERENCE_NATIVEGL:
                gpuChoice = "host";
                break;
            default:
                gpuChoice = "auto";
        }
    } else if (hwGpuModePtr) {
        // Use hw gpu mode
        gpuChoice = hwGpuModePtr;
    } else {
        gpuChoice = "auto";
    }

    // "software" is a special term to select best software mode for the
    // platform/avd and not a real gpu backend mode.
    if (gpuChoice == "software") {
        gpuChoice = DEFAULT_SOFTWARE_GPU_MODE;
    }

    const std::vector<std::string> allowedOptions = {
            "auto", "host", "lavapipe", "swiftshader", "swangle",
    };
    bool validOption = false;
    for (auto& option : allowedOptions) {
        if (option == gpuChoice) {
            validOption = true;
            break;
        }
    }
    if (!validOption) {
        derror("%s: Selected GPU option '%s' is not valid, switching to "
               "'auto' mode.",
               __func__, gpuChoice.c_str());
        gpuChoice = "auto";
    }

    bool hostGpuDenylisted = false;

    // If the user has specified a renderer
    // that is neither "auto", "host" nor "on",
    // don't check the blacklist.
    // Only check the blacklist for 'auto', 'host' or 'on' mode.
    bool gpuChoiceAuto = (gpuChoice == "auto");
    bool gpuChoiceHost = (gpuChoice == "host");

    if (gpuChoiceAuto || gpuChoiceHost) {
        bool switchToSoftware = false;

        // Decide if a switch to software is needed
        const bool onDenyList = isHostGpuBlacklisted();
        if (onDenyList) {
            if (gpuChoiceAuto) {
                // Auto switch to software if denylisted, give warning
                dwarning(
                        "Your GPU drivers may have a bug. "
                        "Switching to software rendering.");
                switchToSoftware = true;
            } else {
                // We cannot use vulkan on this device, it's highly
                // likely that it'll crash with host GPU Overwrite
                // user's 'host' setting and use software rendering
                derror("Your GPU cannot be used for hardware rendering."
                       " Consider using software rendering.");
            }
        }

        if (switchToSoftware) {
            hostGpuDenylisted = onDenyList;
            setGpuBlacklistStatus(hostGpuDenylisted);
            gpuChoice = DEFAULT_SOFTWARE_GPU_MODE;
        }
    }

    // when set, 'force' feature flags will overwrite other options
    const bool force_lavapipe = fc::isEnabled(fc::ForceLavapipe);
    const bool force_swiftshader = fc::isEnabled(fc::ForceSwiftshader);
    const bool force_swangle = fc::isEnabled(fc::ForceANGLE);
    const bool force_lavapipe_on_software = fc::isEnabled(fc::ForceLavapipeForSoftwareRendering);

    // Select Vulkan mode
    if (force_lavapipe ||
        (force_lavapipe_on_software && (gpuChoice == "swiftshader" ||
                                        gpuChoice == "swangle"))) {
        gpuChoice = "lavapipe";
    } else if (force_swiftshader) {
        gpuChoice = "swiftshader";
    } else if (force_swangle) {
        gpuChoice = "swangle";
    }

    return emuglConfig_init(config, gpuChoice.c_str(), noWindow);
}
