// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "android/skin/qt/SharedStreamEmulator.h"

#include <gmock/gmock.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>

#include "absl/random/random.h"

#include "absl/strings/str_cat.h"
#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"
#include "android/base/testing/TestSystem.h"
#include "android/base/testing/TestTempDir.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "emulator_controller.grpc.pb.h"

using namespace android::emulation::control;
using android::base::System;
using android::base::TestSystem;
using android::base::TestTempDir;


namespace {

// A mock implementation of the EmulatorController service for testing.
class MockEmulatorController final : public EmulatorController::Service {
public:
    grpc::Status streamScreenshot(grpc::ServerContext* context,
                                  const ImageFormat* request,
                                  grpc::ServerWriter<Image>* writer) override {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mRequest = *request;
        }
        mStreamStartedPromise.set_value();

        while (!context->IsCancelled()) {
            std::unique_lock<std::mutex> lock(mMutex);
            mFrameSentCv.wait_for(lock, std::chrono::milliseconds(10),
                                  [&] { return mFramesToSend > 0; });

            if (mFramesToSend > 0) {
                Image img;
                writer->Write(img);
                mFramesToSend--;
            }
        }
        return grpc::Status::OK;
    }

    void sendFrame() {
        std::lock_guard<std::mutex> lock(mMutex);
        mFramesToSend++;
        mFrameSentCv.notify_one();
    }

    std::promise<void> mStreamStartedPromise;
    ImageFormat mRequest;

private:
    std::mutex mMutex;
    std::condition_variable mFrameSentCv;
    int mFramesToSend{0};
};

// A test fixture for managing the gRPC server and client instances.
class SharedStreamEmulatorTest : public ::testing::Test {
protected:
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
        int selected_port = 0;
        std::string server_address = "localhost:0";
        grpc::ServerBuilder builder;
        builder.AddListeningPort(server_address,
                                 grpc::InsecureServerCredentials(),
                                 &selected_port);
        builder.RegisterService(&mService);
        mServer = builder.BuildAndStart();
        ASSERT_NE(mServer, nullptr);
        ASSERT_NE(selected_port, 0);
        server_address = absl::StrCat("localhost:", selected_port);

        std::string port = server_address.substr(server_address.find(':') + 1);
        mDiscoveryFile = std::make_unique<TmpDiscoveryFile>(
                absl::StrCat("grpc.port=", port));

        auto emuGrpcClient = EmulatorGrpcClient::Builder()
                                     .withDiscoveryFile(mDiscoveryFile->path())
                                     .build();
        ASSERT_TRUE(emuGrpcClient.ok());
        mClient = std::move(*emuGrpcClient);
    }

    void TearDown() override {
        if (mStreamer) {
            mStreamer->stopStream();
        }
        if (mServer) {
            auto deadline = std::chrono::system_clock::now() +
                            std::chrono::milliseconds(100);
            mServer->Shutdown(deadline);
        }
    }

    MockEmulatorController mService;
    std::unique_ptr<grpc::Server> mServer;
    std::unique_ptr<TmpDiscoveryFile> mDiscoveryFile;
    std::shared_ptr<EmulatorGrpcClient> mClient;
    std::unique_ptr<SharedStreamEmulator> mStreamer;
};

TEST_F(SharedStreamEmulatorTest, StartAndStop) {
    mStreamer = std::make_unique<SharedStreamEmulator>(
            "test_handle", nullptr, 1920, 1080,
            StreamTransport::MMAP, mClient);
    mStreamer->startStream();

    // Wait for the stream to be established on the server side.
    auto future = mService.mStreamStartedPromise.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);

    mStreamer->stopStream();
    // Test completes if it doesn't hang, indicating proper thread joining.
}

TEST_F(SharedStreamEmulatorTest, CorrectRequestParameters) {
    const int width = 800;
    const int height = 600;
    const std::string handle = "my_test_handle";

    mStreamer = std::make_unique<SharedStreamEmulator>(
            handle, nullptr, width, height,
            StreamTransport::MMAP, mClient);
    mStreamer->startStream();

    auto future = mService.mStreamStartedPromise.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);

    EXPECT_EQ(mService.mRequest.width(), width);
    EXPECT_EQ(mService.mRequest.height(), height);
    EXPECT_EQ(mService.mRequest.format(), ImageFormat::RGB888);
    EXPECT_EQ(mService.mRequest.transport().channel(), ImageTransport::MMAP);
    EXPECT_EQ(mService.mRequest.transport().handle(), handle);

    mStreamer->stopStream();
}

TEST_F(SharedStreamEmulatorTest, StandardTransportRequest) {
    const int width = 1280;
    const int height = 720;

    mStreamer = std::make_unique<SharedStreamEmulator>(
            "", nullptr, width, height,
            StreamTransport::Standard, mClient);
    mStreamer->startStream();

    auto future = mService.mStreamStartedPromise.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);

    EXPECT_EQ(mService.mRequest.width(), width);
    EXPECT_EQ(mService.mRequest.height(), height);
    EXPECT_EQ(mService.mRequest.transport().channel(),
              ImageTransport::TRANSPORT_CHANNEL_UNSPECIFIED);

    mStreamer->stopStream();
}

TEST_F(SharedStreamEmulatorTest, FrameCallbackIsInvoked) {
    const int numFrames = 5;
    std::mutex mu;
    std::condition_variable cv;
    int frameCount = 0;

    auto callback = [&](const Image* img) {
        std::lock_guard<std::mutex> lock(mu);
        frameCount++;
        cv.notify_one();
    };

    mStreamer = std::make_unique<SharedStreamEmulator>(
            "test_handle", callback, 1920, 1080,
            StreamTransport::MMAP, mClient);
    mStreamer->startStream();

    auto future = mService.mStreamStartedPromise.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);

    for (int i = 0; i < numFrames; ++i) {
        mService.sendFrame();
    }

    std::unique_lock<std::mutex> lock(mu);
    EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                            [&] { return frameCount == numFrames; }));

    mStreamer->stopStream();
}

TEST_F(SharedStreamEmulatorTest, StopWithoutStart) {
    mStreamer = std::make_unique<SharedStreamEmulator>(
            "test_handle", nullptr, 1920, 1080,
            StreamTransport::MMAP, mClient);
    mStreamer->stopStream();
    // Test passes if it doesn't crash or hang.
}

TEST_F(SharedStreamEmulatorTest, DoubleStartIsHandled) {
    mStreamer = std::make_unique<SharedStreamEmulator>(
            "test_handle", nullptr, 1920, 1080,
            StreamTransport::MMAP, mClient);
    mStreamer->startStream();

    auto future = mService.mStreamStartedPromise.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);

    // Second start should be a no-op.
    mStreamer->startStream();

    mStreamer->stopStream();
    // Test passes if it doesn't crash or hang.
}

}  // namespace
