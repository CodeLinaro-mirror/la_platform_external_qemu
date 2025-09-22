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

#include "android/emulation/control/hw_control_agent.h"

const QAndroidHwControlAgent sFishtankQAndroidHwControlAgent = {
        .setBrightness = [](const char* display,
                            uint32_t brightness) { NOT_IMPLEMENTED("QAndroidHwControlAgent.setBrightness(display: %s, brightness: %u)", display, brightness); },
        .getBrightness = [](const char* display) -> uint32_t {
            NOT_IMPLEMENTED("QAndroidHwControlAgent.getBrightness(display: %s)", display);
            return 0;
        },
        .setCallbacks =
                [](void* opaque, const AndroidHwControlFuncs* funcs) {
                    NOT_IMPLEMENTED("QAndroidHwControlAgent.setCallbacks(opaque: %p, funcs: %p)", opaque, funcs);
                },
};
