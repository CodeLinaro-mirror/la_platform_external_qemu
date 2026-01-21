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

#include <memory>
#include <string>

#include "absl/status/status.h"
#include "android/emulation/control/GrpcServices.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "services/forwarder/service_forwarder.grpc.pb.h"

namespace android {
namespace fishtank {

class FishtankGrpcServer {
public:
    FishtankGrpcServer();
    ~FishtankGrpcServer();

    // Initializes the gRPC server. Returns status.
    absl::Status setup(int preferred_port = 0, bool enable_tls = true);

    // Returns the port the server is listening on, or -1 if not running.
    int port() const;

    // Registers the UiController service with the main emulator.
    // Returns OkStatus on success, or an error status on failure.
    absl::Status registerUiController(
            android::emulation::control::EmulatorGrpcClient* client);

    // Overload for testing that accepts a StubInterface directly.
    absl::Status registerUiController(
            android::emulation::forwarding::ServiceForwarder::StubInterface*
                    stub);

private:
    std::unique_ptr<android::emulation::control::EmulatorControllerService>
            mGrpcService;
    std::string mServerCert;
    std::string mClientKey;
    std::string mClientCert;
    int mPort = -1;
};

}  // namespace fishtank
}  // namespace android
