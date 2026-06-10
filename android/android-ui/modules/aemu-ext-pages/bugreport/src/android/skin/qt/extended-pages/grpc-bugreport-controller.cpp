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

#include "android/skin/qt/extended-pages/grpc-bugreport-controller.h"

#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
// #include "android/skin/qt/qt-helpers.h"
#include "android/skin/qt/function-runner.h"
#include "emulator_controller.pb.h"

#include <fstream>

using android::base::PathUtils;
using android::base::System;
using android::emulation::control::EmulatorControlClient;
using android::emulation::control::EmulatorGrpcClient;
using android::emulation::control::Image;
using android::emulation::control::ImageFormat;

static const int kDefaultUnknownAPILevel = 1000;

GrpcBugreportController::GrpcBugreportController()
    : mEmulatorControl(std::make_unique<EmulatorControlClient>(
              EmulatorGrpcClient::me())) {}

GrpcBugreportController::GrpcBugreportController(
        std::shared_ptr<EmulatorControlClient> client)
    : mEmulatorControl(std::move(client)) {}

void GrpcBugreportController::takeScreenshot(
        std::function<void(const std::string&)> callback) {
    ImageFormat fmt;
    fmt.set_format(ImageFormat::PNG);
    mEmulatorControl->getScreenshotAsync(fmt, [callback](absl::StatusOr<Image*>
                                                                 status) {
        if (!status.ok()) {
            dwarning("Failed to obtain screenshot: %s",
                     status.status().message().data());
            runOnEmuUiThread([callback] { callback(""); });
            return;
        }

        auto screenshotPath =
                PathUtils::join(System::get()->getTempDir(), "screenshot.png");
        std::ofstream screenShotFile(
                PathUtils::asUnicodePath(screenshotPath.c_str()).c_str(),
                std::ios_base::out | std::ios_base::binary);

        if (!screenShotFile.is_open() || !screenShotFile.good()) {
            dwarning("Failed to save screenshot to %s. ",
                     screenshotPath.c_str());
            runOnEmuUiThread([callback] { callback(""); });
            return;
        }

        screenShotFile << status.value()->image();
        runOnEmuUiThread(
                [callback, screenshotPath] { callback(screenshotPath); });
    });
}

void GrpcBugreportController::isBootCompleted(
        std::function<void(bool)> callback) {
    mEmulatorControl->getEmulatorStatusAsync(
            [callback](
                    absl::StatusOr<android::emulation::control::EmulatorStatus*>
                            status) {
                bool booted = false;
                if (status.ok()) {
                    booted = status.value()->booted();
                }
                runOnEmuUiThread([callback, booted] { callback(booted); });
            });
}

BugreportInfo GrpcBugreportController::getSystemInfo() {
    // This is a blocking call, which is not ideal, but the original code
    // was also blocking for this information.
    auto context = mEmulatorControl->client()->newContext();
    ::google::protobuf::Empty empty;
    android::emulation::control::EmulatorStatus emulatorStatus;

    auto status = mEmulatorControl->service()->getStatus(context.get(), empty,
                                                         &emulatorStatus);
    BugreportInfo info;
    if (!status.ok()) {
        derror("Failed to get system info from gRPC: %s",
               status.error_message());
        return info;
    }

    info.emulatorVersion = emulatorStatus.version();
    info.hostOsName = System::get()->getOsName();

    for (const auto& entry : emulatorStatus.hardwareconfig().entry()) {
        if (entry.key() == "avd.api_level") {
            info.apiLevel = std::stoi(entry.value());
        }
        if (info.deviceName.empty() &&
            (entry.key() == "AvdId" || entry.key() == "avd.id" || entry.key() == "avd.name")) {
            info.deviceName = entry.value();
        }
    }

    auto it = emulatorStatus.guestconfig().find("androidVersion");
    if (it != emulatorStatus.guestconfig().end()) {
        info.androidVersion = it->second;
    }
    it = emulatorStatus.guestconfig().find("hypervisorVersion");
    if (it != emulatorStatus.guestconfig().end()) {
        info.hypervisorVersion = it->second;
    }
    it = emulatorStatus.guestconfig().find("avdDetails");
    if (it != emulatorStatus.guestconfig().end()) {
        info.avdDetails = it->second;
    }


    if (info.apiLevel == 0) {
        info.apiLevel = kDefaultUnknownAPILevel;
    }

    return info;
}
