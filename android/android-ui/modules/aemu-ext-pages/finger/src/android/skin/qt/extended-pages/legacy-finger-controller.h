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

#include "android/skin/qt/extended-pages/finger-controller.h"
#include "aemu/base/async/Looper.h"
#include "aemu/base/async/ThreadLooper.h"
#include "android/emulation/control/finger_agent.h"

class LegacyFingerController : public FingerController {
public:
    explicit LegacyFingerController(const QAndroidFingerAgent* agent) : mAgent(agent) {}
    ~LegacyFingerController() = default;

    void sendTouchEvent(bool isTouching, int fingerId) override {
        if (mAgent && mAgent->setTouch) {
            const auto* agent = mAgent;
            android::base::ThreadLooper::runOnMainLooper(
                [agent, isTouching, fingerId]() {
                    agent->setTouch(isTouching, fingerId);
                });
        }
    }

private:
    const QAndroidFingerAgent* mAgent;
};
