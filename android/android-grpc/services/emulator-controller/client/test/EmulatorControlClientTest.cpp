// Copyright (C) 2023 The Android Open Source Project
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

#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"
#include "android/emulation/control/utils/EmulatorControlClient.h"

#include <gmock/gmock.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "emulator_controller.grpc.pb.h"

#include "aemu/base/files/PathUtils.h"
#include "android/base/testing/TestSystem.h"
#include "android/base/testing/TestTempDir.h"

using namespace android::emulation::control;
using android::base::System;
using android::base::TestSystem;
using android::base::TestTempDir;

namespace {

// A mock implementation of the EmulatorController service for testing.
class MockEmulatorController final : public EmulatorController::Service {
public:
    grpc::Status setBattery(grpc::ServerContext* context,
                            const BatteryState* request,
                            Empty* response) override {
        if (mFailSetBattery) {
            return grpc::Status::CANCELLED;
        }
        return grpc::Status::OK;
    }

    grpc::Status getScreenshot(grpc::ServerContext* context,
                               const ImageFormat* request,
                               Image* response) override {
        response->mutable_format()->CopyFrom(*request);
        response->set_image("fakedata");
        return grpc::Status::OK;
    }

    grpc::Status getGps(grpc::ServerContext* context,
                        const Empty* request,
                        GpsState* response) override {
        if (mFailGetGps) {
            return grpc::Status(grpc::StatusCode::CANCELLED,
                                "Failed to get GPS");
        }
        response->set_latitude(12.34);
        return grpc::Status::OK;
    }

    grpc::Status streamNotification(
            grpc::ServerContext* context,
            const Empty* request,
            grpc::ServerWriter<Notification>* writer) override {
        mStreamStartedPromise.set_value();
        while (!context->IsCancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return grpc::Status::OK;
    }

    grpc::Status streamInputEvent(grpc::ServerContext* context,
                                  grpc::ServerReader<InputEvent>* reader,
                                  Empty* response) override {
        InputEvent event;
        streamInputEventCounter = 0;
        while (reader->Read(&event)) {
            // Do nothing.
            std::lock_guard<std::mutex> lock(streamInputMutex);
            streamInputEventCounter++;
            streamInputCv.notify_one();
        }
        return grpc::Status::OK;
    }

    grpc::Status streamClipboard(grpc::ServerContext* context,
                                 const Empty* request,
                                 grpc::ServerWriter<ClipData>* writer) override {
        mClipboardStartedPromise.set_value();
        while (!context->IsCancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return grpc::Status::OK;
    }

    std::promise<void> mStreamStartedPromise;
    std::promise<void> mClipboardStartedPromise;

    std::mutex streamInputMutex;
    std::condition_variable streamInputCv;
    int streamInputEventCounter = 0;
    bool mFailSetBattery = false;
    bool mFailGetGps = false;
};

// A test fixture for managing the gRPC server and client instances.
class EmulatorGrpcClientTest : public ::testing::Test {
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

    MockEmulatorController service;
    std::unique_ptr<grpc::Server> server;
    std::string server_address;
};

class EmulatorControlClientTest : public EmulatorGrpcClientTest {
protected:
    void SetUp() override {
        StartServer();
        std::string port = server_address.substr(server_address.find(':') + 1);
        mDiscoveryFile = std::make_unique<TmpDiscoveryFile>(
                absl::StrCat("grpc.port=", port));

        auto emuGrpcClient = EmulatorGrpcClient::Builder()
                                     .withDiscoveryFile(mDiscoveryFile->path())
                                     .build();
        ASSERT_TRUE(emuGrpcClient.ok());
        mClient = std::shared_ptr<EmulatorControlClient>(
                new EmulatorControlClient(std::move(*emuGrpcClient)));
    }

    void TearDown() override {
        mClient.reset();
        EmulatorGrpcClientTest::TearDown();
    }

    std::shared_ptr<EmulatorControlClient> mClient;
    std::unique_ptr<TmpDiscoveryFile> mDiscoveryFile;
};

TEST_F(EmulatorControlClientTest, SetBatterySuccess) {
    BatteryState state;
    state.set_chargelevel(80);

    std::mutex mu;
    std::condition_variable cv;
    bool onDoneCalled = false;

    mClient->setBatteryAsync(state, [&](absl::StatusOr<Empty*> result) {
        EXPECT_TRUE(result.ok());
        std::lock_guard<std::mutex> lock(mu);
        onDoneCalled = true;
        cv.notify_one();
    });

    std::unique_lock<std::mutex> lock(mu);
    EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(1),
                            [&] { return onDoneCalled; }));
}

TEST_F(EmulatorControlClientTest, SetBatteryFailure) {
    service.mFailSetBattery = true;
    BatteryState state;

    std::mutex mu;
    std::condition_variable cv;
    bool onDoneCalled = false;

    mClient->setBatteryAsync(state, [&](absl::StatusOr<Empty*> result) {
        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.status().code(), absl::StatusCode::kCancelled);
        std::lock_guard<std::mutex> lock(mu);
        onDoneCalled = true;
        cv.notify_one();
    });

