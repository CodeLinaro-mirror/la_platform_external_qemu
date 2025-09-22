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

#include "android/emulation/control/finger_agent.h"

const QAndroidFingerAgent sFishtankQAndroidFingerAgent = {
        .setTouch = [](bool is_down, int finger) { NOT_IMPLEMENTED("QAndroidFingerAgent.setTouch(is_down: %d, finger: %d)", is_down, finger); },
};
