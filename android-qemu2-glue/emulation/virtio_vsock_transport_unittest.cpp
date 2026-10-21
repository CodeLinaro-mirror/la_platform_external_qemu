// Copyright 2026 The Android Open Source Project
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

#ifndef _WIN32

#include <android-qemu2-glue/emulation/virtio_vsock_transport.h>

#include <errno.h>
#include <gtest/gtest.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "aemu/base/async/Looper.h"
#include "aemu/base/async/ThreadLooper.h"
#include "aemu/base/sockets/ScopedSocket.h"
#include "aemu/base/sockets/SocketUtils.h"
#include "android/utils/sockets.h"

namespace {
std::function<size_t(uint64_t, const void*, size_t)> gHostToGuestSendCallback;
}  // namespace

size_t virtio_vsock_host_to_guest_send(uint64_t handle,
                                       const void* data,
                                       size_t size) {
    if (gHostToGuestSendCallback) {
        return gHostToGuestSendCallback(handle, data, size);
    }
    return size;
}

bool android_unix_pipe_check_path(const char* /*path*/) {
    return true;
}

namespace android {
namespace emulation {
namespace {

constexpr uint64_t kTestStreamKey = 0x123456789ABCDEF0ULL;
constexpr size_t kChunkSize = 16384;
constexpr size_t kLargeBuffer = size_t(64) << 20;
constexpr int kDefaultTestSocketBufSize = 256 * 1024;

std::vector<uint8_t> makePattern(size_t size, uint8_t seed = 0) {
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>((i * 131 + seed) & 0xFF);
    }
    return data;
}

class VirtioVsockTransportTest : public ::testing::Test {
protected:
    void SetUp() override {
        gHostToGuestSendCallback = nullptr;
        mLooper = android::base::ThreadLooper::get();
        ASSERT_NE(nullptr, mLooper);
        virtio_vsock_new_transport_init();

        int fd0 = -1;
        int fd1 = -1;
        ASSERT_EQ(0, android::base::socketCreatePair(&fd0, &fd1));
        android::base::socketSetNonBlocking(fd0);
        android::base::socketSetNonBlocking(fd1);
        mTransportFd = fd0;
        mPeerSocket.reset(fd1);

        // macOS defaults AF_UNIX stream socketpair buffers to 8 KiB. Raise the
        // buffer size to 256 KiB so multi-chunk payloads (up to ~66 KiB) can be
        // queued in a single wakeup across both Linux and macOS.
        setSocketBufferSize(kDefaultTestSocketBufSize);
    }

    void TearDown() override {
        mTransport.reset();
        mPeerSocket.reset(-1);
        gHostToGuestSendCallback = nullptr;
    }

    void setSocketBufferSize(int bufSize) {
        setsockopt(mTransportFd, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));
        setsockopt(mTransportFd, SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));
        setsockopt(mPeerSocket.get(), SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));
        setsockopt(mPeerSocket.get(), SOL_SOCKET, SO_RCVBUF, &bufSize, sizeof(bufSize));
    }

    // Runs a single non-blocking iteration of the thread looper, firing any
    // currently ready FdWatch callbacks once and returning immediately.
    void runOneLooperIteration() {
        mLooper->scheduleCallback([looper = mLooper]() {
            looper->forceQuit();
        });
        mLooper->runWithDeadlineMs(android::base::Looper::kDurationInfinite);
    }

    std::vector<uint8_t> drainPeerSocket() {
        std::vector<uint8_t> out;
        uint8_t buf[kChunkSize];
        while (true) {
            const int n = socket_recv(mPeerSocket.get(), buf, sizeof(buf));
            if (n > 0) {
                out.insert(out.end(), buf, buf + n);
            } else {
                break;
            }
        }
        return out;
    }

    android::base::Looper* mLooper = nullptr;
    int mTransportFd = -1;
    android::base::ScopedSocket mPeerSocket;
    std::shared_ptr<IVsockNewTransport> mTransport;
};

TEST_F(VirtioVsockTransportTest, ReadDrainLoopDrainsMultipleChunksInSingleWakeup) {
    std::vector<uint8_t> received;
    size_t sendCallCount = 0;

    gHostToGuestSendCallback =
            [&](uint64_t key, const void* data, size_t size) -> size_t {
        EXPECT_EQ(kTestStreamKey, key);
        const auto* bytes = static_cast<const uint8_t*>(data);
        received.insert(received.end(), bytes, bytes + size);
        ++sendCallCount;
        return received.size();
    };
    mTransport = vsock_create_unix_transport_from_fd(kTestStreamKey, mTransportFd);

    // Write 4 full 16 KiB chunks + a partial chunk (66,770 bytes total).
    const std::vector<uint8_t> payload = makePattern(4 * kChunkSize + 1234, 7);
    const int written =
            socket_send(mPeerSocket.get(), payload.data(), payload.size());
    ASSERT_EQ(static_cast<int>(payload.size()), written);

    // A single looper wakeup should drain all chunks in ioFunc's while(true) loop.
    runOneLooperIteration();

    EXPECT_GE(sendCallCount, 5u);
    EXPECT_EQ(payload, received);
}

