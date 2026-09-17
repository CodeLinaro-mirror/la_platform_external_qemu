// Copyright 2026 The Android Open Source Project
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

#include "android/skin/qt/extended-pages/grpc-help-controller.h"

#include "aemu/base/StringFormat.h"
#include "aemu/base/misc/StringUtils.h"
#include "android/base/system/System.h"
#include "android/emulation/ComponentVersion.h"
#include "android/emulation/ConfigDirs.h"
#include "android/emulation/CpuAccelerator.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "android/metrics/StudioConfig.h"
#include "android/utils/debug.h"
#include "android/utils/system.h"
#include "emulator_controller.pb.h"

using android::base::System;
using android::emulation::control::EmulatorControlClient;
using android::emulation::control::EmulatorGrpcClient;

GrpcHelpController::GrpcHelpController()
    : mEmulatorControl(std::make_unique<EmulatorControlClient>(
              EmulatorGrpcClient::me())) {}

GrpcHelpController::GrpcHelpController(
        std::shared_ptr<EmulatorControlClient> client)
    : mEmulatorControl(std::move(client)) {}

HelpSystemInfo GrpcHelpController::getSystemInfo() {
    HelpSystemInfo info;
    if (!mEmulatorControl || !mEmulatorControl->client()) {
        return info;
    }
    auto context = mEmulatorControl->client()->newContext();
    ::google::protobuf::Empty empty;
    android::emulation::control::EmulatorStatus emulatorStatus;

    auto status = mEmulatorControl->service()->getStatus(context.get(), empty,
                                                         &emulatorStatus);
    if (!status.ok()) {
        derror("Failed to get system info from gRPC: %s",
               status.error_message());
        return info;
    }

    info.emulatorVersion = emulatorStatus.version();

    auto it = emulatorStatus.guestconfig().find("androidVersion");
    if (it != emulatorStatus.guestconfig().end()) {
        info.androidVersion = it->second;
    }

    android::base::Version studioVersion =
            android::studio::lastestAndroidStudioVersion();
    std::string androidStudioVer =
            studioVersion.isValid() ? studioVersion.toString() : "Unknown";

    std::string emulatorVer =
            info.emulatorVersion.empty() ? "Unknown" : info.emulatorVersion;

    std::string hypervisorVer = "None";
    it = emulatorStatus.guestconfig().find("hypervisorVersion");
    if (it != emulatorStatus.guestconfig().end()) {
        hypervisorVer = it->second;
    }

    std::string sdkToolsVer =
            android::getCurrentSdkVersion(
                    android::ConfigDirs::getSdkRootDirectory(),
                    android::SdkComponentType::Tools)
                    .toString();

    std::string hostOsName;
    it = emulatorStatus.guestconfig().find("hostOsName");
    if (it != emulatorStatus.guestconfig().end() && !it->second.empty()) {
        hostOsName = it->second;
    } else {
        hostOsName = System::get()->getOsName();
    }

    std::string cpuModel;
    it = emulatorStatus.guestconfig().find("cpuModel");
    if (it != emulatorStatus.guestconfig().end() && !it->second.empty()) {
        cpuModel = it->second;
    } else {
        cpuModel = android::base::trim(android::GetCpuInfo().second);
    }

    std::string totalMem;
    it = emulatorStatus.guestconfig().find("totalMem");
    if (it != emulatorStatus.guestconfig().end() && !it->second.empty()) {
        totalMem = it->second;
    } else {
        auto usage = System::get()->getMemUsage();
        totalMem = android::base::StringFormat(
                "%d", (int)(usage.total_phys_memory / 1048576.0f));
    }

    std::string gpuInfo;
    it = emulatorStatus.guestconfig().find("gpu");
    if (it != emulatorStatus.guestconfig().end()) {
        gpuInfo = it->second;
    } else {
        auto pit = emulatorStatus.platformconfig().find("hw.gpu.mode");
        if (pit != emulatorStatus.platformconfig().end()) {
            gpuInfo = pit->second;
        }
    }

    std::string buildFingerprint;
    it = emulatorStatus.guestconfig().find("buildFingerprint");
    if (it != emulatorStatus.guestconfig().end()) {
        buildFingerprint = it->second;
    }

    std::string avdDetails;
    it = emulatorStatus.guestconfig().find("avdDetails");
    if (it != emulatorStatus.guestconfig().end()) {
        avdDetails = it->second;
    }

    info.feedbackReport = android::base::StringFormat(
            R"(Please Read:
https://developer.android.com/studio/report-bugs.html#emulator-bugs

Android Studio Version: %s

Emulator Version (Emulator--> Extended Controls--> Emulator Version): %s
Hypervisor Version: %s

Android SDK Tools: %s

Host Operating System: %s

CPU Manufacturer: %s

RAM: %s MB

GPU: %s

Build Fingerprint: %s

AVD Details: %s
)",
            androidStudioVer, emulatorVer, hypervisorVer, sdkToolsVer,
            hostOsName, cpuModel, totalMem, gpuInfo, buildFingerprint,
            avdDetails);

    return info;
}

