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
#include "emulator_controller.pb.h"

using android::emulation::control::PhysicalModelValue;
using android::emulation::control::ParameterValue;

#define DEBUG 1
#if DEBUG >= 1
#define DPRINT(fmt, ...) \
    dinfo("FishtankAgents (Sensors): " fmt, ##__VA_ARGS__);
#else
#define DPRINT(...)
#endif

const QAndroidSensorsAgent sFishtankQAndroidSensorsAgent = {
        .setPhysicalParameterTarget =
                [](int parameter, const float* value, size_t len, int interpolation_method) {
                    DPRINT("setPhysicalParameterTarget(parameter: %d, len: %zu)", parameter, len);
                    auto client = getGlobalControlClient();
                    if (!client) return -1;

                    PhysicalModelValue request;
                    request.set_target((PhysicalModelValue::PhysicalType)parameter);
                    auto val = request.mutable_value();
                    for (size_t i = 0; i < len; i++) {
                        val->add_data(value[i]);
                    }

                    grpc::ClientContext context;
                    google::protobuf::Empty response;
                    auto status = client->service()->setPhysicalModel(&context, request, &response);
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
                    NOT_IMPLEMENTED("QAndroidSensorsAgent.setPhysicalStateAgent(agent: %p)", agent);
                    return 0;
                },
        .advanceTime = []() { NOT_IMPLEMENTED("QAndroidSensorsAgent.advanceTime"); },
};
