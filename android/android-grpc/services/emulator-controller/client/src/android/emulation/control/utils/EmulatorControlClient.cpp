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
#include "android/emulation/control/utils/EmulatorControlClient.h"

#include <grpcpp/grpcpp.h>
#include <thread>
#include <utility>

#include "absl/status/status.h"
#include "android/emulation/control/utils/GenericCallbackFunctions.h"
#include "android/grpc/utils/SimpleAsyncGrpc.h"
#include "emulator_controller.pb.h"

// #define DEBUG 0
/* set  >1 for very verbose debugging */
#if DEBUG <= 1
#define DD(...) (void)0
#else
#define DD(...) dinfo(__VA_ARGS__)
#endif

namespace android {
namespace emulation {
namespace control {

EmulatorControlClient::EmulatorControlClient(
        std::shared_ptr<EmulatorGrpcClient> client,
        EmulatorController::StubInterface* service)
    : mClient(client), mService(service) {
    if (!service) {
        mService = client->stub<EmulatorController>();
    }
    mOnFinally = [this]() {
        if (--mOutstandingRpcs == 0) {
            // If this was the last RPC, notify the waiting destructor
            mOutstandingCondition.notify_one();
        }
    };
}

EmulatorControlClient::~EmulatorControlClient() {
    mClient->cancelAll();
    {
        std::unique_lock<std::mutex> lock(mOutstandingMutex);
        mOutstandingCondition.wait(lock,
                                   [this] { return mOutstandingRpcs == 0; });
    }
    {
        std::lock_guard<std::mutex> lock(mInputWriterAccess);
        mInputEventWriter = nullptr;
    }
    mService.reset();
    mClient.reset();
}

void EmulatorControlClient::setBatteryAsync(BatteryState state,
                                            OnCompleted<Empty> onDone) {
    DD("setBattery %s", state.ShortDebugString().c_str());
    auto [request, response, context] =
            createGrpcRequestContext<BatteryState, Empty>(mClient);
    request->CopyFrom(state);
    mOutstandingRpcs++;
    mService->async()->setBattery(
            context.get(), request, response,
            grpcCallCompletionHandler(context, request, response, onDone,
                                      mOnFinally));
}

void EmulatorControlClient::getScreenshotAsync(ImageFormat format,
                                               OnCompleted<Image> onDone) {
    auto [request, response, context] =
            createGrpcRequestContext<ImageFormat, Image>(mClient);
    request->CopyFrom(format);
    mOutstandingRpcs++;
    mService->async()->getScreenshot(
            context.get(), request, response,
            grpcCallCompletionHandler(context, request, response, onDone,
                                      mOnFinally));
}

void EmulatorControlClient::getStatusAsync(OnCompleted<EmulatorStatus> onDone) {
    auto [request, response, context] =
            createGrpcRequestContext<Empty, EmulatorStatus>(mClient);
    mOutstandingRpcs++;
    mService->async()->getStatus(
            context.get(), request, response,
            grpcCallCompletionHandler(context, request, response, onDone,
                                      mOnFinally));
}

void EmulatorControlClient::getDisplayConfigurationsAsync(
        OnCompleted<DisplayConfigurations> onDone) {
    auto [request, response, context] =
            createGrpcRequestContext<Empty, DisplayConfigurations>(mClient);
    mOutstandingRpcs++;
    mService->async()->getDisplayConfigurations(
            context.get(), request, response,
            grpcCallCompletionHandler(context, request, response, onDone,
                                      mOnFinally));
}

void EmulatorControlClient::getEmulatorStatusAsync(
        OnCompleted<EmulatorStatus> onDone) {
    auto [request, response, context] =
            createGrpcRequestContext<Empty, EmulatorStatus>(mClient);
    mOutstandingRpcs++;
    mService->async()->getStatus(
            context.get(), request, response,
            grpcCallCompletionHandler(context, request, response, onDone,
                                      mOnFinally));
}

void EmulatorControlClient::setDisplayConfigurationsAsync(
        DisplayConfigurations state,
        OnCompleted<DisplayConfigurations> onDone) {
    auto [request, response, context] =
            createGrpcRequestContext<DisplayConfigurations,
                                     DisplayConfigurations>(mClient);
    request->CopyFrom(state);
    mOutstandingRpcs++;
    mService->async()->setDisplayConfigurations(
            context.get(), request, response,
            grpcCallCompletionHandler(context, request, response, onDone,
                                      mOnFinally));
}

void EmulatorControlClient::registerNotificationListener(
        OnEvent<Notification> incoming,
        OnFinished onDone) {
    auto context = mClient->newContext();
    static google::protobuf::Empty empty;
    auto read = new SimpleClientLambdaReader<Notification>(
            context,
            [incoming](const Notification* event) {
                incoming(event);
                return grpc::Status::OK;
            },
            [onDone](auto status) {
                onDone(ConvertGrpcStatusToAbseilStatus(status));
            });
    mOutstandingRpcs++;
    mService->async()->streamNotification(context.get(), &empty, read);
    read->StartCall();
    read->StartRead();
}

void EmulatorControlClient::sendFingerprintAsync(Fingerprint finger,
                                                 OnCompleted<Empty> onDone) {
    auto [request, response, context] =
            createGrpcRequestContext<Fingerprint, Empty>(mClient);
    request->CopyFrom(finger);
    mOutstandingRpcs++;
    mService->async()->sendFingerprint(
            context.get(), request, response,
            grpcCallCompletionHandler(context, request, response, onDone,
                                      mOnFinally));
}

void EmulatorControlClient::setVmStateAsync(VmRunState state,
                                            OnCompleted<Empty> onDone) {
    auto [request, response, context] =
            createGrpcRequestContext<VmRunState, Empty>(mClient);
    request->CopyFrom(state);
    mOutstandingRpcs++;
    mService->async()->setVmState(
            context.get(), request, response,
            grpcCallCompletionHandler(context, request, response, onDone,
                                      mOnFinally));
}

void EmulatorControlClient::setClipboardAsync(std::string state,
                                              OnCompleted<Empty> onDone) {
    auto [request, response, context] =
            createGrpcRequestContext<ClipData, Empty>(mClient);
    request->set_text(state);
    mOutstandingRpcs++;
    mService->async()->setClipboard(
            context.get(), request, response,
            grpcCallCompletionHandler(context, request, response, onDone,
                                      mOnFinally));
}

void EmulatorControlClient::streamClipboardAsync(OnEvent<ClipData> cb,
                                                 OnFinished finished) {
    auto [request, response, context] =
            createGrpcRequestContext<Empty, ClipData>(mClient);
    mOutstandingRpcs++;
    auto read = new SimpleClientLambdaReader<ClipData>(
            context,
            [cb](const ClipData* event) {
                cb(event);
                return grpc::Status::OK;
            },
            [response, request, finished](auto status) {
                finished(ConvertGrpcStatusToAbseilStatus(status));
                delete response;
                delete request;
            });
    mService->async()->streamClipboard(context.get(), request, read);
    read->StartRead();
    read->StartCall();
}

void EmulatorControlClient::setBrightnessAsync(BrightnessValue bv,
                                               OnCompleted<Empty> onDone) {
    auto [request, response, context] =
            createGrpcRequestContext<BrightnessValue, Empty>(mClient);
    request->CopyFrom(bv);
    mOutstandingRpcs++;
    mService->async()->setBrightness(
            context.get(), request, response,
            grpcCallCompletionHandler(context, request, response, onDone,
                                      mOnFinally));
}

void EmulatorControlClient::setGpsAsync(GpsState gps,
                                        OnCompleted<Empty> onDone) {
    auto [request, response, context] =
            createGrpcRequestContext<GpsState, Empty>(mClient);
    request->CopyFrom(gps);
    mOutstandingRpcs++;
    mService->async()->setGps(
            context.get(), request, response,
            grpcCallCompletionHandler(context, request, response, onDone,
                                      mOnFinally));
}

absl::StatusOr<GpsState> EmulatorControlClient::getGps() {
    GpsState response;
    Empty empty;
    auto context = mClient->newContext();
    auto grpcStatus = mService->getGps(context.get(), empty, &response);
    if (!grpcStatus.ok()) {
        return ConvertGrpcStatusToAbseilStatus(grpcStatus);
    }

    return response;
}

SimpleClientWriter<InputEvent>*
EmulatorControlClient::asyncInputEventWriter() {
    std::lock_guard<std::mutex> lock(mInputWriterAccess);
    if (mInputEventWriter)
        return mInputEventWriter;

    static Empty empty;
    auto context = mClient->newContext();
    mInputEventWriter = new SimpleClientWriter<InputEvent>(std::move(context));
    mService->async()->streamInputEvent(mInputEventWriter->context(), &empty,
                                        mInputEventWriter);
    mInputEventWriter->StartCall();
    return mInputEventWriter;
}

}  // namespace control
}  // namespace emulation
}  // namespace android