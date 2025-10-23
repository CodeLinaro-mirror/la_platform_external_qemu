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

#include "android/emulation/control/net_agent.h"

const QAndroidNetAgent sFishtankQAndroidNetAgent = {
        .isSlirpInited =
                []() {
                    NOT_IMPLEMENTED("QAndroidNetAgent.isSlirpInited");
                    return false;
                },
        .slirpRedir =
                [](bool is_udp, int host_port, int guest_port) {
                    NOT_IMPLEMENTED("QAndroidNetAgent.slirpRedir(is_udp: %d, host_port: %d, guest_port: %d)", is_udp, host_port, guest_port);
                    return false;
                },
        .slirpUnredir =
                [](bool is_udp, int host_port) {
                    NOT_IMPLEMENTED("QAndroidNetAgent.slirpUnredir(is_udp: %d, host_port: %d)", is_udp, host_port);
                    return false;
                },
        .slirpRedirIpv6 =
                [](bool is_udp, int host_port, int guest_port) {
                    NOT_IMPLEMENTED("QAndroidNetAgent.slirpRedirIpv6(is_udp: %d, host_port: %d, guest_port: %d)", is_udp, host_port, guest_port);
                    return false;
                },
        .slirpUnredirIpv6 =
                [](bool is_udp, int host_port) {
                    NOT_IMPLEMENTED("QAndroidNetAgent.slirpUnredirIpv6(is_udp: %d, host_port: %d)", is_udp, host_port);
                    return false;
                },
        .slirpBlockSsid =
                [](const char* ssid, bool block) {
                    NOT_IMPLEMENTED("QAndroidNetAgent.slirpBlockSsid(ssid: %s, block: %d)", ssid, block);
                    return false;
                },
};
