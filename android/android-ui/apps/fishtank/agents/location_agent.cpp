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

#include "android/emulation/control/location_agent.h"

const QAndroidLocationAgent sFishtankQAndroidLocationAgent = {
        .gpsIsSupported =
                []() {
                    NOT_IMPLEMENTED("QAndroidLocationAgent.gpsIsSupported()");
                    return false;
                },
        .gpsSendLoc =
                [](double lat,
                   double lon,
                   double alt,
                   double speed,
                   double bearing,
                   int n_sv,
                   const struct timeval* tv) {
                    NOT_IMPLEMENTED("QAndroidLocationAgent.gpsSendLoc(lat: %f, lon: %f, alt: %f, speed: %f, bearing: %f, n_sv: %d, tv: %p)", lat, lon, alt, speed, bearing, n_sv, tv);
                },
        .gpsGetLoc =
                [](double* lat, double* lon, double* alt, double* speed, double* bearing, int* n_sv) {
                    NOT_IMPLEMENTED("QAndroidLocationAgent.gpsGetLoc(lat: %p, lon: %p, alt: %p, speed: %p, bearing: %p, n_sv: %p)", lat, lon, alt, speed, bearing, n_sv);
                    return 0;
                },
        .gpsSendNmea = [](const char* nmea) { NOT_IMPLEMENTED("QAndroidLocationAgent.gpsSendNmea(nmea: %s)", nmea); },
        .gpsSendGnss = [](const char* gnss) { NOT_IMPLEMENTED("QAndroidLocationAgent.gpsSendGnss(gnss: %s)", gnss); },
        .gpsSetPassiveUpdate = [](bool passive) { NOT_IMPLEMENTED("QAndroidLocationAgent.gpsSetPassiveUpdate(passive: %d)", passive); },
        .gpsGetPassiveUpdate =
                []() {
                    NOT_IMPLEMENTED("QAndroidLocationAgent.gpsGetPassiveUpdate");
                    return false;
                },
        .gpsEnableGnssGrpcV1 = []() { NOT_IMPLEMENTED("QAndroidLocationAgent.gpsEnableGnssGrpcV1"); },
        .gpsGetGpsSignal =
                []() {
                    NOT_IMPLEMENTED("QAndroidLocationAgent.gpsGetGpsSignal");
                    return false;
                },
        .gpsSetGpsSignal = [](bool has_signal) { NOT_IMPLEMENTED("QAndroidLocationAgent.gpsSetGpsSignal(has_signal: %d)", has_signal); },
};
