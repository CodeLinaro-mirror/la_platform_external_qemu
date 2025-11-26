#include "android/skin/qt/extended-pages/grpc-cellular-controller.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "android/emulation/control/utils/ModemClient.h"
#include "grpc_endpoint_description.pb.h"

using ::android::emulation::control::EmulatorGrpcClient;
using ::android::emulation::control::ModemClient;
using ::android::emulation::remote::Endpoint;
using ::google::protobuf::Empty;
using ::grpc::ClientContext;
using ::grpc::Status;
using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::WithArg;

#pragma once

#include "gmock/gmock.h"

#include <condition_variable>
#include <mutex>

namespace android::emulation::control::incubating {
class ModemServiceImpl final
    : public android::emulation::control::Modem::Service {
public:
    ::grpc::Status setCellInfo(::grpc::ServerContext* context,
                               const CellInfo* request,
                               CellInfo* response) override {
        std::unique_lock<std::mutex> lock(mMutex);
        state = *request;
        mCv.notify_one();
        return Status::OK;
    }

    ::android::emulation::control::incubating::CellInfo state;
    std::mutex mMutex;
    std::condition_variable mCv;
};
}  // namespace android::emulation::control::incubating

using namespace ::android::emulation::control::incubating;

class GrpcCellularControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string uri = "localhost:" + std::to_string(mPort);
        grpc::ServerBuilder builder;
        mService = std::make_unique<android::emulation::control::ModemServiceImpl>();
        builder.RegisterService(mService.get());
        builder.AddListeningPort(uri, grpc::InsecureServerCredentials(),
                                 &mPort);

        mServer = builder.BuildAndStart();

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
                std::make_unique<GrpcCellularController>(sharedModemClient);
    }

    void TearDown() override {
        auto deadline = std::chrono::system_clock::now() +
                        std::chrono::milliseconds(50);
        mServer->Shutdown(deadline);
        mServer->Wait();
    }

    std::unique_ptr<GrpcCellularController> mController;
    std::unique_ptr<android::emulation::control::ModemServiceImpl> mService;
    std::unique_ptr<grpc::Server> mServer;
    int mPort{52103};
};

TEST_F(GrpcCellularControllerTest, SetCellularCallsGrpcClient) {
    CellularState state = {Cellular_Std_LTE, Cellular_Signal_Great,
                           Cellular_Stat_Roaming, Cellular_Stat_Denied,
                           Cellular_Temporarily_Not_Metered};

    mController->setCellular(state);

    std::unique_lock<std::mutex> lock(mService->mMutex);
    mService->mCv.wait(lock,
                       [this] { return mService->state.cell_standard() != 0; });
    ::android::emulation::control::incubating::CellInfo capturedProto =
            mService->state;

    EXPECT_EQ(capturedProto.cell_standard(),
              ::android::emulation::control::incubating::CellInfo::CELL_STANDARD_LTE);
    EXPECT_EQ(capturedProto.cell_signal_strength().level(),
              ::android::emulation::control::incubating::CellSignalStrength::
                      SIGNAL_STRENGTH_GREAT);
    EXPECT_EQ(capturedProto.cell_status_voice(),
              ::android::emulation::control::incubating::CellInfo::CELL_STATUS_ROAMING);
    EXPECT_EQ(capturedProto.cell_status_data(),
              ::android::emulation::control::incubating::CellInfo::CELL_STATUS_DENIED);
    EXPECT_EQ(capturedProto.cell_meter_status(),
              ::android::emulation::control::incubating::CellInfo::
                      CELL_METER_STATUS_TEMPORARILY_NOT_METERED);
}
