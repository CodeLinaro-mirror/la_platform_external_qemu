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

#include "android/android-ui/apps/fishtank/grpc/FishtankGrpcServer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "services/forwarder/service_forwarder.grpc.pb.h"

using android::emulation::forwarding::ServiceForwarder;
using android::fishtank::FishtankGrpcServer;
using testing::_;
using testing::Return;

class MockServiceForwarderStub : public ServiceForwarder::StubInterface {
public:
    MOCK_METHOD(::grpc::Status,
                registerForwarder,
                (::grpc::ClientContext * context,
                 const ::android::emulation::forwarding::ForwardingRule& request,
                 ::google::protobuf::Empty* response),
                (override));

    MOCK_METHOD(::grpc::Status,
                listForwardingRules,
                (::grpc::ClientContext * context,
                 const ::google::protobuf::Empty& request,
                 ::android::emulation::forwarding::ForwardingRuleList* response),
                (override));

private:
    MOCK_METHOD(
            (::grpc::ClientAsyncResponseReaderInterface<::google::protobuf::Empty>*),
            AsyncregisterForwarderRaw,
            (::grpc::ClientContext * context,
             const ::android::emulation::forwarding::ForwardingRule& request,
             ::grpc::CompletionQueue* cq),
            (override));
    MOCK_METHOD(
            (::grpc::ClientAsyncResponseReaderInterface<::google::protobuf::Empty>*),
            PrepareAsyncregisterForwarderRaw,
            (::grpc::ClientContext * context,
             const ::android::emulation::forwarding::ForwardingRule& request,
             ::grpc::CompletionQueue* cq),
            (override));
    MOCK_METHOD(
            (::grpc::ClientAsyncResponseReaderInterface<
                    ::android::emulation::forwarding::ForwardingRuleList>*),
            AsynclistForwardingRulesRaw,
            (::grpc::ClientContext * context,
             const ::google::protobuf::Empty& request,
             ::grpc::CompletionQueue* cq),
            (override));
    MOCK_METHOD(
            (::grpc::ClientAsyncResponseReaderInterface<
                    ::android::emulation::forwarding::ForwardingRuleList>*),
            PrepareAsynclistForwardingRulesRaw,
            (::grpc::ClientContext * context,
             const ::google::protobuf::Empty& request,
             ::grpc::CompletionQueue* cq),
            (override));
};

class FishtankGrpcServerTest : public ::testing::Test {
protected:
    FishtankGrpcServer mServer;
    MockServiceForwarderStub mMockStub;
};

TEST_F(FishtankGrpcServerTest, RegisterUiControllerSuccess) {
    EXPECT_CALL(mMockStub, registerForwarder(_, _, _))
            .WillOnce(Return(::grpc::Status::OK));

    auto status = mServer.registerUiController(&mMockStub);
    EXPECT_TRUE(status.ok());
}

TEST_F(FishtankGrpcServerTest, RegisterUiControllerWithProvidedContext) {
    grpc::ClientContext context;
    context.AddMetadata("test-key", "test-value");

    EXPECT_CALL(mMockStub, registerForwarder(&context, _, _))
            .WillOnce(Return(::grpc::Status::OK));

    auto status = mServer.registerUiController(&mMockStub, &context);
    EXPECT_TRUE(status.ok());
}

TEST_F(FishtankGrpcServerTest, RegisterUiControllerFailure) {
    EXPECT_CALL(mMockStub, registerForwarder(_, _, _))
            .WillOnce(Return(
                    ::grpc::Status(::grpc::StatusCode::INTERNAL, "Mock Error")));

    auto status = mServer.registerUiController(&mMockStub);
    EXPECT_FALSE(status.ok());
    EXPECT_THAT(status.message(), testing::HasSubstr("Mock Error"));
}

TEST_F(FishtankGrpcServerTest, RegisterUiControllerNullStub) {
    auto status = mServer.registerUiController(
            static_cast<ServiceForwarder::StubInterface*>(nullptr));
    EXPECT_FALSE(status.ok());
    EXPECT_TRUE(absl::IsInvalidArgument(status));
}
