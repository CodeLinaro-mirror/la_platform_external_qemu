// Copyright (C) 2025 The Android Open Source Project
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
#include "fishtank_agents.h"

#include "android/emulation/control/telephony_agent.h"

const QAndroidTelephonyAgent sFishtankQAndroidTelephonyAgent = {
        .telephonyCmd =
                [](TelephonyOperation op, const char* arg) {
                    NOT_IMPLEMENTED("QAndroidTelephonyAgent.telephonyCmd(op: %d, arg: %s)", op, arg);
                    return Tel_Resp_OK;
                },
        .initModem = [](int line) { NOT_IMPLEMENTED("QAndroidTelephonyAgent.initModem(line: %d)", line); },
        .getModem = []() -> AModem {
            NOT_IMPLEMENTED("QAndroidTelephonyAgent.getModem");
            return nullptr;
        },
        .setNotifyCallback = [](ModemCallback cb,
                                void* opaque) { NOT_IMPLEMENTED("QAndroidTelephonyAgent.setNotifyCallback(cb: %p, opaque: %p)", cb, opaque); },
};
