// Copyright 2025 The Android Open Source Project
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

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "emulator_controller.grpc.pb.h"

/**
 * @brief Manages a gRPC stream to receive screenshot frames from the emulator.
 *
 * This class sets up a gRPC stream that notifies when a new frame is
 * available. The actual frame data is transferred via a shared memory region,
 * identified by a handle provided during construction. A background thread is
 * used to listen for incoming frames.
 */
class SharedStreamEmulator {
public:
    /**
     * @brief A callback function that is invoked when a new frame is available.
     *
     * The consumer is responsible for reading the frame data from the
     * corresponding shared memory region when this callback is triggered.
     */
    using FrameCallback = std::function<void()>;

    /**
     * @brief Constructs a SharedStreamEmulator object.
     *
     * @param handle The handle for the shared memory region. This can be a named
     *               handle for a shared memory object, or a URI for a
     *               memory-mapped file (e.g., "file:///path/to/file").
     * @param callback The function to call when a new frame is ready.
     * @param w The width of the stream.
     * @param h The height of the stream.
     * @param client A shared pointer to the EmulatorGrpcClient instance.
     *               Defaults to the singleton instance.
     */
    explicit SharedStreamEmulator(
            std::string_view handle,
            FrameCallback callback,
            int w,
            int h,
            std::shared_ptr<
                    android::emulation::control::EmulatorGrpcClient> client =
                    android::emulation::control::EmulatorGrpcClient::me());
    ~SharedStreamEmulator();

    // This class manages a thread and raw pointers, it is not safe to copy or
    // move.
    SharedStreamEmulator(const SharedStreamEmulator&) = delete;
    SharedStreamEmulator& operator=(const SharedStreamEmulator&) = delete;
    SharedStreamEmulator(SharedStreamEmulator&&) = delete;
    SharedStreamEmulator& operator=(SharedStreamEmulator&&) = delete;

    /**
     * @brief Starts the gRPC stream on a background thread.
     *
     * If the stream is already running, this function does nothing.
     */
    void startStream();

    /**
     * @brief Stops the gRPC stream and joins the background thread.
     *
     * If the stream is not running, this function does nothing.
     */
    void stopStream();

private:
    void streamLoop();

    int mWidth;
    int mHeight;
    std::shared_ptr<android::emulation::control::EmulatorGrpcClient> mClient;
    std::string mHandle;
    FrameCallback mFrameCallback;
    std::unique_ptr<android::emulation::control::EmulatorController::Stub>
            mStub;
    std::shared_ptr<grpc::ClientContext> mContext;
    std::unique_ptr<grpc::ClientReader<android::emulation::control::Image>>
            mReader;
    std::thread mStreamThread;
    std::atomic<bool> mIsStreaming{false};
};
