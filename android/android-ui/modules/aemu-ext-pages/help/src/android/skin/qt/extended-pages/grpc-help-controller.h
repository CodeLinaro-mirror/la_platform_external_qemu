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

#pragma once

#include "android/skin/qt/extended-pages/help-controller.h"
#include "android/emulation/control/utils/EmulatorControlClient.h"

#include <memory>

class GrpcHelpController : public HelpController {
public:
    GrpcHelpController();
    explicit GrpcHelpController(
            std::shared_ptr<android::emulation::control::EmulatorControlClient> client);
    ~GrpcHelpController() override = default;

    HelpSystemInfo getSystemInfo() override;

private:
    std::shared_ptr<android::emulation::control::EmulatorControlClient>
            mEmulatorControl;
};
