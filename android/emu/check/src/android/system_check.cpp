// Copyright 2024 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#include "absl/strings/str_format.h"
#include "android/avd/info.h"
#include "android/base/system/System.h"
#include "android/emulation/compatibility_check.h"

#include "host-common/FeatureControl.h"  // for isEnabled

namespace android {
namespace emulation {

using android::base::System;

// A check to make sure various system properties (OS, CPU, RAM) are
// supported for the target AVD
AvdCompatibilityCheckResult hasSufficientSystem(AvdInfo* avd) {
    if (avd == nullptr) {
        return {
                .description =
                        "No avd present, cannot check for system capabilities",
                .status = AvdCompatibility::Error,
        };
    }

    auto sys = System::get();
    // Allow users and tests to skip compatibility checks
    if (sys->envGet("ANDROID_EMU_SKIP_SYSTEM_CHECKS") == "1") {
        return {
                .description = "System compatibility checks are disabled",
                .status = AvdCompatibility::Warning,
        };
    }
    if (sys->envGet("ANDROID_EMU_ABORT_SYSTEM_CHECKS") == "1") {
        return {
                .description =
                        "The user forced a compatibility error, unset "
                        "ANDROID_EMU_ABORT_SYSTEM_CHECKS environment variable "
                        "to launch the emulator",
                .status = AvdCompatibility::Error,
        };
    }

    const char* avdName = avdInfo_getName(avd);
    const bool isXrAvd = (avdInfo_getAvdFlavor(avd) == AVD_DEV_2024);

    // Check number of cores
    const int numCores = System::get()->getCpuCoreCount();
    const int minNumCores = isXrAvd ? 4 : 2;
    const int idealMinNumCores = isXrAvd ? 8 : 4;
    if (numCores < minNumCores) {
        // < 0.1% of our users as of November 2024
        return {
                .description =
                        absl::StrFormat("AVD '%s' requires %d CPU cores to "
                                        "run. Only %d cores are available.",
                                        avdName, numCores, minNumCores),
                .status = AvdCompatibility::Error,
        };
    } else if (numCores < idealMinNumCores) {
        // < 2% of our users as of November 2024
        return {
                .description =
                        absl::StrFormat("AVD '%s' will run more smoothly with "
                                        "%d CPU cores (currently using %d)",
                                        avdName, idealMinNumCores, numCores),
                .status = AvdCompatibility::Warning,
        };
    }

    // Check system RAM
    const android::base::MemUsage memUsage = System::get()->getMemUsage();
    if (memUsage.total_phys_memory == 0) {
        return {
                .description = absl::StrFormat(
                        "Unable to determine available system memory"),
                .status = AvdCompatibility::Warning,
        };
    }
    const uint64_t ramMB = (memUsage.total_phys_memory / (1024 * 1024));
    const uint64_t minRamMB = 2048;
    const uint64_t idealMinRamMB = isXrAvd ? 16384 : 4096;
    // < 5% of our users as of November 2024
    // TODO(b/376873919): Improve the reporting to account for avd requirements.
    if (ramMB < minRamMB) {
        return {
                .description = absl::StrFormat(
                        "Available system RAM is not enough to run "
                        "avd: '%s'. Available: %d MiB, minimum "
                        "required: %d MiB",
                        avdName, ramMB, minRamMB),
                .status = AvdCompatibility::Error,
        };
    } else if (ramMB < idealMinRamMB) {
        return {
                .description = absl::StrFormat(
                        "Suggested minimum system RAM to run "
                        "avd '%s' is %d MiB (available: %d MiB)",
                        avdName, idealMinRamMB, ramMB),
                .status = AvdCompatibility::Warning,
        };
    }

    return {
            .description = absl::StrFormat(
                    "System requirements to run avd: `%s` are met", avdName),
            .status = AvdCompatibility::Ok,
    };
}

REGISTER_COMPATIBILITY_CHECK(hasSufficientSystem);
}  // namespace emulation
}  // namespace android
