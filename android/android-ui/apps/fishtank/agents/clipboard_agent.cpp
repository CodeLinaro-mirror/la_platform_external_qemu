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

#include "android/emulation/control/clipboard_agent.h"

const QAndroidClipboardAgent sFishtankQAndroidClipboardAgent = {
        .setEnabled = [](bool enabled) { NOT_IMPLEMENTED("QAndroidClipboardAgent.setEnabled(enabled: %d)", enabled); },
        .registerGuestClipboardCallback =
                [](void (*cb)(void*, const uint8_t*, size_t), void* opaque) {
                    NOT_IMPLEMENTED("QAndroidClipboardAgent.registerGuestClipboardCallback(cb: %p, opaque: %p)", cb, opaque);
                },
        .setGuestClipboardContents =
                [](const uint8_t* data, size_t size) {
                    NOT_IMPLEMENTED("QAndroidClipboardAgent.setGuestClipboardContents(data: %p, size: %zu)", data, size);
                },
};
