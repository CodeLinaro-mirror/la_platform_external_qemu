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
#pragma once
#include "aemu/base/logging/CLog.h"
#include "android/cmdline-definitions.h"
#include "host-common/window_agent.h"
#include <memory>
#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "android/emulation/control/utils/SensorClient.h"

// clang-format off
struct QAndroidAutomationAgent;
struct QAndroidBatteryAgent;
struct QCarDataAgent;
struct QAndroidCellularAgent;
struct QAndroidClipboardAgent;
struct QAndroidDisplayAgent;
struct QAndroidFingerAgent;
struct QAndroidGlobalVarsAgent;
struct QGrpcAgent;
struct QAndroidHwControlAgent;
struct QAndroidHttpProxyAgent;
struct QAndroidLibuiAgent;
struct QAndroidLocationAgent;
struct QAndroidMultiDisplayAgent;
struct QAndroidNetAgent;
struct QAndroidRecordScreenAgent;
struct QAndroidSensorsAgent;
struct QAndroidSurfaceAgent;
struct QAndroidTelephonyAgent;
struct QAndroidUserEventAgent;
struct QAndroidVirtualSceneAgent;
struct QAndroidVmOperations;
// clang-format on

#define DEBUG 1
#if DEBUG >= 1
#define NOT_IMPLEMENTED(fmt, ...) \
    derror("[NOT_IMPLEMENTED] FishtankAgents: " fmt, ##__VA_ARGS__);
#else
#define NOT_IMPLEMENTED(...)
#endif

extern const QAndroidAutomationAgent sFishtankQAndroidAutomationAgent;
extern const QAndroidBatteryAgent sFishtankQAndroidBatteryAgent;
extern const QCarDataAgent sFishtankQCarDataAgent;
extern const QAndroidCellularAgent sFishtankQAndroidCellularAgent;
extern const QAndroidClipboardAgent sFishtankQAndroidClipboardAgent;
extern const QAndroidDisplayAgent sFishtankQAndroidDisplayAgent;
extern const QAndroidFingerAgent sFishtankQAndroidFingerAgent;
extern const QAndroidGlobalVarsAgent sFishtankQAndroidGlobalVarsAgent;
extern const QGrpcAgent sFishtankQGrpcAgent;
extern const QAndroidHwControlAgent sFishtankQAndroidHwControlAgent;
extern const QAndroidHttpProxyAgent sFishtankQAndroidHttpProxyAgent;
extern const QAndroidLibuiAgent sFishtankQAndroidLibuiAgent;
extern const QAndroidLocationAgent sFishtankQAndroidLocationAgent;
extern const QAndroidMultiDisplayAgent sFishtankQAndroidMultiDisplayAgent;
extern const QAndroidNetAgent sFishtankQAndroidNetAgent;
extern const QAndroidRecordScreenAgent sFishtankQAndroidRecordScreenAgent;
extern const QAndroidSensorsAgent sFishtankQAndroidSensorsAgent;
extern const QAndroidTelephonyAgent sFishtankQAndroidTelephonyAgent;
extern const QAndroidUserEventAgent sFishtankQAndroidUserEventAgent;
extern const QAndroidVirtualSceneAgent sFishtankQAndroidVirtualSceneAgent;
extern const QAndroidVmOperations sFishtankQAndroidVmOperations;
extern "C" const QAndroidSurfaceAgent* const gQAndroidSurfaceAgent;


std::shared_ptr<android::emulation::control::EmulatorControlClient> getGlobalControlClient();
std::shared_ptr<android::emulation::control::SensorClient> getGlobalSensorClient();

void initializeGrpcUserEventAgent(
        android::emulation::control::EmulatorControlClient* client);
void injectFishtankConsoleAgents();
const QAndroidEmulatorWindowAgent* const getFishtankEmulatorWindowAgent();
