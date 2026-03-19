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

namespace android {
namespace emulation {
namespace control {
class EmulatorGrpcClient;
class Image;
class EmulatorController;
}  // namespace control
}  // namespace emulation
}  // namespace android

namespace grpc {
class ClientContext;
template <typename R>
class ClientReader;
}  // namespace grpc

/**
 * @brief Defines the transport mechanism for the image stream.
 */
enum class StreamTransport {
    /**
     * @brief Standard gRPC transport, where image data is sent in the gRPC
     * response.
     */
    Standard = 0,
    /**
     * @brief Shared memory transport, where image data is written to a
     * shared memory region.
     */
    MMAP = 1,
};

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
     * @param image A pointer to the Image protobuf message containing frame
     *              metadata and potentially pixel data (if using Standard
     *              transport).
     */
    using FrameCallback =
            std::function<void(const android::emulation::control::Image*)>;

    /**
     * @brief Constructs a SharedStreamEmulator object.
     *
     * @param handle The handle for the shared memory region (only used for MMAP
     *               transport).
     * @param callback The function to call when a new frame is ready.
     * @param w The width of the stream.
     * @param h The height of the stream.
     * @param transport The transport mechanism to use. Defaults to MMAP.
     * @param client A shared pointer to the EmulatorGrpcClient instance.
     *               Defaults to the singleton instance.
     */
    explicit SharedStreamEmulator(
            std::string_view handle,
            FrameCallback callback,
            int w,
            int h,
            StreamTransport transport = StreamTransport::MMAP,
            std::shared_ptr<android::emulation::control::EmulatorGrpcClient>
                    client = nullptr);
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
    StreamTransport mTransport;
    FrameCallback mFrameCallback;

    // We use a pimpl-like approach for the gRPC stub to avoid including
    // heavy headers in this header.
    struct Stub;
    std::unique_ptr<Stub> mStub;

    std::shared_ptr<grpc::ClientContext> mContext;
    std::unique_ptr<grpc::ClientReader<android::emulation::control::Image>>
            mReader;
    std::thread mStreamThread;
    std::atomic<bool> mIsStreaming{false};
};
