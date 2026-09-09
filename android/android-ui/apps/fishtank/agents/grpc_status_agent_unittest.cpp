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
    EXPECT_THAT(
            info.feedbackReport,
            testing::StartsWith(
                    "Please Read:\nhttps://developer.android.com/studio/report-bugs.html#emulator-bugs\n\n"));
    EXPECT_THAT(
            info.feedbackReport,
            testing::HasSubstr(
                    "Emulator Version (Emulator--> Extended Controls--> Emulator Version): 37.1.2-16173978\n"));
    EXPECT_THAT(info.feedbackReport,
                testing::HasSubstr("Hypervisor Version: KVM\n"));
    EXPECT_THAT(info.feedbackReport,
                testing::HasSubstr("AVD Details: details\n"));
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
    EXPECT_THAT(
            info.feedbackReport,
            testing::StartsWith(
                    "Please Read:\nhttps://developer.android.com/studio/report-bugs.html#emulator-bugs\n\n"));
    EXPECT_THAT(
            info.feedbackReport,
            testing::HasSubstr(
                    "Emulator Version (Emulator--> Extended Controls--> Emulator Version): 37.1.2.0 (37.1.2-15513348)\n"));
    EXPECT_THAT(info.feedbackReport,
                testing::HasSubstr("Hypervisor Version: None\n"));
}

TEST_F(GrpcHelpControllerTest,
       GrpcHelpController_FormatsFeedbackReportWithAllBackendFields) {
    EXPECT_CALL(*mMockStub, getStatus(_, _, _))
            .WillOnce(Invoke([](grpc::ClientContext* context,
                                const google::protobuf::Empty& request,
                                android::emulation::control::EmulatorStatus*
                                        response) {
                response->set_version("37.2.4-16031473");
                auto& guestConfig = *response->mutable_guestconfig();
                guestConfig["androidVersion"] = "17 (C) - API CANARY";
                guestConfig["hypervisorVersion"] = "KVM 12.0.0";
                guestConfig["hostOsName"] = "Debian GNU/Linux rodete";
                guestConfig["cpuModel"] = "Intel CPU";
                guestConfig["totalMem"] = "192021";
                guestConfig["gpu"] = "host";
                guestConfig["buildFingerprint"] =
                        "google/generic_system_google/generic:CANARY/...";
                guestConfig["avdDetails"] = "Name: canary\nCPU/ABI: x86_64\n";
                return grpc::Status::OK;
            }));

    GrpcHelpController controller(mControlClient);
    HelpSystemInfo info = controller.getSystemInfo();

    EXPECT_THAT(
            info.feedbackReport,
            testing::HasSubstr(
                    "Emulator Version (Emulator--> Extended Controls--> Emulator Version): 37.2.4-16031473\nHypervisor Version: KVM 12.0.0\n"));
    EXPECT_THAT(info.feedbackReport,
                testing::HasSubstr(
                        "Host Operating System: Debian GNU/Linux rodete\n"));
    EXPECT_THAT(info.feedbackReport,
                testing::HasSubstr("CPU Manufacturer: Intel CPU\n"));
    EXPECT_THAT(info.feedbackReport, testing::HasSubstr("RAM: 192021 MB\n"));
    EXPECT_THAT(info.feedbackReport, testing::HasSubstr("GPU: host\n"));
    EXPECT_THAT(
            info.feedbackReport,
            testing::HasSubstr(
                    "Build Fingerprint: google/generic_system_google/generic:CANARY/...\n"));
    EXPECT_THAT(
            info.feedbackReport,
            testing::HasSubstr("AVD Details: Name: canary\nCPU/ABI: x86_64\n"));
}
