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

#include "android/skin/qt/extended-pages/grpc-battery-controller.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "emulator_controller_mock.grpc.pb.h"
#include "grpc_endpoint_description.pb.h"

using ::android::emulation::control::EmulatorControlClient;
using ::android::emulation::control::EmulatorController;
using ::android::emulation::control::EmulatorGrpcClient;
using android::emulation::control::MockEmulatorControllerStub;
using ::android::emulation::remote::Endpoint;
using ::google::protobuf::Empty;
using ::grpc::ClientContext;
using ::grpc::Status;
using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::WithArg;

#pragma once

#include "gmock/gmock.h"

#include <condition_variable>
#include <mutex>

class EmulatorControllerImpl final : public EmulatorController::Service {
public:
    Status setBattery(
            grpc::ServerContext* context,
            const ::android::emulation::control::BatteryState* requestPtr,
            ::google::protobuf::Empty* reply) override {
        LOG(INFO) << "Look: " << requestPtr->DebugString();
        std::unique_lock<std::mutex> lock(mMutex);
        state = *requestPtr;
        mCv.notify_one();
        return Status::OK;
    }

    ::android::emulation::control::BatteryState state;
    std::mutex mMutex;
    std::condition_variable mCv;
};

class GrpcBatteryControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string uri = "localhost:" + std::to_string(mPort);
        grpc::ServerBuilder builder;
        mService = std::make_unique<EmulatorControllerImpl>();
        builder.RegisterService(mService.get());
        builder.AddListeningPort(uri, grpc::InsecureServerCredentials(),
                                 &mPort);

        mServer = builder.BuildAndStart();

        Endpoint dest;
        dest.set_target(uri);
        EmulatorGrpcClient::Builder clientBuilder;
        clientBuilder.withEndpoint(dest);
        auto maybeClient = clientBuilder.build();
        ASSERT_TRUE(maybeClient.ok());
        auto uniqueClient = std::move(maybeClient.value());
        auto sharedClient = std::shared_ptr<EmulatorGrpcClient>(uniqueClient.release());

        auto sharedControlClient =
                std::make_shared<EmulatorControlClient>(sharedClient);
        mController =
                std::make_unique<GrpcBatteryController>(sharedControlClient);
    }

    void TearDown() override {
        // Note: We only give the server 50ms to shutdown to make sure we do not
        // have to block and wait for server shutdown. We should be safely to do
        // so as we do not expect there to be any ongoing requests.
        auto deadline = std::chrono::system_clock::now() +
                        std::chrono::milliseconds(50);
        mServer->Shutdown(deadline);
        mServer->Wait();
    }

    std::unique_ptr<GrpcBatteryController> mController;
    std::unique_ptr<MockEmulatorControllerStub> mMockStub;
    std::unique_ptr<EmulatorControllerImpl> mService;
    std::unique_ptr<grpc::Server> mServer;
    std::shared_ptr<grpc::Channel> mChannel;
    std::unique_ptr<EmulatorController::Stub> mStub;
    int mPort{52102};
};

TEST_F(GrpcBatteryControllerTest, SetBatteryCallsGrpcClient) {
    BatteryState state = {77,
                          BATTERY_CHARGER_USB,
                          BATTERY_HEALTH_OVERVOLTAGE,
                          BATTERY_STATUS_DISCHARGING,
                          true,
                          true};

    mController->setBattery(state);

    // The setBattery call is async, so we need to wait until the state
    // has been updated.
    std::unique_lock<std::mutex> lock(mService->mMutex);
    mService->mCv.wait(lock, [this] { return mService->state.hasbattery(); });
    ::android::emulation::control::BatteryState capturedProto = mService->state;

    EXPECT_EQ(capturedProto.chargelevel(), 77);
    EXPECT_EQ(capturedProto.charger(),
              ::android::emulation::control::BatteryState::USB);
    EXPECT_EQ(capturedProto.health(),
              ::android::emulation::control::BatteryState::OVERVOLTAGE);
    EXPECT_EQ(capturedProto.status(),
              ::android::emulation::control::BatteryState::DISCHARGING);
    EXPECT_TRUE(capturedProto.hasbattery());
    EXPECT_TRUE(capturedProto.ispresent());
}