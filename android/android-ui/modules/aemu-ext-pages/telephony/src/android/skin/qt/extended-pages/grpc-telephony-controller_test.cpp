// Copyright (C) 2026 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/skin/qt/extended-pages/grpc-telephony-controller.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "android/emulation/control/utils/ModemClient.h"
#include "android/skin/qt/function-runner.h"
#include "grpc_endpoint_description.pb.h"

#ifdef _WIN32
#undef ERROR
#endif

using ::android::emulation::control::EmulatorGrpcClient;
using ::android::emulation::control::ModemClient;
using ::android::emulation::remote::Endpoint;
using ::google::protobuf::Empty;
using ::grpc::ClientContext;
using ::grpc::Status;
using ::testing::_;

namespace android::emulation::control::incubating {
class ModemServiceImpl final
    : public android::emulation::control::Modem::Service {
public:
    ::grpc::Status createCall(::grpc::ServerContext* context,
                              const Call* request,
                              Call* response) override {
        std::unique_lock<std::mutex> lock(mMutex);
        lastCall = *request;
        lastOp = "createCall";
        mCv.notify_one();
        return mNextStatus;
    }

    ::grpc::Status updateCall(::grpc::ServerContext* context,
                              const Call* request,
                              Call* response) override {
        std::unique_lock<std::mutex> lock(mMutex);
        lastCall = *request;
        lastOp = "updateCall";
        mCv.notify_one();
        return mNextStatus;
    }

    ::grpc::Status deleteCall(::grpc::ServerContext* context,
                              const Call* request,
                              google::protobuf::Empty* response) override {
        std::unique_lock<std::mutex> lock(mMutex);
        lastCall = *request;
        lastOp = "deleteCall";
        mCv.notify_one();
        return mNextStatus;
    }

    ::grpc::Status receiveSms(::grpc::ServerContext* context,
                              const SmsMessage* request,
                              google::protobuf::Empty* response) override {
        std::unique_lock<std::mutex> lock(mMutex);
        lastSms = *request;
        lastOp = "receiveSms";
        mCv.notify_one();
        return mNextStatus;
    }

    ::grpc::Status receivePhoneEvents(
            ::grpc::ServerContext* context,
            const google::protobuf::Empty* request,
            ::grpc::ServerWriter<PhoneEvent>* writer) override {
        std::unique_lock<std::mutex> lock(mMutex);
        mWriter = writer;
        mCv.notify_one();
        // Keep the stream open for a bit
        mStreamFinished.wait(lock);
        return Status::OK;
    }

    ::grpc::Status updateClock(::grpc::ServerContext* context,
                               const google::protobuf::Empty* request,
                               google::protobuf::Empty* response) override {
        std::unique_lock<std::mutex> lock(mMutex);
        lastOp = "updateTime";
        mCv.notify_one();
        return mNextStatus;
    }

    std::string lastOp;
    Call lastCall;
    SmsMessage lastSms;
    ::grpc::ServerWriter<PhoneEvent>* mWriter = nullptr;
    std::mutex mMutex;
    std::condition_variable mCv;
    std::condition_variable mStreamFinished;
    ::grpc::Status mNextStatus = Status::OK;
};
}  // namespace android::emulation::control::incubating

using namespace ::android::emulation::control::incubating;

class GrpcTelephonyControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        grpc::ServerBuilder builder;
        mService = std::make_unique<ModemServiceImpl>();
        builder.RegisterService(mService.get());
        // Use port 0 to let the OS pick an available port
        builder.AddListeningPort("localhost:0",
                                 grpc::InsecureServerCredentials(), &mPort);

        mServer = builder.BuildAndStart();
        std::string uri = "localhost:" + std::to_string(mPort);

        Endpoint dest;
        dest.set_target(uri);
        EmulatorGrpcClient::Builder clientBuilder;
        clientBuilder.withEndpoint(dest);
        auto maybeClient = clientBuilder.build();
        ASSERT_TRUE(maybeClient.ok());
        auto uniqueClient = std::move(maybeClient.value());
        auto sharedClient =
                std::shared_ptr<EmulatorGrpcClient>(uniqueClient.release());

        auto sharedModemClient = std::make_shared<ModemClient>(sharedClient);
        mController =
                std::make_unique<GrpcTelephonyController>(sharedModemClient);
    }

    void TearDown() override {
        mService->mStreamFinished.notify_all();
        auto deadline = std::chrono::system_clock::now() +
                        std::chrono::milliseconds(50);
        mServer->Shutdown(deadline);
        mServer->Wait();
    }

    std::unique_ptr<GrpcTelephonyController> mController;
    std::unique_ptr<ModemServiceImpl> mService;
    std::unique_ptr<grpc::Server> mServer;
    int mPort{0};
};

