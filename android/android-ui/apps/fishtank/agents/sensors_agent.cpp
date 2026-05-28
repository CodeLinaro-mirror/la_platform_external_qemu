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
#include "android/hw-sensors.h"

#include "android/emulation/control/sensors_agent.h"
#include "android/physics/physical_state_agent.h"
#include "emulator_controller.pb.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using android::emulation::control::PhysicalModelValue;
using android::emulation::control::ParameterValue;

#define DEBUG 0
#if DEBUG >= 1
#define DPRINT(fmt, ...) \
    dinfo("FishtankAgents (Sensors): " fmt, ##__VA_ARGS__);
#else
#define DPRINT(...)
#endif

/*
 * The Sensors Agent in Fishtank redirects sensor and physical model interactions
 * to a remote emulator instance over gRPC.
 *
 * Fishtank notifies its UI when the physical state (e.g., device rotation) changes
 * by subscribing to PhysicalStateEvents via the SensorService gRPC API.
 *
 * NOTE: This implementation currently only supports a single registration cycle
 * (one setPhysicalStateAgent call with a valid agent). It does not track or
 * cancel previous gRPC streams if setPhysicalStateAgent is called multiple times
 * with different valid agents.
 */

static QAndroidPhysicalStateAgent gPhysicalStateAgent = {};

// Protects gPhysicalStateAgent access. Callbacks can arrive on gRPC threads
// while the agent is being registered/unregistered on the main UI thread.
static std::mutex gAgentMutex;

static void subscribeToPhysicalStateEvents(
        android::emulation::control::SensorClient* sensorClient) {
    DPRINT("Attempting to subscribe to PhysicalStateEvents via SensorService...");
    sensorClient->receivePhysicalStateEvents(
            /* incoming: Called every time a new event is received from the stream */
            [](const android::emulation::control::incubating::PhysicalStateEvent* event) {
                if (!event)
                    return;
                DPRINT("Received PhysicalStateEvent: %d", (int)event->event());

                std::lock_guard<std::mutex> lock(gAgentMutex);
                switch (event->event()) {
                    case android::emulation::control::incubating::PhysicalStateEvent::STATE_EVENT_UNDEFINED:
                        break;
                    case android::emulation::control::incubating::PhysicalStateEvent::STATE_PHYSICAL_STATE_CHANGING:
                        if (gPhysicalStateAgent.onPhysicalStateChanging) {
                            gPhysicalStateAgent.onPhysicalStateChanging(
                                    gPhysicalStateAgent.context);
                        }
                        break;
                    case android::emulation::control::incubating::PhysicalStateEvent::STATE_PHYSICAL_STATE_STABILIZED:
                        if (gPhysicalStateAgent.onPhysicalStateStabilized) {
                            gPhysicalStateAgent.onPhysicalStateStabilized(
                                    gPhysicalStateAgent.context);
                        }
                        break;
                    case android::emulation::control::incubating::PhysicalStateEvent::STATE_TARGET_STATE_CHANGED:
                        if (gPhysicalStateAgent.onTargetStateChanged) {
                            gPhysicalStateAgent.onTargetStateChanged(
                                    gPhysicalStateAgent.context);
                        }
                        break;
                }
            },
            /* onDone: Called when the stream is closed or an error occurs */
            [](absl::Status status) {
                if (!status.ok()) {
                    derror("SensorService stream error: %s", status.ToString().c_str());
                }
            });
}

const QAndroidSensorsAgent sFishtankQAndroidSensorsAgent = {
        .setPhysicalParameterTarget =
                [](int parameter, const float* value, size_t len, int interpolation_method) {
                    DPRINT("setPhysicalParameterTarget(parameter: %d, len: %zu)", parameter, len);
                    auto client = getGlobalControlClient();
                    if (!client) return -1;

                    {
                        std::lock_guard<std::mutex> lock(gAgentMutex);
                        // Manually trigger "changing" callback for instant local feedback
                        if (gPhysicalStateAgent.onPhysicalStateChanging) {
                            gPhysicalStateAgent.onPhysicalStateChanging(gPhysicalStateAgent.context);
                        }
                    }

                    PhysicalModelValue request;
                    request.set_target((PhysicalModelValue::PhysicalType)parameter);

                    PhysicalModelValue::Interpolation interpolation;
                    switch (interpolation_method) {
                        case PHYSICAL_INTERPOLATION_STEP:
                            interpolation = PhysicalModelValue::STEP;
                            break;
                        case PHYSICAL_INTERPOLATION_SMOOTH:
                            interpolation = PhysicalModelValue::SMOOTH;
                            break;
                        default:
                            derror("Unknown interpolation method: %d",
                                   interpolation_method);
                            return -1;
                    }
                    request.set_interpolation(interpolation);

                    auto val = request.mutable_value();
                    for (size_t i = 0; i < len; i++) {
                        val->add_data(value[i]);
                    }

                    auto context = client->client()->newContext();
                    google::protobuf::Empty response;
                    auto status = client->service()->setPhysicalModel(context.get(), request, &response);

                    {
                        std::lock_guard<std::mutex> lock(gAgentMutex);
                        // Manually trigger "target changed" callback
                        if (gPhysicalStateAgent.onTargetStateChanged) {
                            gPhysicalStateAgent.onTargetStateChanged(gPhysicalStateAgent.context);
                        }
                    }

                    return status.ok() ? 0 : -1;
                },
        .getPhysicalParameter =
                [](int parameter, float* const* value, size_t len, int parameterValueType) {
                    DPRINT("getPhysicalParameter(parameter: %d, len: %zu, type: %d)", parameter, len, parameterValueType);
                    auto client = getGlobalControlClient();
                    if (!client) return -1;

                    PhysicalModelValue request;
                    request.set_target((PhysicalModelValue::PhysicalType)parameter);
                    // Note: emulator_controller.proto doesn't support parameterValueType yet.

                    PhysicalModelValue response;
                    auto context = client->client()->newContext();
                    auto status = client->service()->getPhysicalModel(context.get(), request, &response);
                    if (!status.ok()) return -1;

                    auto data = response.value().data();
                    for (size_t i = 0; i < len && i < (size_t)data.size(); i++) {
                        if (value[i]) {
                            *value[i] = data.Get(i);
                        }
                    }
                    return (int)response.status();
                },
        .getPhysicalParameterSize =
                [](int parameter, size_t* size) {
                    DPRINT("getPhysicalParameterSize(parameter: %d)", parameter);
                    if (!size) return 0;
#define VALUE_SIZE_float 1
#define VALUE_SIZE_vec3 3
#define VALUE_SIZE_vec4 4
#define PHYSICAL_PARAMETER_(x, y, z, w) \
    case PHYSICAL_PARAMETER_##x:        \
        *size = VALUE_SIZE_##w;         \
        return 0;

                    switch (parameter) {
                        PHYSICAL_PARAMETERS_LIST
                        case MAX_PHYSICAL_PARAMETERS:
                            break;
                    }
                    derror("Unknown physical parameter: %d", parameter);
                    return -1;
#undef PHYSICAL_PARAMETER_
#undef VALUE_SIZE_float
#undef VALUE_SIZE_vec3
#undef VALUE_SIZE_vec4
                },

        .setCoarseOrientation =
                [](int orientation) {
                    DPRINT("setCoarseOrientation(orientation: %d)",
                           orientation);
                    float rotation[3] = {0, 0, 0};
                    switch (orientation) {
                        case ANDROID_COARSE_PORTRAIT:
                            rotation[2] = 0.0f;
                            break;
                        case ANDROID_COARSE_REVERSE_LANDSCAPE:
                            rotation[2] = -90.0f;
                            break;
                        case ANDROID_COARSE_REVERSE_PORTRAIT:
                            rotation[2] = 180.0f;
                            break;
                        case ANDROID_COARSE_LANDSCAPE:
                            rotation[2] = 90.0f;
                            break;
                        default:
                            return -1;
                    }

                    return sFishtankQAndroidSensorsAgent
                            .setPhysicalParameterTarget(
                                    PHYSICAL_PARAMETER_ROTATION, rotation, 3,
                                    PHYSICAL_INTERPOLATION_STEP);
                },
        .setSensorOverride =
                [](int sensor, const float* value, size_t len) {
                    DPRINT("setSensorOverride(sensor: %d, len: %zu)", sensor,
                           len);
                    auto client = getGlobalControlClient();
                    if (!client) return -1;

                    android::emulation::control::SensorValue request;
                    request.set_target(
                            (android::emulation::control::SensorValue::SensorType)
                                    sensor);
                    auto val = request.mutable_value();
                    for (size_t i = 0; i < len; i++) {
                        val->add_data(value[i]);
                    }

                    auto context = client->client()->newContext();
                    google::protobuf::Empty response;
                    auto status =
                            client->service()->setSensor(context.get(), request,
                                                         &response);

                    return status.ok() ? 0 : -1;
                },
        .getSensor =
                [](int sensor, float* const* value, size_t len) {
                    DPRINT("getSensor(sensor: %d, len: %zu)", sensor, len);
                    auto client = getGlobalControlClient();
                    if (!client) return -1;

                    android::emulation::control::SensorValue request;
                    request.set_target(
                            (android::emulation::control::SensorValue::SensorType)
                                    sensor);

                    android::emulation::control::SensorValue response;
                    auto context = client->client()->newContext();
                    auto status = client->service()->getSensor(context.get(), request,
                                                               &response);
                    if (!status.ok()) return -1;

                    auto data = response.value().data();
                    for (size_t i = 0; i < len && i < (size_t)data.size(); i++) {
                        if (value[i]) {
                            *value[i] = data.Get(i);
                        }
                    }
                    return (int)response.status();
                },
        .getSensorSize =
                [](int sensor, size_t* size) {
                    DPRINT("getSensorSize(sensor: %d)", sensor);
                    if (!size) return 0;
#define VALUE_SIZE_float 1
#define VALUE_SIZE_vec3 3
#define VALUE_SIZE_vec4 4
#define SENSOR_(x, y, z, v, w) \
    case ANDROID_SENSOR_##x:   \
        *size = VALUE_SIZE_##v; \
        return 0;

                    switch (sensor) {
                        SENSORS_LIST
                        case MAX_SENSORS:
                            break;
                    }
                    derror("Unknown sensor: %d", sensor);
                    return -1;
#undef SENSOR_
#undef VALUE_SIZE_float
#undef VALUE_SIZE_vec3
#undef VALUE_SIZE_vec4
                },
        .getDelayMs =
                []() {
                    DPRINT("getDelayMs()");
                    // We return 0 here because there is currently no gRPC API
                    // available to retrieve the sensor update delay from the
                    // backend.
                    return 0;
                },
        .setPhysicalStateAgent =
                [](const struct QAndroidPhysicalStateAgent* agent) {
                    DPRINT("setPhysicalStateAgent(agent: %p)", agent);

                    {
                        std::lock_guard<std::mutex> lock(gAgentMutex);
                        gPhysicalStateAgent =
                                agent ? *agent : QAndroidPhysicalStateAgent{};
                    }

                    if (agent) {
                        auto sensorClient = getGlobalSensorClient();
                        if (sensorClient) {
                            subscribeToPhysicalStateEvents(sensorClient.get());
                        } else {
                            derror("SensorClient not available, cannot subscribe to events.");
                        }
                    }
                    return 0;
                },
        .advanceTime = []() {
            // We are just a UI, therefore we don't care about advancing time.
        },
};