TEST_F(VirtioVsockTransportTest, ReadBackpressureStopsAtLargeBufferThresholdAndResumes) {
    std::vector<uint8_t> received;
    size_t sendCallCount = 0;
    bool simulateBackpressure = true;

    gHostToGuestSendCallback =
            [&](uint64_t key, const void* data, size_t size) -> size_t {
        EXPECT_EQ(kTestStreamKey, key);
        const auto* bytes = static_cast<const uint8_t*>(data);
        received.insert(received.end(), bytes, bytes + size);
        ++sendCallCount;
        return simulateBackpressure ? kLargeBuffer : received.size();
    };
    mTransport = vsock_create_unix_transport_from_fd(kTestStreamKey, mTransportFd);

    // Send 3 full 16 KiB chunks (49,152 bytes).
    const std::vector<uint8_t> payload = makePattern(3 * kChunkSize, 19);
    ASSERT_EQ(static_cast<int>(payload.size()),
              socket_send(mPeerSocket.get(), payload.data(), payload.size()));

    // First wakeup hits bufSize >= kLargeBuffer after the first read chunk
    // and breaks out of the read loop to apply backpressure.
    runOneLooperIteration();
    EXPECT_EQ(1u, sendCallCount);
    EXPECT_GT(received.size(), 0u);
    EXPECT_LE(received.size(), kChunkSize);

    // Once the guest drains buffer space below kLargeBuffer, the next wakeup
    // drains the remaining chunks from the socket.
    simulateBackpressure = false;
    runOneLooperIteration();
    EXPECT_GE(sendCallCount, 3u);
    EXPECT_EQ(payload, received);
}

TEST_F(VirtioVsockTransportTest, ReadStopsWhenHostToGuestSendReturnsZero) {
    size_t sendCallCount = 0;

    gHostToGuestSendCallback =
            [&](uint64_t key, const void* /*data*/, size_t /*size*/) -> size_t {
        EXPECT_EQ(kTestStreamKey, key);
        ++sendCallCount;
        return 0;
    };
    mTransport = vsock_create_unix_transport_from_fd(kTestStreamKey, mTransportFd);

    const std::vector<uint8_t> payload = makePattern(2 * kChunkSize, 33);
    ASSERT_EQ(static_cast<int>(payload.size()),
              socket_send(mPeerSocket.get(), payload.data(), payload.size()));

    runOneLooperIteration();
    EXPECT_EQ(1u, sendCallCount);
}

TEST_F(VirtioVsockTransportTest, WriteDrainLoopSendsMultipleChunksInSingleWakeup) {
    mTransport = vsock_create_unix_transport_from_fd(kTestStreamKey, mTransportFd);

    const std::vector<uint8_t> payload = makePattern(4 * kChunkSize + 777, 42);
    std::shared_ptr<IVsockNewTransport> nextStage;
    EXPECT_EQ(static_cast<int>(payload.size()),
              mTransport->writeGuestToHost(payload.data(), payload.size(), &nextStage));
    EXPECT_EQ(nullptr, nextStage);

    // A single LOOP_IO_WRITE wakeup should drain all 66,313 bytes across 5 chunks.
    runOneLooperIteration();

    const std::vector<uint8_t> received = drainPeerSocket();
    EXPECT_EQ(payload, received);

    // Verify that loopIo_dontWantWrite was called when mGuestToHostBuf became empty.
    runOneLooperIteration();
    EXPECT_TRUE(drainPeerSocket().empty());
}

TEST_F(VirtioVsockTransportTest, WritePartialAndBackpressureResumesWithoutDataLoss) {
    setSocketBufferSize(4096);

    mTransport = vsock_create_unix_transport_from_fd(kTestStreamKey, mTransportFd);

    // Queue an unaligned write followed by a large 1 MiB write so that
    // socket_send encounters partial writes and EAGAIN/EWOULDBLOCK mid-drain
    // even if the OS clamps minimum socket buffer sizes.
    const std::vector<uint8_t> part1 = makePattern(1234, 11);
    const std::vector<uint8_t> part2 = makePattern(1024 * 1024, 99);
    std::vector<uint8_t> expected = part1;
    expected.insert(expected.end(), part2.begin(), part2.end());

    std::shared_ptr<IVsockNewTransport> nextStage;
    ASSERT_EQ(static_cast<int>(part1.size()),
              mTransport->writeGuestToHost(part1.data(), part1.size(), &nextStage));
    ASSERT_EQ(static_cast<int>(part2.size()),
              mTransport->writeGuestToHost(part2.data(), part2.size(), &nextStage));

    // First looper iteration writes until the socket buffer fills up.
    runOneLooperIteration();
    std::vector<uint8_t> received = drainPeerSocket();
    EXPECT_GT(received.size(), 0u);
    EXPECT_LT(received.size(), expected.size());

    // Continue draining from peer and running looper iterations until complete.
    int iterations = 0;
    while (received.size() < expected.size() && iterations < 500) {
        runOneLooperIteration();
        const std::vector<uint8_t> chunk = drainPeerSocket();
        received.insert(received.end(), chunk.begin(), chunk.end());
        ++iterations;
    }

    EXPECT_EQ(expected.size(), received.size());
    EXPECT_EQ(expected, received);
}

TEST_F(VirtioVsockTransportTest, PeerCloseMarksTransportClosedAndRejectsFurtherWrites) {
    mTransport = vsock_create_unix_transport_from_fd(kTestStreamKey, mTransportFd);

    // Close the peer socket to trigger EOF (socket_recv == 0) on LOOP_IO_READ.
    mPeerSocket.reset(-1);
    runOneLooperIteration();

    const uint8_t byte = 0xAA;
    std::shared_ptr<IVsockNewTransport> nextStage;
    EXPECT_EQ(-1, mTransport->writeGuestToHost(&byte, sizeof(byte), &nextStage));
}

}  // namespace
}  // namespace emulation
}  // namespace android

#endif  // !_WIN32
