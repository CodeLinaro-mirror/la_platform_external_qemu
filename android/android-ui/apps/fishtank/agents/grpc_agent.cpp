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

#include "android/emulation/control/grpc_agent.h"

const QGrpcAgent sFishtankQGrpcAgent = {
        .start =
                [](int port, const char* cert) {
                    NOT_IMPLEMENTED("QGrpcAgent.start(port: %d, cert: %s)", port, cert);
                    return 0;
                },
        .active_port =
                []() {
                    NOT_IMPLEMENTED("QGrpcAgent.active_port");
                    return -1;
                },
};
