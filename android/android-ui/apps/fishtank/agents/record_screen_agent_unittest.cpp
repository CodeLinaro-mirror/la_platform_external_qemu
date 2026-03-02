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
#include <fstream>
#include <memory>
#include <thread>

#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"
#include "android/base/testing/TestSystem.h"
#include "android/base/testing/TestTempDir.h"
#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "emulator_controller.grpc.pb.h"
#include "fishtank_agents.h"
#include "host-common/record_screen_agent.h"

using namespace android::emulation::control;
using namespace testing;
using android::base::System;
using android::base::TestSystem;
using android::base::TestTempDir;

// Defined in test_client_setup.cpp
extern std::shared_ptr<EmulatorControlClient> gTestControlClient;

// A mock implementation of the EmulatorController service for testing.
class MockEmulatorController final : public EmulatorController::Service {
public:
    MOCK_METHOD(::grpc::Status, getScreenshot, (::grpc::ServerContext* context, const ImageFormat* request, Image* response), (override));
};

class RecordScreenAgentTest : public Test {
protected:
    void TearDown() override {
        if (server) {
            server->Shutdown();
        }
    }

    void StartServer() {
        int selected_port = 0;
        server_address = "localhost:0";
        grpc::ServerBuilder builder;
        builder.AddListeningPort(server_address,
                                 grpc::InsecureServerCredentials(),
                                 &selected_port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        ASSERT_NE(server, nullptr);
        ASSERT_NE(selected_port, 0);
        server_address = absl::StrCat("localhost:", selected_port);
    }

    // RAII helper for creating and cleaning up a temporary discovery file.
    class TmpDiscoveryFile {
    public:
        explicit TmpDiscoveryFile(const std::string& content)
            : mTestSystem("/", System::kProgramBitness) {
            TestTempDir* testDir = mTestSystem.getTempRoot();
            std::string tmp_dir = testDir->pathString();
            std::string file_name = absl::StrCat(
                    "grpc_test_",
                    absl::Hex(absl::Uniform<uint64_t>(absl::BitGen()),
                              absl::kSpacePad16));
            mPath = android::base::pj(tmp_dir, file_name);
            std::ofstream out(mPath);
            out << content;
        }
        ~TmpDiscoveryFile() {
            std::error_code ec;
            std::filesystem::remove(mPath, ec);
        }
        const std::string path() const { return mPath; }

    private:
        TestSystem mTestSystem;
        std::string mPath;
    };

    void SetUp() override {
        StartServer();
        std::string port = server_address.substr(server_address.find(':') + 1);
        mDiscoveryFile = std::make_unique<TmpDiscoveryFile>(
                absl::StrCat("grpc.port=", port));

        auto emuGrpcClient = EmulatorGrpcClient::Builder()
                                     .withDiscoveryFile(mDiscoveryFile->path())
                                     .build();
        ASSERT_TRUE(emuGrpcClient.ok());
        gTestControlClient = std::make_shared<EmulatorControlClient>(
                std::shared_ptr<EmulatorGrpcClient>(std::move(*emuGrpcClient)));

        mAgent = &sFishtankQAndroidRecordScreenAgent;
    }

    MockEmulatorController service;
    std::unique_ptr<grpc::Server> server;
    std::string server_address;
    std::unique_ptr<TmpDiscoveryFile> mDiscoveryFile;
    const QAndroidRecordScreenAgent* mAgent;
};

TEST_F(RecordScreenAgentTest, DoSnapSuccess) {
    std::string testFile = "test_screenshot.png";
    std::string imageData = "fake-png-data";

    EXPECT_CALL(service, getScreenshot(_, _, _))
            .WillOnce([&](::grpc::ServerContext* context,
                         const ImageFormat* format, Image* response) {
                response->set_image(imageData);
                return ::grpc::Status::OK;
            });

    // Run doSnap
    bool result = mAgent->doSnap(testFile.c_str(), 0);

    EXPECT_TRUE(result);

    // Verify file content
    std::ifstream file(testFile, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    std::string content((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
    EXPECT_EQ(content, imageData);
    file.close();

    // Clean up
    std::remove(testFile.c_str());
}

TEST_F(RecordScreenAgentTest, DoSnapFailure) {
    std::string testFile = "test_screenshot_fail.png";

    EXPECT_CALL(service, getScreenshot(_, _, _))
            .WillOnce([&](::grpc::ServerContext* context,
                         const ImageFormat* format, Image* response) {
                return ::grpc::Status(::grpc::StatusCode::INTERNAL, "Error");
            });

    // Run doSnap
    bool result = mAgent->doSnap(testFile.c_str(), 0);

    EXPECT_FALSE(result);

    // Verify file does not exist
    std::ifstream file(testFile);
    EXPECT_FALSE(file.is_open());
}
