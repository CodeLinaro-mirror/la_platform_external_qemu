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
#include <atomic>
#include <memory>
#include <thread>

#include "android/android-ui/apps/fishtank/fishtank_agents.h"
#include "android/emulation/control/sensors_agent.h"
#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "android/physics/physical_state_agent.h"
#include "emulator_controller_mock.grpc.pb.h"

using namespace android::emulation::control;
using testing::_;
using testing::DoAll;
using testing::Invoke;
using testing::Return;
using testing::SetArgPointee;

// Helper to capture callbacks from the PhysicalStateAgent.
struct MockPhysicalStateAgent {
    static void onTargetStateChanged(void* context) {
        static_cast<MockPhysicalStateAgent*>(context)->targetStateChangedCount++;
    }
    static void onPhysicalStateChanging(void* context) {
        static_cast<MockPhysicalStateAgent*>(context)->physicalStateChangingCount++;
    }
    static void onPhysicalStateStabilized(void* context) {
        static_cast<MockPhysicalStateAgent*>(context)->physicalStateStabilizedCount++;
    }

    std::atomic<int> targetStateChangedCount{0};
    std::atomic<int> physicalStateChangingCount{0};
    std::atomic<int> physicalStateStabilizedCount{0};

    QAndroidPhysicalStateAgent agent() {
        return {
                .context = this,
                .onTargetStateChanged = onTargetStateChanged,
                .onPhysicalStateChanging = onPhysicalStateChanging,
                .onPhysicalStateStabilized = onPhysicalStateStabilized,
        };
    }
};

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

// Verifies that getSensorSize returns the correct size (number of floats)
// for various sensor types.
TEST_F(SensorsAgentTest, GetSensorSize) {
    size_t size = 0;
    // ACCELERATION (0) should be 3
    EXPECT_EQ(0, mAgent->getSensorSize(0, &size));
    EXPECT_EQ(3, size);

    // LIGHT (6) should be 1
    EXPECT_EQ(0, mAgent->getSensorSize(6, &size));
    EXPECT_EQ(1, size);

    // RGBC_LIGHT (15) should be 4
    EXPECT_EQ(0, mAgent->getSensorSize(15, &size));
    EXPECT_EQ(4, size);

    // Unknown sensors should return -1.
    EXPECT_EQ(-1, mAgent->getSensorSize(100, &size));
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

// Verifies that setPhysicalParameterTarget triggers the appropriate callbacks
// in the registered PhysicalStateAgent.
TEST_F(SensorsAgentTest, SetPhysicalParameterTargetTriggersCallbacks) {
    MockPhysicalStateAgent mockAgent;
    auto agent = mockAgent.agent();

    // We expect some getPhysicalModel calls from the polling thread if we register the agent.
    mAgent->setPhysicalStateAgent(&agent);

    float values[] = {1.0f, 2.0f, 3.0f};
    EXPECT_CALL(*mMockStub, setPhysicalModel(_, _, _))
            .WillOnce(Return(::grpc::Status::OK));
    EXPECT_CALL(*mMockStub, getPhysicalModel(_, _, _))
            .WillRepeatedly(Return(::grpc::Status::OK));

    EXPECT_EQ(0, mAgent->setPhysicalParameterTarget(0, values, 3, 0));

    EXPECT_GT(mockAgent.physicalStateChangingCount, 0);
    EXPECT_GT(mockAgent.targetStateChangedCount, 0);
}

// Verifies that the background polling thread detects changes in the physical
// model and triggers the appropriate callbacks.
TEST_F(SensorsAgentTest, PollingDetectsChanges) {
    MockPhysicalStateAgent mockAgent;
    auto agent = mockAgent.agent();

    // Mock getPhysicalModel to return a moving value.
    // The polling loop checks both POSITION (0) and ROTATION (1).
    std::atomic<int> callCount{0};
    EXPECT_CALL(*mMockStub, getPhysicalModel(_, _, _))
            .WillRepeatedly(Invoke([&callCount](::grpc::ClientContext*,
                                                const PhysicalModelValue& req,
                                                PhysicalModelValue* resp) {
                resp->set_status(PhysicalModelValue::OK);
                auto data = resp->mutable_value();
                // Return 0.0 then 1.0 to simulate movement.
                float val = (callCount.load() < 10) ? 0.0f : 1.0f;
                data->add_data(val);
                data->add_data(val);
                data->add_data(val);
                callCount++;
                return ::grpc::Status::OK;
            }));

    mAgent->setPhysicalStateAgent(&agent);

    // Wait long enough for the physical state to stabilize.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    EXPECT_GT(mockAgent.physicalStateChangingCount, 0);
    EXPECT_GT(mockAgent.targetStateChangedCount, 0);
}
