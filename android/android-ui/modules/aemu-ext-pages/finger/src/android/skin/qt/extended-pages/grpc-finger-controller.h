// Copyright 2023 The Android Open Source Project
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

#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "android/skin/qt/extended-pages/finger-controller.h"
#include "emulator_controller.grpc.pb.h"

using android::emulation::control::EmulatorControlClient;
using android::emulation::control::EmulatorGrpcClient;
using android::emulation::control::Fingerprint;

class GrpcFingerController : public FingerController {
public:
    GrpcFingerController()
        : mClient(std::make_shared<EmulatorControlClient>(
                  EmulatorGrpcClient::me())) {}

    explicit GrpcFingerController(std::shared_ptr<EmulatorControlClient> client)
        : mClient(client) {}
    ~GrpcFingerController() = default;

    void sendTouchEvent(bool isTouching, int fingerId) override {
        Fingerprint fp;
        fp.set_istouching(isTouching);
        fp.set_touchid(fingerId);
        mClient->sendFingerprintAsync(fp);
    }

private:
    std::shared_ptr<EmulatorControlClient> mClient;
};