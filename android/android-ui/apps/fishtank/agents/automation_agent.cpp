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

#include "aemu/base/Result.h"
#include "android/emulation/control/automation_agent.h"

#include <string>
#include <string_view>

const QAndroidAutomationAgent sFishtankQAndroidAutomationAgent = {
        .reset = []() { NOT_IMPLEMENTED("QAndroidAutomationAgent.reset()"); },
        .startRecording =
                [](std::string_view sv) -> android::automation::StartResult {
            NOT_IMPLEMENTED("QAndroidAutomationAgent.startRecording(sv: '%.*s')",
                          (int)sv.length(), sv.data());
            return android::base::Ok();
        },
        .stopRecording = []() -> android::automation::StopResult {
            NOT_IMPLEMENTED("QAndroidAutomationAgent.stopRecording()");
            return android::base::Ok();
        },
        .startPlayback =
                [](std::string_view sv) -> android::automation::StartResult {
            NOT_IMPLEMENTED("QAndroidAutomationAgent.startPlayback(sv: '%.*s')",
                          (int)sv.length(), sv.data());
            return android::base::Ok();
        },
        .stopPlayback = []() -> android::automation::StopResult {
            NOT_IMPLEMENTED("QAndroidAutomationAgent.stopPlayback()");
            return android::base::Ok();
        },
        .startPlaybackWithCallback =
                [](std::string_view sv,
                   void (*cb)()) -> android::automation::StartResult {
            NOT_IMPLEMENTED(
                    "QAndroidAutomationAgent.startPlaybackWithCallback(sv: "
                    "'%.*s', cb: %p)",
                    (int)sv.length(), sv.data(), cb);
            return android::base::Ok();
        },
        .setMacroName = [](std::string_view macro, std::string_view name) {
            NOT_IMPLEMENTED(
                    "QAndroidAutomationAgent.setMacroName(macro: '%.*s', name: "
                    "'%.*s')",
                    (int)macro.length(), macro.data(), (int)name.length(),
                    name.data());
        },
        .getMacroName = [](std::string_view sv) {
            NOT_IMPLEMENTED("QAndroidAutomationAgent.getMacroName(sv: '%.*s')",
                          (int)sv.length(), sv.data());
                    return std::string();
                },
        .getMetadata = [](std::string_view sv) -> std::pair<uint64_t, uint64_t> {
            NOT_IMPLEMENTED("QAndroidAutomationAgent.getMetadata(sv: '%.*s')",
                          (int)sv.length(), sv.data());
            return {0, 0};
        },
};
