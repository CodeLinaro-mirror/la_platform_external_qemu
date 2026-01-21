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

#include "android/android-ui/apps/fishtank/grpc/FishtankGrpcServer.h"

#include <string>

#include "absl/strings/str_cat.h"
#include "aemu/base/Log.h"
#include "aemu/base/misc/FileUtils.h"
#include "android/base/system/System.h"
#include "android/emulation/control/CertificateFactory.h"
#include "android/emulation/control/EmulatorService.h"
#include "android/emulation/control/GrpcServices.h"
#include "android/emulation/control/UiController.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "services/forwarder/service_forwarder.grpc.pb.h"
#include "services/forwarder/service_forwarder.pb.h"

// Needed for getConsoleAgents used in setup
#include "android/android-ui/apps/fishtank/fishtank_agents.h"

using android::base::System;
using android::emulation::control::CertificateFactory;
using android::emulation::control::EmulatorControllerService;
using android::emulation::forwarding::ForwardingRule;
using android::emulation::forwarding::ServiceForwarder;

namespace {
constexpr int kDefaultGrpcPort = 8554;
constexpr int kGrpcPortRange = 1024;
constexpr const char* kDefaultAddress = "[::1]";
}  // namespace

namespace android {
namespace fishtank {

FishtankGrpcServer::FishtankGrpcServer() = default;
FishtankGrpcServer::~FishtankGrpcServer() = default;

absl::Status FishtankGrpcServer::setup(int preferred_port, bool enable_tls) {
    if (mGrpcService) {
        return absl::OkStatus();
    }

    int grpc_start = kDefaultGrpcPort;
    int grpc_end = grpc_start + kGrpcPortRange;
    std::string address = kDefaultAddress;

    if (preferred_port > 0) {
        grpc_start = preferred_port;
        grpc_end = grpc_start + kGrpcPortRange;
        address = "[::]";
    }

    // Fishtank will handle the ui controller interaction.
    auto uiController = android::emulation::control::getUiControllerService(
            getConsoleAgents());

    auto builder = EmulatorControllerService::Builder()
                           .withConsoleAgents(getConsoleAgents())
                           .withLogging(true)
                           .withPortRange(grpc_start, grpc_end)
                           .withAddress(address)
                           .withService(uiController);

    if (enable_tls) {
        // Generate certificates for mTLS
        std::string certDir = System::get()->getTempDir();
        auto [serverKeyPath, serverCertPath] =
                CertificateFactory::generateCertKeyPair(certDir,
                                                        "fishtank_server");
        auto [clientKeyPath, clientCertPath] =
                CertificateFactory::generateCertKeyPair(certDir,
                                                        "fishtank_client");

        if (serverKeyPath.empty() || serverCertPath.empty() ||
            clientKeyPath.empty() || clientCertPath.empty()) {
            return absl::InternalError(
                    "Failed to generate certificates for Fishtank gRPC.");
        }

        // Read cert contents for registration later
        auto serverCert = android::readFileIntoString(serverCertPath);
        auto clientKey = android::readFileIntoString(clientKeyPath);
        auto clientCert = android::readFileIntoString(clientCertPath);

        if (!serverCert || !clientKey || !clientCert) {
            return absl::InternalError(
                    "Failed to read generated certificates for Fishtank gRPC.");
        }

        mServerCert = *serverCert;
        mClientKey = *clientKey;
        mClientCert = *clientCert;

        // Configure server to use these certs and trust the client cert
        builder.withCertAndKey(serverCertPath.c_str(), serverKeyPath.c_str(),
                               clientCertPath.c_str());
        LOG(INFO) << "Fishtank gRPC mTLS enabled.";
    }

    mGrpcService = builder.build();

    if (mGrpcService) {
        mPort = mGrpcService->port();
        LOG(INFO) << "Fishtank gRPC server listening on port " << mPort;
        return absl::OkStatus();
    } else {
        LOG(ERROR) << "Failed to start Fishtank gRPC server.";
        return absl::InternalError("Failed to start Fishtank gRPC server.");
    }
}

int FishtankGrpcServer::port() const {
    return mPort;
}

absl::Status FishtankGrpcServer::registerUiController(
        android::emulation::control::EmulatorGrpcClient* client) {
    if (!client ||
        !client->stub<android::emulation::forwarding::ServiceForwarder>()) {
        return absl::InternalError(
                "No gRPC client available to register UiController.");
    }

    auto stub =
            client->stub<android::emulation::forwarding::ServiceForwarder>();
    return registerUiController(stub.get());
}

absl::Status FishtankGrpcServer::registerUiController(
        android::emulation::forwarding::ServiceForwarder::StubInterface* stub) {
    if (!stub) {
        return absl::InvalidArgumentError("StubInterface cannot be null.");
    }

    grpc::ClientContext context;
    ForwardingRule rule;
    rule.set_service_uri("android.emulation.control.UiController");
    auto* endpoint = rule.mutable_endpoint();
    endpoint->set_target(absl::StrCat("localhost:", mPort));

    // Configure credentials for the emulator to connect back to us
    if (!mServerCert.empty() && !mClientKey.empty() && !mClientCert.empty()) {
        auto* credentials = endpoint->mutable_tls_credentials();
        credentials->set_pem_root_certs(mServerCert);
        credentials->set_pem_private_key(mClientKey);
        credentials->set_pem_cert_chain(mClientCert);
    }

    google::protobuf::Empty response;
    grpc::Status status = stub->registerForwarder(&context, rule, &response);

    if (status.ok()) {
        LOG(INFO) << "Successfully registered UiController with main emulator "
                     "on port "
                  << mPort;
        return absl::OkStatus();
    } else {
        return absl::InternalError(absl::StrCat(
                "Failed to register UiController: ", status.error_message(),
                " (", status.error_code(), ")"));
    }
}

}  // namespace fishtank
}  // namespace android