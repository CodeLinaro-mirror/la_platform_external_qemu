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
#pragma once

#include <functional>
#include <memory>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "android/emulation/control/utils/GenericCallbackFunctions.h"
#include "google/protobuf/empty.pb.h"
#include "screen_recording_service.grpc.pb.h"
#include "screen_recording_service.pb.h"

namespace android {
namespace emulation {
namespace control {

using namespace incubating;
using ::google::protobuf::Empty;

/**
 * @brief A client for interacting with the Screen Recording Service.
 */
class SimpleScreenRecordingClient {
public:
    explicit SimpleScreenRecordingClient(
            std::shared_ptr<EmulatorGrpcClient> client,
            ScreenRecording::StubInterface* service = nullptr)
        : mClient(client), mService(service) {
        if (!service) {
            mService = client->stub<ScreenRecording>();
        }
    }

    /**
     * @brief Asynchronously starts a screen recording session.
     * @param info The recording information.
     * @param onDone Callback invoked when the operation completes.
     */
    void startRecordingAsync(RecordingInfo info,
                             OnCompleted<RecordingInfo> onDone);

    /**
     * @brief Asynchronously stops an active screen recording session.
     * @param info The recording information (usually just display ID is
     * needed).
     * @param onDone Callback invoked when the operation completes.
     */
    void stopRecordingAsync(RecordingInfo info,
                            OnCompleted<RecordingInfo> onDone);

    /**
     * @brief Asynchronously streams recording events from the emulator.
     * @param onEvent Callback invoked for each incoming event.
     * @param onDone Callback invoked when the stream terminates.
     */
    void streamRecordingEvents(OnEvent<RecordingInfo> onEvent,
                               OnFinished onDone);

private:
    std::shared_ptr<EmulatorGrpcClient> mClient;
    std::unique_ptr<ScreenRecording::StubInterface> mService;
};

}  // namespace control
}  // namespace emulation
}  // namespace android
