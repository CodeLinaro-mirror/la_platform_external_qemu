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

#include "android/emulation/control/AndroidAgentFactory.h"
#include "android/emulation/control/automation_agent.h"
#include "android/emulation/control/battery_agent.h"
#include "android/emulation/control/car_data_agent.h"
#include "android/emulation/control/cellular_agent.h"
#include "android/emulation/control/clipboard_agent.h"
#include "android/emulation/control/finger_agent.h"
#include "android/emulation/control/globals_agent.h"
#include "android/emulation/control/grpc_agent.h"
#include "android/emulation/control/http_proxy_agent.h"
#include "android/emulation/control/hw_control_agent.h"
#include "android/emulation/control/libui_agent.h"
#include "android/emulation/control/location_agent.h"
#include "android/emulation/control/net_agent.h"
#include "android/emulation/control/sensors_agent.h"
#include "android/emulation/control/surface_agent.h"
#include "android/emulation/control/telephony_agent.h"
#include "android/emulation/control/user_event_agent.h"
#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "android/emulation/control/virtual_scene_agent.h"
#include "host-common/display_agent.h"
#include "host-common/multi_display_agent.h"
#include "host-common/record_screen_agent.h"
#include "host-common/vm_operations.h"
#include "host-common/window_agent.h"

#define ANDROID_AGENTS_LIST(X)   \
    X(QAndroidAutomationAgent)   \
    X(QAndroidBatteryAgent)      \
    X(QAndroidClipboardAgent)    \
    X(QAndroidCellularAgent)     \
    X(QAndroidDisplayAgent)      \
    X(QAndroidFingerAgent)       \
    X(QAndroidHttpProxyAgent)    \
    X(QAndroidLocationAgent)     \
    X(QAndroidMultiDisplayAgent) \
    X(QAndroidNetAgent)          \
    X(QAndroidRecordScreenAgent) \
    X(QAndroidSensorsAgent)      \
    X(QAndroidTelephonyAgent)    \
    X(QAndroidUserEventAgent)    \
    X(QAndroidVirtualSceneAgent) \
    X(QAndroidVmOperations)      \
    X(QCarDataAgent)             \
    X(QGrpcAgent)                \
    X(QAndroidHwControlAgent)    \
    X(QAndroidGlobalVarsAgent)

class FishtankAgentConsoleFactory
    : public android::emulation::AndroidConsoleFactory {
public:
#define ANDROID_DEFINE_CONSOLE_GETTER_IMPL(typ) \
    const typ* android_get_##typ() const override { return &sFishtank##typ; };

    ANDROID_AGENTS_LIST(ANDROID_DEFINE_CONSOLE_GETTER_IMPL)

    const QAndroidEmulatorWindowAgent* android_get_QAndroidEmulatorWindowAgent()
            const override {
        return getFishtankEmulatorWindowAgent();
    }

    const QAndroidLibuiAgent* android_get_QAndroidLibuiAgent() const override {
        return &sFishtankQAndroidLibuiAgent;
    }

    const QAndroidSurfaceAgent* android_get_QAndroidSurfaceAgent()
            const override {
        // This points to the qtSurfaceAgent implementation.
        return gQAndroidSurfaceAgent;
    }
};

void injectFishtankConsoleAgents() {
    android::emulation::injectConsoleAgents(FishtankAgentConsoleFactory());
}