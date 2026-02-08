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

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>

#include "android/android-ui/apps/fishtank/fishtank_agents.h"
#include "android/emulation/control/sensors_agent.h"
#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "emulator_controller_mock.grpc.pb.h"

using namespace android::emulation::control;
using testing::_;
using testing::DoAll;
using testing::Return;
using testing::SetArgPointee;

// Global control client for the agent to use.
static std::shared_ptr<EmulatorControlClient> gTestControlClient;

std::shared_ptr<EmulatorControlClient> getGlobalControlClient() {
    return gTestControlClient;
}

class SensorsAgentTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto mockStub = std::make_unique<MockEmulatorControllerStub>();
        mMockStub = mockStub.get();
        // EmulatorControlClient takes ownership of the stub.
        auto testClient = std::make_shared<EmulatorTestClient>();
        gTestControlClient = std::make_shared<EmulatorControlClient>(
                testClient, mockStub.release());
        mAgent = &sFishtankQAndroidSensorsAgent;
    }

    void TearDown() override {
        if (mAgent) {
            mAgent->setPhysicalStateAgent(nullptr);
        }
        gTestControlClient.reset();
    }

    MockEmulatorControllerStub* mMockStub;
    const QAndroidSensorsAgent* mAgent;
};

// Verifies that getPhysicalParameterSize returns the correct size (number of floats)
// for various physical parameter types.
TEST_F(SensorsAgentTest, GetPhysicalParameterSize) {
    size_t size = 0;
    // POSITION (0) should be 3
    EXPECT_EQ(0, mAgent->getPhysicalParameterSize(0, &size));
    EXPECT_EQ(3, size);

    // RGBC_LIGHT (18) should be 4
    EXPECT_EQ(0, mAgent->getPhysicalParameterSize(18, &size));
    EXPECT_EQ(4, size);

    // Unknown parameters should return -1.
    EXPECT_EQ(-1, mAgent->getPhysicalParameterSize(100, &size));
}

// Verifies that setPhysicalParameterTarget correctly forwards the request to the
// gRPC backend and returns success (0) when the RPC is successful.
TEST_F(SensorsAgentTest, SetPhysicalParameterTargetSuccess) {
    float values[] = {1.0f, 2.0f, 3.0f};

    EXPECT_CALL(*mMockStub, setPhysicalModel(_, _, _))
            .WillOnce(Return(::grpc::Status::OK));

    EXPECT_EQ(0, mAgent->setPhysicalParameterTarget(0, values, 3, 0));
}

// Verifies that setPhysicalParameterTarget returns failure (-1) when the
// underlying gRPC call fails.
TEST_F(SensorsAgentTest, SetPhysicalParameterTargetFailure) {
    float values[] = {1.0f, 2.0f, 3.0f};

    EXPECT_CALL(*mMockStub, setPhysicalModel(_, _, _))
            .WillOnce(Return(::grpc::Status(::grpc::StatusCode::INTERNAL, "Error")));

    EXPECT_EQ(-1, mAgent->setPhysicalParameterTarget(0, values, 3, 0));
}

// Verifies that getPhysicalParameter correctly retrieves data from the gRPC
// backend and populates the provided value pointers.
TEST_F(SensorsAgentTest, GetPhysicalParameterSuccess) {
    float val1 = 0.0f, val2 = 0.0f, val3 = 0.0f;
    float* values[] = {&val1, &val2, &val3};

    EXPECT_CALL(*mMockStub, getPhysicalModel(_, _, _))
            .WillOnce([](::grpc::ClientContext* context,
                         const PhysicalModelValue& request,
                         PhysicalModelValue* response) {
                response->mutable_value()->add_data(10.0f);
                response->mutable_value()->add_data(20.0f);
                response->mutable_value()->add_data(30.0f);
                response->set_status(PhysicalModelValue::OK);
                return ::grpc::Status::OK;
            });

    EXPECT_EQ(0, mAgent->getPhysicalParameter(0, values, 3, 0));
    EXPECT_EQ(10.0f, val1);
    EXPECT_EQ(20.0f, val2);
    EXPECT_EQ(30.0f, val3);
}

// Verifies that getPhysicalParameter returns failure (-1) when the underlying
// gRPC call fails.
TEST_F(SensorsAgentTest, GetPhysicalParameterFailure) {
    float val1 = 0.0f;
    float* values[] = {&val1};

    EXPECT_CALL(*mMockStub, getPhysicalModel(_, _, _))
            .WillOnce(Return(::grpc::Status(::grpc::StatusCode::INTERNAL, "Error")));

    EXPECT_EQ(-1, mAgent->getPhysicalParameter(0, values, 1, 0));
}
