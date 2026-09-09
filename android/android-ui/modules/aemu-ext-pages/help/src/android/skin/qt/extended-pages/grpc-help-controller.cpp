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

#include "android/emulation/control/utils/EmulatorGrcpClient.h"

using android::emulation::control::EmulatorControlClient;
using android::emulation::control::EmulatorGrpcClient;

GrpcHelpController::GrpcHelpController()
    : mEmulatorControl(std::make_unique<EmulatorControlClient>(
              EmulatorGrpcClient::me())) {}

GrpcHelpController::GrpcHelpController(
        std::shared_ptr<EmulatorControlClient> client)
    : mEmulatorControl(std::move(client)) {}

HelpSystemInfo GrpcHelpController::getSystemInfo() {
    // Stub implementation returning empty info for TDD Red phase.
    return {};
}
