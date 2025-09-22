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

#include "android/emulation/control/battery_agent.h"

const QAndroidBatteryAgent sFishtankQAndroidBatteryAgent = {
        .setHasBattery = [](bool hasBattery) { NOT_IMPLEMENTED("QAndroidBatteryAgent.setHasBattery(hasBattery: %d)", hasBattery); },
        .hasBattery =
                []() {
                    NOT_IMPLEMENTED("QAndroidBatteryAgent.hasBattery");
                    return false;
                },
        .setIsBatteryPresent = [](bool present) { NOT_IMPLEMENTED("QAndroidBatteryAgent.setIsBatteryPresent(present: %d)", present); },
        .present =
                []() {
                    NOT_IMPLEMENTED("QAndroidBatteryAgent.present");
                    return false;
                },
        .setIsCharging = [](bool isCharging) { NOT_IMPLEMENTED("QAndroidBatteryAgent.setIsCharging(isCharging: %d)", isCharging); },
        .charging =
                []() {
                    NOT_IMPLEMENTED("QAndroidBatteryAgent.charging");
                    return false;
                },
        .setCharger = [](BatteryCharger charger) { NOT_IMPLEMENTED("QAndroidBatteryAgent.setCharger(charger: %d)", charger); },
        .charger =
                []() {
                    NOT_IMPLEMENTED("QAndroidBatteryAgent.charger");
                    return BATTERY_CHARGER_NONE;
                },
        .setChargeLevel = [](int level) { NOT_IMPLEMENTED("QAndroidBatteryAgent.setChargeLevel(level: %d)", level); },
        .chargeLevel =
                []() {
                    NOT_IMPLEMENTED("QAndroidBatteryAgent.chargeLevel");
                    return 0;
                },
        .setHealth = [](BatteryHealth health) { NOT_IMPLEMENTED("QAndroidBatteryAgent.setHealth(health: %d)", health); },
        .health =
                []() {
                    NOT_IMPLEMENTED("QAndroidBatteryAgent.health");
                    return BATTERY_HEALTH_UNKNOWN;
                },
        .setStatus = [](BatteryStatus status) { NOT_IMPLEMENTED("QAndroidBatteryAgent.setStatus(status: %d)", status); },
        .status =
                []() {
                    NOT_IMPLEMENTED("QAndroidBatteryAgent.status");
                    return BATTERY_STATUS_UNKNOWN;
                },
        .batteryDisplay =
                [](void* opaque, LineConsumerCallback cb) {
                    NOT_IMPLEMENTED("QAndroidBatteryAgent.batteryDisplay(opaque: %p, cb: %p)", opaque, cb);
                },
};
