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

#include <set>

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

static std::set<uint32_t> sOpenedDisplays;

void initializeGrpcNotifications(
        android::emulation::control::EmulatorControlClient* client) {
    client->registerNotificationListener(
            [](const android::emulation::control::Notification* event) {
                if (event && event->has_displayconfigurationschangednotification()) {
                    const auto& displayConfigs =
                            event->displayconfigurationschangednotification()
                                    .displayconfigurations();

                    std::set<uint32_t> activeIds;
                    for (const auto& display : displayConfigs.displays()) {
                        if (display.display() > 0) {
                            activeIds.insert(display.display());
                        }
                    }

                    const auto windowAgent = getFishtankEmulatorWindowAgent();
                    if (!windowAgent) {
                        derror("FishtankAgents: No window agent to apply display changes");
                        return;
                    }

                    // Open new displays
                    for (uint32_t id : activeIds) {
                        if (sOpenedDisplays.count(id) == 0) {
                            // Find config to get width/height
                            for (const auto& display : displayConfigs.displays()) {
                                if (display.display() == id) {
                                    windowAgent->addMultiDisplayWindow(
                                            id, true, display.width(),
                                            display.height());
                                    windowAgent->updateUIMultiDisplayPage(id);
                                    sOpenedDisplays.insert(id);
                                    break;
                                }
                            }
                        }
                    }

                    // Close removed displays
                    for (auto it = sOpenedDisplays.begin();
                         it != sOpenedDisplays.end();) {
                        uint32_t id = *it;
                        if (activeIds.count(id) == 0) {
                            windowAgent->addMultiDisplayWindow(id, false, 0, 0);
                            windowAgent->updateUIMultiDisplayPage(id);
                            it = sOpenedDisplays.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
            },
            [](absl::Status status) {
                dwarning("FishtankAgents: Notification stream finished with status: %s",
                         status.ToString().c_str());
            });
}