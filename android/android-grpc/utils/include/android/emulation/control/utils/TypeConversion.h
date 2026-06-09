// Copyright (C) 2026 The Android Open Source Project
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

#include "android/utils/debug.h"

#include "android/xr-defines.h"
#include "emulator_controller.pb.h"
#include "xr_emulator_conn.pb.h"

namespace android {
namespace emulation {
namespace control {

inline android::emulation::control::XrOptions_Environment toHostEnvironment(
        xr_emulator_proto::XrOptions_Environment value) {
    constexpr android::emulation::control::XrOptions_Environment
            lookup_table[] = {
                    [xr_emulator_proto::XrOptions_Environment_LIVING_ROOM_DAY] =
                            android::emulation::control::XrOptions::
                                    LIVING_ROOM_DAY,
                    [xr_emulator_proto::
                             XrOptions_Environment_LIVING_ROOM_NIGHT] =
                            android::emulation::control::XrOptions::
                                    LIVING_ROOM_NIGHT,
            };
    android::emulation::control::XrOptions_Environment result =
            XrOptions::LIVING_ROOM_DAY;
    int index = static_cast<int>(value);
    if (0 <= value && value < std::size(lookup_table)) {
        result = lookup_table[index];
    } else {
        derror("Attempt to convert from unknown xr_emulator_proto::XrOptions_Environment value: %d",
               index);
        assert(false);
    }
    return result;
}

inline xr_emulator_proto::XrOptions_Environment toGuestEnvironment(
        android::emulation::control::XrOptions_Environment value) {
    constexpr xr_emulator_proto::XrOptions_Environment lookup_table[] = {
            [android::emulation::control::XrOptions::LIVING_ROOM_DAY] =
                    xr_emulator_proto::XrOptions_Environment_LIVING_ROOM_DAY,
            [android::emulation::control::XrOptions::LIVING_ROOM_NIGHT] =
                    xr_emulator_proto::XrOptions_Environment_LIVING_ROOM_NIGHT,
    };
    xr_emulator_proto::XrOptions_Environment result =
            xr_emulator_proto::XrOptions_Environment_LIVING_ROOM_DAY;
    int index = static_cast<int>(value);
    if (0 <= value && value < std::size(lookup_table)) {
        result = lookup_table[index];
    } else {
        derror("Attempt to convert from unknown android::emulation::control::XrOptions_Environment value: %d",
               static_cast<int>(value));
        assert(false);
    }

    return result;
}

inline xr_emulator_proto::XrOptions_Environment
XrEnvironmentModeToGuestEnvironment(XrEnvironmentMode value) {
    constexpr xr_emulator_proto::XrOptions_Environment lookup_table[] = {
            [XR_ENVIRONMENT_MODE_UNKNOWN] =
                    xr_emulator_proto::XrOptions_Environment_LIVING_ROOM_DAY,
            [XR_ENVIRONMENT_MODE_PASSTHROUGH_ON] =
                    xr_emulator_proto::XrOptions_Environment_LIVING_ROOM_DAY,
            [XR_ENVIRONMENT_MODE_PASSTHROUGH_OFF] =
                    xr_emulator_proto::XrOptions_Environment_LIVING_ROOM_DAY,
            [XR_ENVIRONMENT_MODE_LIVING_ROOM_DAY] =
                    xr_emulator_proto::XrOptions_Environment_LIVING_ROOM_DAY,
            [XR_ENVIRONMENT_MODE_LIVING_ROOM_NIGHT] =
                    xr_emulator_proto::XrOptions_Environment_LIVING_ROOM_NIGHT,
    };
    xr_emulator_proto::XrOptions_Environment result =
            xr_emulator_proto::XrOptions_Environment_LIVING_ROOM_DAY;
    int index = static_cast<int>(value);
    if (0 <= value && value < std::size(lookup_table)) {
        result = lookup_table[index];
    } else {
        derror("Attempt to convert from unknown XrEnvironmentMode value: %d",
               static_cast<int>(value));
        assert(false);
    }

    return result;
}

inline android::emulation::control::XrOptions toHostXrOptions(
        const xr_emulator_proto::XrOptions& guest_options) {
    android::emulation::control::XrOptions host_options;
    host_options.set_passthrough_coefficient(
            guest_options.passthrough_coefficient());
    host_options.set_environment(
            toHostEnvironment(guest_options.environment()));
    host_options.set_dimming_value(guest_options.dimming_value());
    return host_options;
}

inline xr_emulator_proto::XrOptions toGuestXrOptions(
        const android::emulation::control::XrOptions& host_options) {
    xr_emulator_proto::XrOptions guest_options;
    guest_options.set_passthrough_coefficient(
            host_options.passthrough_coefficient());
    guest_options.set_environment(
            toGuestEnvironment(host_options.environment()));
    guest_options.set_dimming_value(host_options.dimming_value());
    return guest_options;
}

}  // namespace control
}  // namespace emulation
}  // namespace android
