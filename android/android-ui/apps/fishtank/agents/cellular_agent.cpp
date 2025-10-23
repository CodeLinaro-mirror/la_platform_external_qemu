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

#include "android/emulation/control/cellular_agent.h"

const QAndroidCellularAgent sFishtankQAndroidCellularAgent = {
        .setSignalStrength = [](int strength) { NOT_IMPLEMENTED("QAndroidCellularAgent.setSignalStrength(strength: %d)", strength); },
        .setSignalStrengthProfile =
                [](CellularSignal profile) { NOT_IMPLEMENTED("QAndroidCellularAgent.setSignalStrengthProfile(profile: %d)", profile); },
        .setVoiceStatus =
                [](CellularStatus status) { NOT_IMPLEMENTED("QAndroidCellularAgent.setVoiceStatus(status: %d)", status); },
        .setMeterStatus =
                [](CellularMeterStatus status) { NOT_IMPLEMENTED("QAndroidCellularAgent.setMeterStatus(status: %d)", status); },
        .setDataStatus =
                [](CellularStatus status) { NOT_IMPLEMENTED("QAndroidCellularAgent.setDataStatus(status: %d)", status); },
        .setStandard =
                [](CellularStandard standard) { NOT_IMPLEMENTED("QAndroidCellularAgent.setStandard(standard: %d)", standard); },
        .setSimPresent = [](bool present) { NOT_IMPLEMENTED("QAndroidCellularAgent.setSimPresent(present: %d)", present); },
};
