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

#include "host-common/record_screen_agent.h"

const QAndroidRecordScreenAgent sFishtankQAndroidRecordScreenAgent = {
        .startRecording =
                [](const RecordingInfo* info) {
                    NOT_IMPLEMENTED("QAndroidRecordScreenAgent.startRecording(info: %p)", info);
                    return false;
                },
        .startRecordingAsync =
                [](const RecordingInfo* info) {
                    NOT_IMPLEMENTED("QAndroidRecordScreenAgent.startRecordingAsync(info: %p)", info);
                    return false;
                },
        .stopRecording =
                []() {
                    NOT_IMPLEMENTED("QAndroidRecordScreenAgent.stopRecording");
                    return false;
                },
        .stopRecordingAsync =
                []() {
                    NOT_IMPLEMENTED("QAndroidRecordScreenAgent.stopRecordingAsync");
                    return false;
                },
        .getRecorderState =
                []() {
                    NOT_IMPLEMENTED("QAndroidRecordScreenAgent.getRecorderState");
                    return RecorderStates{RECORDER_STOPPED, 0};
                },
        .doSnap =
                [](const char* name, uint32_t displayId) {
                    NOT_IMPLEMENTED("QAndroidRecordScreenAgent.doSnap(name: %s, displayId: %u)", name, displayId);
                    return false;
                },
        .startSharedMemoryModule = [](int size) -> const char* {
            NOT_IMPLEMENTED("QAndroidRecordScreenAgent.startSharedMemoryModule(size: %d)", size);
            return nullptr;
        },
        .stopSharedMemoryModule =
                []() {
                    NOT_IMPLEMENTED("QAndroidRecordScreenAgent.stopSharedMemoryModule");
                    return false;
                },
};