    std::unique_lock<std::mutex> lock(mu);
    EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(1),
                            [&] { return onDoneCalled; }));
}

TEST_F(EmulatorControlClientTest, GetScreenshotSuccess) {
    ImageFormat format;
    format.set_format(ImageFormat::PNG);

    std::mutex mu;
    std::condition_variable cv;
    bool onDoneCalled = false;

    mClient->getScreenshotAsync(format, [&](absl::StatusOr<Image*> result) {
        EXPECT_TRUE(result.ok());
        EXPECT_EQ((*result)->image(), "fakedata");
        EXPECT_EQ((*result)->format().format(), ImageFormat::PNG);
        std::lock_guard<std::mutex> lock(mu);
        onDoneCalled = true;
        cv.notify_one();
    });

    std::unique_lock<std::mutex> lock(mu);
    EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(1),
                            [&] { return onDoneCalled; }));
}

TEST_F(EmulatorControlClientTest, GetGpsSuccess) {
    auto result = mClient->getGps();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result->latitude(), 12.34);
}

TEST_F(EmulatorControlClientTest, GetGpsFailure) {
    service.mFailGetGps = true;
    auto result = mClient->getGps();
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), absl::StatusCode::kCancelled);
}

TEST_F(EmulatorControlClientTest, AsyncInputEventWriter) {
    auto writer = mClient->asyncInputEventWriter();
    EXPECT_NE(writer, nullptr);

    // Subsequent calls should return the same writer instance.
    auto writer2 = mClient->asyncInputEventWriter();
    EXPECT_EQ(writer, writer2);
}

TEST_F(EmulatorControlClientTest, AsyncInputEventWriterCanWrite) {
    auto writer = mClient->asyncInputEventWriter();
    EXPECT_NE(writer, nullptr);
    InputEvent event;
    writer->Write(event);
    std::unique_lock<std::mutex> lock(service.streamInputMutex);
    EXPECT_TRUE(service.streamInputCv.wait_for(
            lock, std::chrono::seconds(1),
            [&] { return service.streamInputEventCounter == 1; }))
            << "Failed to write input event";
}

TEST_F(EmulatorControlClientTest, RegisterNotificationListenerDoesNotHangOnDestruction) {
    auto future = service.mStreamStartedPromise.get_future();
    mClient->registerNotificationListener([](const Notification* n) {},
                                          [](auto s) {});

    // Wait for the stream to actually start
    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);

    // Resetting the client should trigger cancellation and wait for completion.
    // If there's a hang, this test will time out.
    mClient.reset();
}

TEST_F(EmulatorControlClientTest, StreamClipboardDoesNotHangOnDestruction) {
    auto future = service.mClipboardStartedPromise.get_future();
    mClient->streamClipboardAsync([](const ClipData* n) {}, [](auto s) {});

    // Wait for the stream to actually start
    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);

    // Resetting the client should trigger cancellation and wait for completion.
    // If there's a hang, this test will time out.
    mClient.reset();
}

TEST_F(EmulatorControlClientTest, AsyncInputEventWriterDoesNotHangOnDestruction) {
    auto writer = mClient->asyncInputEventWriter();
    EXPECT_NE(writer, nullptr);

    // Resetting the client should trigger cancellation and wait for completion.
    // If there's a hang, this test will time out.
    mClient.reset();
}
}  // namespace
