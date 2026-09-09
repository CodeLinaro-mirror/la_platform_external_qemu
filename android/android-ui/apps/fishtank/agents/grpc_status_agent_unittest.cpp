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

#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "android/skin/qt/extended-pages/grpc-help-controller.h"
#include "emulator_controller_mock.grpc.pb.h"

using android::emulation::control::EmulatorControlClient;
using android::emulation::control::EmulatorTestClient;
using android::emulation::control::MockEmulatorControllerStub;
using testing::_;
using testing::Invoke;

class GrpcHelpControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto mockStub = std::make_unique<MockEmulatorControllerStub>();
        mMockStub = mockStub.get();

        auto testClient = std::make_shared<EmulatorTestClient>();
        mControlClient = std::make_shared<EmulatorControlClient>(
                testClient, mockStub.release());
    }

    MockEmulatorControllerStub* mMockStub = nullptr;
    std::shared_ptr<EmulatorControlClient> mControlClient;
};

TEST_F(GrpcHelpControllerTest, GrpcHelpController_FetchesEmulatorStatusFromBackend) {
    EXPECT_CALL(*mMockStub, getStatus(_, _, _))
            .WillOnce(Invoke([](grpc::ClientContext* context,
                                const google::protobuf::Empty& request,
                                android::emulation::control::EmulatorStatus* response) {
                response->set_version("37.1.2-16173978");
                (*response->mutable_guestconfig())["androidVersion"] = "15.0";
                (*response->mutable_guestconfig())["hypervisorVersion"] = "KVM";
                (*response->mutable_guestconfig())["avdDetails"] = "details";
                return grpc::Status::OK;
            }));

    GrpcHelpController controller(mControlClient);
    HelpSystemInfo info = controller.getSystemInfo();

    EXPECT_EQ(info.emulatorVersion, "37.1.2-16173978");
    EXPECT_EQ(info.androidVersion, "15.0");
    EXPECT_FALSE(info.feedbackReport.empty());
}

TEST_F(GrpcHelpControllerTest, GrpcHelpController_HandlesMissingGuestConfigGracefully) {
    EXPECT_CALL(*mMockStub, getStatus(_, _, _))
            .WillOnce(Invoke([](grpc::ClientContext* context,
                                const google::protobuf::Empty& request,
                                android::emulation::control::EmulatorStatus* response) {
                response->set_version("37.1.2.0 (37.1.2-15513348)");
                return grpc::Status::OK;
            }));

    GrpcHelpController controller(mControlClient);
    HelpSystemInfo info = controller.getSystemInfo();

    EXPECT_EQ(info.emulatorVersion, "37.1.2.0 (37.1.2-15513348)");
    EXPECT_TRUE(info.androidVersion.empty());
}
