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
#include "host-common/multi_display_agent.h"
#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "emulator_controller_mock.grpc.pb.h"

using android::emulation::control::EmulatorControlClient;
using android::emulation::control::EmulatorGrpcClient;
using android::emulation::control::EmulatorTestClient;
using android::emulation::control::MockEmulatorControllerStub;
using android::emulation::control::DisplayConfigurations;
using android::emulation::control::DisplayConfiguration;
using testing::_;
using testing::Return;
using testing::Invoke;

// Defined in test_client_setup.cpp
extern std::shared_ptr<android::emulation::control::EmulatorControlClient> gTestControlClient;

class MultiDisplayAgentTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto mockStub = std::make_unique<android::emulation::control::MockEmulatorControllerStub>();
        mMockStub = mockStub.get();

        // EmulatorControlClient takes ownership of the stub.
        auto testClient = std::make_shared<android::emulation::control::EmulatorTestClient>();
        gTestControlClient = std::make_shared<android::emulation::control::EmulatorControlClient>(
                testClient, mockStub.release());

        mAgent = &sFishtankQAndroidMultiDisplayAgent;
    }

    void TearDown() override {
        gTestControlClient.reset();
    }

    android::emulation::control::MockEmulatorControllerStub* mMockStub;
    const QAndroidMultiDisplayAgent* mAgent;
};

// Helper to populate a DisplayConfiguration
void populateDisplay(DisplayConfiguration* disp, uint32_t id, uint32_t w, uint32_t h, uint32_t dpi, uint32_t flags) {
    disp->set_display(id);
    disp->set_width(w);
    disp->set_height(h);
    disp->set_dpi(dpi);
    disp->set_flags(flags);
}

TEST_F(MultiDisplayAgentTest, IsMultiDisplayEnabled_False) {
    EXPECT_CALL(*mMockStub, getDisplayConfigurations(_, _, _))
            .WillOnce([](::grpc::ClientContext* context,
                         const ::google::protobuf::Empty& request,
                         DisplayConfigurations* response) {
                populateDisplay(response->add_displays(), 0, 1080, 1920, 440, 0);
                return ::grpc::Status::OK;
            });

    EXPECT_FALSE(mAgent->isMultiDisplayEnabled());
}

TEST_F(MultiDisplayAgentTest, IsMultiDisplayEnabled_True) {
    EXPECT_CALL(*mMockStub, getDisplayConfigurations(_, _, _))
            .WillOnce([](::grpc::ClientContext* context,
                         const ::google::protobuf::Empty& request,
                         DisplayConfigurations* response) {
                populateDisplay(response->add_displays(), 0, 1080, 1920, 440, 0);
                populateDisplay(response->add_displays(), 1, 720, 1280, 320, 0);
                return ::grpc::Status::OK;
            });

    EXPECT_TRUE(mAgent->isMultiDisplayEnabled());
}

TEST_F(MultiDisplayAgentTest, IsMultiDisplayEnabled_Error) {
    EXPECT_CALL(*mMockStub, getDisplayConfigurations(_, _, _))
            .WillOnce(Return(::grpc::Status(::grpc::StatusCode::INTERNAL, "Error")));

    EXPECT_FALSE(mAgent->isMultiDisplayEnabled());
}

TEST_F(MultiDisplayAgentTest, GetMultiDisplay_Success) {
    EXPECT_CALL(*mMockStub, getDisplayConfigurations(_, _, _))
            .WillOnce([](::grpc::ClientContext* context,
                         const ::google::protobuf::Empty& request,
                         DisplayConfigurations* response) {
                populateDisplay(response->add_displays(), 0, 1080, 1920, 440, 1);
                populateDisplay(response->add_displays(), 1, 720, 1280, 320, 2);
                return ::grpc::Status::OK;
            });

    int32_t x = -1, y = -1;
    uint32_t w = 0, h = 0, dpi = 0, flag = 0;
    bool enabled = false;

    EXPECT_TRUE(mAgent->getMultiDisplay(1, &x, &y, &w, &h, &dpi, &flag, &enabled));
    EXPECT_EQ(0, x);
    EXPECT_EQ(0, y);
    EXPECT_EQ(720, w);
    EXPECT_EQ(1280, h);
    EXPECT_EQ(320, dpi);
    EXPECT_EQ(2, flag);
    EXPECT_TRUE(enabled);
}

TEST_F(MultiDisplayAgentTest, GetMultiDisplay_NotFound) {
    EXPECT_CALL(*mMockStub, getDisplayConfigurations(_, _, _))
            .WillOnce([](::grpc::ClientContext* context,
                         const ::google::protobuf::Empty& request,
                         DisplayConfigurations* response) {
                populateDisplay(response->add_displays(), 0, 1080, 1920, 440, 1);
                return ::grpc::Status::OK;
            });

    int32_t x = -1, y = -1;
    uint32_t w = 0, h = 0, dpi = 0, flag = 0;
    bool enabled = true;

    EXPECT_FALSE(mAgent->getMultiDisplay(1, &x, &y, &w, &h, &dpi, &flag, &enabled));
    EXPECT_FALSE(enabled);
}

TEST_F(MultiDisplayAgentTest, GetMultiDisplay_Error) {
    EXPECT_CALL(*mMockStub, getDisplayConfigurations(_, _, _))
            .WillOnce(Return(::grpc::Status(::grpc::StatusCode::INTERNAL, "Error")));

    bool enabled = true;
    EXPECT_FALSE(mAgent->getMultiDisplay(1, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &enabled));
    EXPECT_FALSE(enabled);
}

