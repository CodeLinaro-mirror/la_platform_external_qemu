// Copyright 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "android/skin/qt/extended-pages/legacy-bugreport-controller.h"

#include "absl/strings/str_format.h"
#include "aemu/base/StringFormat.h"
#include "aemu/base/files/PathUtils.h"
#include "android/avd/BugreportInfo.h"
#include "android/avd/info.h"
#include "android/base/system/System.h"
#include "android/console.h"
#include "android/emulation/CpuAccelerator.h"
#include "android/emulation/control/ScreenCapturer.h"
#include "android/skin/qt/function-runner.h"
#include "android/update-check/VersionExtractor.h"
#include "android/version.h"

using android::base::PathUtils;
using android::base::StringAppendFormat;
using android::base::System;
using android::base::Version;
using android::update_check::VersionExtractor;
using BugInfo = android::avd::BugreportInfo;

static const int kDefaultUnknownAPILevel = 1000;

LegacyBugreportController::LegacyBugreportController() {}

void LegacyBugreportController::takeScreenshot(
        std::function<void(const std::string&)> callback) {
    // This is a blocking call, but it's fast. We'll wrap it to be async-like.
    runOnEmuUiThread([callback] {
        std::string screenshotPath;
        int displayId = 0;
        // TODO: Handle foldable display correctly.
        if (android::emulation::captureScreenshot(
                    System::get()->getTempDir().c_str(), &screenshotPath,
                    displayId)) {
            callback(screenshotPath);
        } else {
            callback("");
        }
    });
}

void LegacyBugreportController::isBootCompleted(
        std::function<void(bool)> callback) {
    // This is also a fast check.
    runOnEmuUiThread([callback] {
        callback(getConsoleAgents()->settings->guest_boot_completed());
    });
}

BugreportInfo LegacyBugreportController::getSystemInfo() {
    BugInfo buginfo;
    BugreportInfo info;
    const auto* avdInfo = getConsoleAgents()->settings->avdInfo();
    info.emulatorVersion = buginfo.emulatorVer;
    info.hypervisorVersion = buginfo.hypervisorVer;
    info.androidVersion = buginfo.androidVer;
    info.deviceName = buginfo.deviceName;
    info.hostOsName = buginfo.hostOsName;
    info.avdDetails = buginfo.avdDetails;
    info.apiLevel = avdInfo_getApiLevel(avdInfo);
    if (info.apiLevel == 0) {
        info.apiLevel = kDefaultUnknownAPILevel;
    }
    return info;
}
