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
#include <optional>
#include <thread>
#include <vector>

using android::emulation::control::PhysicalModelValue;
using android::emulation::control::ParameterValue;

#define DEBUG 1
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
 * One challenge is that Fishtank needs to notify its UI when the physical state
 * (e.g., device rotation) changes, even if that change was initiated by another
 * gRPC client or the backend's own simulation.
 *
 * Since the current gRPC backend does not support streaming notifications for
 * physical model changes, we implement a polling mechanism here. When a
 * QAndroidPhysicalStateAgent is registered by the UI, we start a background
 * thread that periodically checks for changes in key physical parameters
 * (Position and Rotation) and triggers the appropriate callbacks.
 */

static QAndroidPhysicalStateAgent gPhysicalStateAgent = {};
static std::thread gPollingThread;
static std::atomic<bool> gStopPolling{false};

// Helper to get a vec3 physical parameter synchronously
static bool getPhysicalParameterVec3(int parameter, float out[3]) {
    auto client = getGlobalControlClient();
    if (!client) return false;

    PhysicalModelValue request;
    request.set_target((PhysicalModelValue::PhysicalType)parameter);

    PhysicalModelValue response;
    grpc::ClientContext context;
    // Use a short timeout for polling
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(100));

    auto status = client->service()->getPhysicalModel(&context, request, &response);
    if (!status.ok()) return false;

    auto data = response.value().data();
    for (int i = 0; i < 3 && i < data.size(); i++) {
        out[i] = data.Get(i);
    }
    return true;
}

// Helper to check if a 3-component parameter has changed significantly.
static bool hasSignificantChange(const float current[3],
                                 const float last[3],
                                 float threshold) {
    for (int i = 0; i < 3; i++) {
        if (std::abs(current[i] - last[i]) > threshold) {
            return true;
        }
    }
    return false;
}

static void pollingLoop() {
    enum class MovementState { STATIONARY, MOVING };

    std::optional<std::array<float, 3>> lastPos;
    std::optional<std::array<float, 3>> lastRot;
    MovementState movementState = MovementState::STATIONARY;

    while (!gStopPolling) {
        float currentPos[3] = {0, 0, 0};
        float currentRot[3] = {0, 0, 0};

        // Poll POSITION and ROTATION from the backend.
        // These are the primary indicators of device movement in the physical model.
        bool ok = getPhysicalParameterVec3(PHYSICAL_PARAMETER_POSITION, currentPos);
        ok &= getPhysicalParameterVec3(PHYSICAL_PARAMETER_ROTATION, currentRot);

        if (ok) {
            // These thresholds (0.001 for position, 0.01 for rotation) are
            // chosen to filter out minor numerical jitter or noise in the
            // physical model simulation while remaining sensitive enough to
            // detect intentional movement.
            bool changed = false;

            // We only check for changes if we have a baseline from a previous poll.
            if (lastPos && lastRot) {
                const bool posChanged =
                        hasSignificantChange(currentPos, lastPos->data(), 0.001f);
                const bool rotChanged =
                        hasSignificantChange(currentRot, lastRot->data(), 0.01f);
                changed = posChanged || rotChanged;

                if (changed) {
                    // Transition: Stationary -> Moving
                    if (movementState == MovementState::STATIONARY) {
                        movementState = MovementState::MOVING;
                        if (gPhysicalStateAgent.onPhysicalStateChanging) {
                            gPhysicalStateAgent.onPhysicalStateChanging(
                                    gPhysicalStateAgent.context);
                        }
                    }

                    // Notify the UI that the state has updated (e.g. from an
                    // external gRPC client) so it can refresh its view.
                    if (gPhysicalStateAgent.onTargetStateChanged) {
                        gPhysicalStateAgent.onTargetStateChanged(
                                gPhysicalStateAgent.context);
                    }
                } else if (movementState == MovementState::MOVING) {
                    // Transition: Moving -> Stationary
                    movementState = MovementState::STATIONARY;
                    if (gPhysicalStateAgent.onPhysicalStateStabilized) {
                        gPhysicalStateAgent.onPhysicalStateStabilized(
                                gPhysicalStateAgent.context);
                    }
                }
            }

            lastPos = {currentPos[0], currentPos[1], currentPos[2]};
            lastRot = {currentRot[0], currentRot[1], currentRot[2]};
        }

        // Polling at 100ms is completely arbitrary and can be adjusted if needed.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

const QAndroidSensorsAgent sFishtankQAndroidSensorsAgent = {
        .setPhysicalParameterTarget =
                [](int parameter, const float* value, size_t len, int interpolation_method) {
                    DPRINT("setPhysicalParameterTarget(parameter: %d, len: %zu)", parameter, len);
                    auto client = getGlobalControlClient();
                    if (!client) return -1;

                    // Manually trigger "changing" callback for instant local feedback
                    if (gPhysicalStateAgent.onPhysicalStateChanging) {
                        gPhysicalStateAgent.onPhysicalStateChanging(gPhysicalStateAgent.context);
                    }

                    PhysicalModelValue request;
                    request.set_target((PhysicalModelValue::PhysicalType)parameter);
                    auto val = request.mutable_value();
                    for (size_t i = 0; i < len; i++) {
                        val->add_data(value[i]);
                    }

                    grpc::ClientContext context;
                    google::protobuf::Empty response;
                    auto status = client->service()->setPhysicalModel(&context, request, &response);

                    // Manually trigger "target changed" callback
                    if (gPhysicalStateAgent.onTargetStateChanged) {
                        gPhysicalStateAgent.onTargetStateChanged(gPhysicalStateAgent.context);
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
                    grpc::ClientContext context;
                    auto status = client->service()->getPhysicalModel(&context, request, &response);
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
                    NOT_IMPLEMENTED("QAndroidSensorsAgent.setCoarseOrientation(orientation: %d)", orientation);
                    return 0;
                },
        .setSensorOverride =
                [](int sensor, const float* value, size_t len) {
                    NOT_IMPLEMENTED("QAndroidSensorsAgent.setSensorOverride(sensor: %d, value: %p, len: %zu)", sensor, value, len);
                    return 0;
                },
        .getSensor =
                [](int sensor, float* const* value, size_t len) {
                    NOT_IMPLEMENTED("QAndroidSensorsAgent.getSensor(sensor: %d, value: %p, len: %zu)", sensor, value, len);
                    return 0;
                },
        .getSensorSize =
                [](int sensor, size_t* size) {
                    NOT_IMPLEMENTED("QAndroidSensorsAgent.getSensorSize(sensor: %d, size: %p)", sensor, size);
                    return 0;
                },
        .getDelayMs =
                []() {
                    NOT_IMPLEMENTED("QAndroidSensorsAgent.getDelayMs");
                    return 0;
                },
        .setPhysicalStateAgent =
                [](const struct QAndroidPhysicalStateAgent* agent) {
                    DPRINT("setPhysicalStateAgent(agent: %p)", agent);

                    // Stop previous polling if any
                    if (gPollingThread.joinable()) {
                        gStopPolling = true;
                        gPollingThread.join();
                    }

                    if (agent) {
                        gPhysicalStateAgent = *agent;
                        gStopPolling = false;
                        gPollingThread = std::thread(pollingLoop);
                    } else {
                        gPhysicalStateAgent = {};
                    }
                    return 0;
                },
        .advanceTime = []() { NOT_IMPLEMENTED("QAndroidSensorsAgent.advanceTime"); },
};
