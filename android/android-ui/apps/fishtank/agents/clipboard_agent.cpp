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

#include <string>
#include "android/emulation/control/clipboard_agent.h"

bool sEnabled = true;

const QAndroidClipboardAgent sFishtankQAndroidClipboardAgent = {
        .setEnabled = [](bool enabled) { sEnabled = enabled; },
        .registerGuestClipboardCallback =
                [](void (*cb)(void*, const uint8_t*, size_t), void* opaque) {
                    getGlobalControlClient()->streamClipboardAsync(
                            [cb, opaque](auto clipdata) {
                                auto text = clipdata->text();
                                cb(opaque, (uint8_t*)text.c_str(), text.size());
                            },
                            [](auto status) {
                                LOG(INFO) << "Clipboard closed: " << status;
                            });
                },
        .setGuestClipboardContents =
                [](const uint8_t* data, size_t size) {
                    getGlobalControlClient()->setClipboardAsync(
                            std::string((char*)data, size));
                },
};
