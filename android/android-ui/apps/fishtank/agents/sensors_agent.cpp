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

#include "android/emulation/control/sensors_agent.h"

const QAndroidSensorsAgent sFishtankQAndroidSensorsAgent = {
        .setPhysicalParameterTarget =
                [](int parameter, const float* value, size_t len, int interpolation_method) {
                    NOT_IMPLEMENTED("QAndroidSensorsAgent.setPhysicalParameterTarget(parameter: %d, value: %p, len: %zu, interpolation_method: %d)", parameter, value, len, interpolation_method);
                    return 0;
                },
        .getPhysicalParameter =
                [](int parameter, float* const* value, size_t len, int interpolation_method) {
                    NOT_IMPLEMENTED("QAndroidSensorsAgent.getPhysicalParameter(parameter: %d, value: %p, len: %zu, interpolation_method: %d)", parameter, value, len, interpolation_method);
                    return 0;
                },
        .getPhysicalParameterSize =
                [](int parameter, size_t* size) {
                    NOT_IMPLEMENTED("QAndroidSensorsAgent.getPhysicalParameterSize(parameter: %d, size: %p)", parameter, size);
                    return 0;
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