TEST_F(MultiDisplayAgentTest, SetMultiDisplay_Add_New) {
    EXPECT_CALL(*mMockStub, getDisplayConfigurations(_, _, _))
            .WillOnce([](::grpc::ClientContext* context,
                         const ::google::protobuf::Empty& request,
                         DisplayConfigurations* response) {
                populateDisplay(response->add_displays(), 0, 1080, 1920, 440, 1);
                response->set_userconfigurable(3);
                response->set_maxdisplays(5);
                return ::grpc::Status::OK;
            });

    EXPECT_CALL(*mMockStub, setDisplayConfigurations(_, _, _))
            .WillOnce([](::grpc::ClientContext* context,
                         const DisplayConfigurations& request,
                         DisplayConfigurations* response) {
                EXPECT_EQ(1, request.displays_size());
                EXPECT_EQ(1, request.displays(0).display());
                EXPECT_EQ(720, request.displays(0).width());
                EXPECT_EQ(1280, request.displays(0).height());
                EXPECT_EQ(320, request.displays(0).dpi());
                EXPECT_EQ(2, request.displays(0).flags());
                return ::grpc::Status::OK;
            });

    EXPECT_EQ(0, mAgent->setMultiDisplay(1, -1, -1, 720, 1280, 320, 2, true));
}

TEST_F(MultiDisplayAgentTest, SetMultiDisplay_Add_Update) {
    EXPECT_CALL(*mMockStub, getDisplayConfigurations(_, _, _))
            .WillOnce([](::grpc::ClientContext* context,
                         const ::google::protobuf::Empty& request,
                         DisplayConfigurations* response) {
                populateDisplay(response->add_displays(), 0, 1080, 1920, 440, 1);
                populateDisplay(response->add_displays(), 1, 720, 1280, 320, 2);
                return ::grpc::Status::OK;
            });

    EXPECT_CALL(*mMockStub, setDisplayConfigurations(_, _, _))
            .WillOnce([](::grpc::ClientContext* context,
                         const DisplayConfigurations& request,
                         DisplayConfigurations* response) {
                EXPECT_EQ(1, request.displays_size());
                EXPECT_EQ(1, request.displays(0).display());
                EXPECT_EQ(800, request.displays(0).width()); // Updated
                EXPECT_EQ(1280, request.displays(0).height());
                EXPECT_EQ(320, request.displays(0).dpi());
                EXPECT_EQ(2, request.displays(0).flags());
                return ::grpc::Status::OK;
            });

    EXPECT_EQ(0, mAgent->setMultiDisplay(1, -1, -1, 800, 1280, 320, 2, true));
}

TEST_F(MultiDisplayAgentTest, SetMultiDisplay_Remove) {
    EXPECT_CALL(*mMockStub, getDisplayConfigurations(_, _, _))
            .WillOnce([](::grpc::ClientContext* context,
                         const ::google::protobuf::Empty& request,
                         DisplayConfigurations* response) {
                populateDisplay(response->add_displays(), 0, 1080, 1920, 440, 1);
                populateDisplay(response->add_displays(), 1, 720, 1280, 320, 2);
                return ::grpc::Status::OK;
            });

    EXPECT_CALL(*mMockStub, setDisplayConfigurations(_, _, _))
            .WillOnce([](::grpc::ClientContext* context,
                         const DisplayConfigurations& request,
                         DisplayConfigurations* response) {
                EXPECT_EQ(0, request.displays_size());
                return ::grpc::Status::OK;
            });

    EXPECT_EQ(0, mAgent->setMultiDisplay(1, -1, -1, 0, 0, 0, 0, false));
}

TEST_F(MultiDisplayAgentTest, GetCombinedDisplaySize) {
    EXPECT_CALL(*mMockStub, getDisplayConfigurations(_, _, _))
            .WillOnce([](::grpc::ClientContext* context,
                         const ::google::protobuf::Empty& request,
                         DisplayConfigurations* response) {
                populateDisplay(response->add_displays(), 0, 1080, 1920, 440, 1);
                populateDisplay(response->add_displays(), 1, 720, 1280, 320, 2);
                return ::grpc::Status::OK;
            });

    uint32_t w = 0, h = 0;
    mAgent->getCombinedDisplaySize(&w, &h);
    EXPECT_EQ(1080 + 720, w);
    EXPECT_EQ(1920, h); // max(1920, 1280)
}

TEST_F(MultiDisplayAgentTest, MultiDisplayParamValidate) {
    // Valid
    EXPECT_TRUE(mAgent->multiDisplayParamValidate(1, 720, 1280, 320, 0));

    // Invalid DPI < 120
    EXPECT_FALSE(mAgent->multiDisplayParamValidate(1, 720, 1280, 100, 0));

    // Invalid DPI > 640
    EXPECT_FALSE(mAgent->multiDisplayParamValidate(1, 720, 1280, 700, 0));

    // Invalid width < 320 * dpi / 160 (for 320 dpi, min width is 640)
    EXPECT_FALSE(mAgent->multiDisplayParamValidate(1, 600, 1280, 320, 0));

    // Invalid height < 320 * dpi / 160
    EXPECT_FALSE(mAgent->multiDisplayParamValidate(1, 720, 600, 320, 0));

    // Invalid ID > 11
    EXPECT_FALSE(mAgent->multiDisplayParamValidate(12, 720, 1280, 320, 0));
}