TEST_F(GrpcTelephonyControllerTest, InitCallSuccess) {
    TelephonyResponseStatus resultStatus = TelephonyResponseStatus::ERROR;
    mController->initCallAsync("123456", [&](auto s) { resultStatus = s; });

    std::unique_lock<std::mutex> lock(mService->mMutex);
    mService->mCv.wait(lock,
                       [this] { return mService->lastOp == "createCall"; });
    EXPECT_EQ(mService->lastCall.number(), "123456");
    EXPECT_EQ(mService->lastCall.direction(), Call::CALL_DIRECTION_INBOUND);

    // Give some time for the callback to be triggered.
    for (int i = 0; i < 10 && resultStatus != TelephonyResponseStatus::OK;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(resultStatus, TelephonyResponseStatus::OK);
}

TEST_F(GrpcTelephonyControllerTest, InitCallRadioOff) {
    mService->mNextStatus =
            Status(grpc::StatusCode::FAILED_PRECONDITION, "The radio is off");
    TelephonyResponseStatus resultStatus = TelephonyResponseStatus::OK;
    mController->initCallAsync("123456", [&](auto s) { resultStatus = s; });

    std::unique_lock<std::mutex> lock(mService->mMutex);
    mService->mCv.wait(lock,
                       [this] { return mService->lastOp == "createCall"; });

    for (int i = 0;
         i < 10 && resultStatus != TelephonyResponseStatus::RADIO_OFF; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(resultStatus, TelephonyResponseStatus::RADIO_OFF);
}

TEST_F(GrpcTelephonyControllerTest, InitCallGenericError) {
    mService->mNextStatus = Status(grpc::StatusCode::INTERNAL, "Unknown error");
    TelephonyResponseStatus resultStatus = TelephonyResponseStatus::OK;
    mController->initCallAsync("123456", [&](auto s) { resultStatus = s; });

    std::unique_lock<std::mutex> lock(mService->mMutex);
    mService->mCv.wait(lock,
                       [this] { return mService->lastOp == "createCall"; });

    for (int i = 0; i < 10 && resultStatus != TelephonyResponseStatus::ERROR;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(resultStatus, TelephonyResponseStatus::ERROR);
}

TEST_F(GrpcTelephonyControllerTest, DisconnectCall) {
    TelephonyResponseStatus resultStatus = TelephonyResponseStatus::ERROR;
    mController->disconnectCallAsync("654321",
                                     [&](auto s) { resultStatus = s; });

    std::unique_lock<std::mutex> lock(mService->mMutex);
    mService->mCv.wait(lock,
                       [this] { return mService->lastOp == "deleteCall"; });
    EXPECT_EQ(mService->lastCall.number(), "654321");

    for (int i = 0; i < 10 && resultStatus != TelephonyResponseStatus::OK;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(resultStatus, TelephonyResponseStatus::OK);
}

TEST_F(GrpcTelephonyControllerTest, HoldCall) {
    mController->holdCallAsync("111");

    std::unique_lock<std::mutex> lock(mService->mMutex);
    mService->mCv.wait(lock,
                       [this] { return mService->lastOp == "updateCall"; });
    EXPECT_EQ(mService->lastCall.number(), "111");
    EXPECT_EQ(mService->lastCall.state(), Call::CALL_STATE_HELD);
}

TEST_F(GrpcTelephonyControllerTest, SendSms) {
    mController->sendSmsAsync("555", "Hello World");

    std::unique_lock<std::mutex> lock(mService->mMutex);
    mService->mCv.wait(lock,
                       [this] { return mService->lastOp == "receiveSms"; });
    EXPECT_EQ(mService->lastSms.number(), "555");
    EXPECT_EQ(mService->lastSms.text(), "Hello World");
}

TEST_F(GrpcTelephonyControllerTest, UpdateTimeAsync) {
    TelephonyResponseStatus resultStatus = TelephonyResponseStatus::ERROR;
    mController->updateTimeAsync([&](auto s) { resultStatus = s; });

    std::unique_lock<std::mutex> lock(mService->mMutex);
    mService->mCv.wait(lock,
                       [this] { return mService->lastOp == "updateTime"; });

    for (int i = 0; i < 10 && resultStatus != TelephonyResponseStatus::OK;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(resultStatus, TelephonyResponseStatus::OK);
}

TEST_F(GrpcTelephonyControllerTest, CallStateCallback) {
    int receivedCalls = -1;
    mController->setCallStateCallback(
            [&](int calls) { receivedCalls = calls; });

    // Wait for the server to receive the request
    {
        std::unique_lock<std::mutex> lock(mService->mMutex);
        mService->mCv.wait(lock,
                           [this] { return mService->mWriter != nullptr; });

        PhoneEvent event;
        event.set_type(PhoneEvent::PHONE_EVENT_TYPE_ACTIVE);
        event.mutable_active()->add_calls()->set_number("123");
        event.mutable_active()->add_calls()->set_number("456");
        mService->mWriter->Write(event);
    }

    // Give some time for the callback to be triggered.
    for (int i = 0; i < 100 && receivedCalls != 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(receivedCalls, 2);
}
