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

#include <cstddef>
#include "android/console.h"
#include "android/emulation/control/location_agent.h"
#include "host-common/hw-config.h"

static android::emulation::control::GpsState gGpsState;

const QAndroidLocationAgent sFishtankQAndroidLocationAgent = {
        .gpsIsSupported = []() -> bool {
            return (bool)getConsoleAgents()->settings->hw()->hw_gps;
        },
        .gpsSendLoc =
                [](double lat,
                   double lon,
                   double alt,
                   double speed,
                   double bearing,
                   int n_sv,
                   const struct timeval* tv) {
                    gGpsState.set_latitude(lat);
                    gGpsState.set_longitude(lon);
                    gGpsState.set_altitude(alt);
                    gGpsState.set_speed(speed);
                    gGpsState.set_bearing(bearing);
                    gGpsState.set_satellites(n_sv);
                    getGlobalControlClient()->setGpsAsync(gGpsState);
                },
        .gpsGetLoc =
                [](double* lat,
                   double* lon,
                   double* alt,
                   double* speed,
                   double* bearing,
                   int* n_sv) {
                    auto gps = getGlobalControlClient()->getGps();
                    if (!gps.ok()) {
                        return -1;
                    }
                    if (lat) {
                        *lat = gps->latitude();
                    }
                    if (lon) {
                        *lon = gps->longitude();
                    }
                    if (alt) {
                        *alt = gps->altitude();
                    }
                    if (speed) {
                        *speed = gps->speed();
                    }
                    if (bearing) {
                        *bearing = gps->bearing();
                    }
                    if (n_sv) {
                        *n_sv = gps->satellites();
                    }
                    return 0;
                },
        .gpsSendNmea =
                [](const char* nmea) {
                    NOT_IMPLEMENTED(
                            "QAndroidLocationAgent.gpsSendNmea(nmea: "
                            "%s)",
                            nmea);
                },
        .gpsSendGnss =
                [](const char* gnss) {
                    NOT_IMPLEMENTED(
                            "QAndroidLocationAgent.gpsSendGnss(gnss: "
                            "%s)",
                            gnss);
                },
        .gpsSetPassiveUpdate =
                [](bool passive) {
                    NOT_IMPLEMENTED(
                            "QAndroidLocationAgent.gpsSetPassiveUpdate("
                            "passive:"
                            " %d)",
                            passive);
                },
        .gpsGetPassiveUpdate =
                []() {
                    NOT_IMPLEMENTED(
                            "QAndroidLocationAgent."
                            "gpsGetPassiveUpdate");
                    return false;
                },
        .gpsEnableGnssGrpcV1 =
                []() {
                    NOT_IMPLEMENTED(
                            "QAndroidLocationAgent."
                            "gpsEnableGnssGrpcV1");
                },
        .gpsGetGpsSignal =
                []() {
                    NOT_IMPLEMENTED("QAndroidLocationAgent.gpsGetGpsSignal");
                    return false;
                },
        .gpsSetGpsSignal =
                [](bool has_signal) {
                    NOT_IMPLEMENTED(
                            "QAndroidLocationAgent.gpsSetGpsSignal(has_"
                            "signal: "
                            "%d)",
                            has_signal);
                },
};
