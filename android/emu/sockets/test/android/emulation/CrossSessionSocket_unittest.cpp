// Copyright 2018 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/emulation/CrossSessionSocket.h"

#include "aemu/base/files/InplaceStream.h"
#include "aemu/base/sockets/SocketUtils.h"

#include <cstring>
#include <gtest/gtest.h>

namespace android {
namespace emulation {

TEST(CrossSessionSocket, ReclaimWithoutRegister) {
    CrossSessionSocket::clearRecycleSockets();
    const int kUnknownFd = 1;
    EXPECT_FALSE(CrossSessionSocket::reclaimSocket(kUnknownFd).valid());
}

TEST(CrossSessionSocket, Recycle) {
    CrossSessionSocket::clearRecycleSockets();
    int fd = base::socketCreateTcp4();
    CrossSessionSocket socket(fd);
    EXPECT_TRUE(socket.valid());
    CrossSessionSocket::registerForRecycle(socket.fd());
    CrossSessionSocket::recycleSocket(std::move(socket));
    EXPECT_FALSE(socket.valid());
    CrossSessionSocket recycled = CrossSessionSocket::reclaimSocket(fd);
    EXPECT_TRUE(recycled.valid());
    EXPECT_EQ(recycled.fd(), fd);
    CrossSessionSocket::clearRecycleSockets();
}

TEST(CrossSessionSocket, ReclaimTwice) {
    CrossSessionSocket::clearRecycleSockets();
    int fd = base::socketCreateTcp4();
    CrossSessionSocket socket(fd);
    EXPECT_TRUE(socket.valid());
    CrossSessionSocket::registerForRecycle(socket.fd());
    CrossSessionSocket::recycleSocket(std::move(socket));
    CrossSessionSocket::reclaimSocket(fd);
    CrossSessionSocket recycled = CrossSessionSocket::reclaimSocket(fd);
    EXPECT_FALSE(recycled.valid());
    CrossSessionSocket::clearRecycleSockets();
}

TEST(CrossSessionSocket, RecycleWithoutRegister) {
    CrossSessionSocket::clearRecycleSockets();
    int fd = base::socketCreateTcp4();
    CrossSessionSocket socket(fd);
    EXPECT_TRUE(socket.valid());
    CrossSessionSocket::recycleSocket(std::move(socket));
    EXPECT_TRUE(socket.valid());
    CrossSessionSocket recycled = CrossSessionSocket::reclaimSocket(fd);
    EXPECT_FALSE(recycled.valid());
    CrossSessionSocket::clearRecycleSockets();
}

// BUG: 173703455
TEST(CrossSessionSocket, DISABLED_DrainStale) {
    const char* kContents = "Stale text";
    const int kContentSize = strlen(kContents) + 1;
    // Make buffer slightly larger so that it can catch if there are
    // unexpected extra bits written to the buffer.
    const int kBufferSize = kContentSize + 10;
    // Number of bytes going to be written into the snapshot
    const int kSnapshotSize = kBufferSize + 4;
    CrossSessionSocket::clearRecycleSockets();

    int fd1, fd2;
    int ret = base::socketCreatePair(&fd1, &fd2);
    EXPECT_EQ(0, ret);
    CrossSessionSocket socket(fd1);
    CrossSessionSocket::registerForRecycle(socket.fd());

    ssize_t sendSize = base::socketSend(fd2, kContents, kContentSize);
    EXPECT_EQ(kContentSize, sendSize);
    socket.drainSocket(CrossSessionSocket::DrainBehavior::AppendToBuffer);
    char snapshotBuffer[kSnapshotSize];
    base::InplaceStream snapshotStream(snapshotBuffer, kSnapshotSize);
    socket.onSave(&snapshotStream);

    CrossSessionSocket::recycleSocket(std::move(socket));
    CrossSessionSocket socket2 = CrossSessionSocket::reclaimSocket(fd1);
    socket2.onLoad(&snapshotStream);

    char readbackBuffer[kBufferSize];
    ssize_t readSize = socket2.readStaleData(readbackBuffer, kBufferSize);
    EXPECT_EQ(kContentSize, readSize);
    EXPECT_EQ(0, memcmp(kContents, readbackBuffer, kContentSize));

    // fd1 will be closed automatically
    base::socketClose(fd2);
    CrossSessionSocket::clearRecycleSockets();
}

} // namespace emulation
} // namespace android

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif
#include "android/sdk-controller-socket.h"
#include "android/utils/looper.h"
#include "android/utils/sockets.h"

namespace {

struct SdkCtlTestState {
    bool connected = false;
    bool failedAfterHeader = false;
};

static AsyncIOAction onTestSocketConnection(void* opaque,
                                            SDKCtlSocket* sdkctl,
                                            AsyncIOState status) {
    auto* state = static_cast<SdkCtlTestState*>(opaque);
    if (status == ASIO_STATE_SUCCEEDED) {
        state->connected = true;
        return ASIO_ACTION_DONE;
    }
    if (status == ASIO_STATE_FAILED && state->connected) {
        state->failedAfterHeader = true;
        looper_forceQuit(looper_getForThread());
        return ASIO_ACTION_ABORT;
    }
    return ASIO_ACTION_DONE;
}

static void onTestPortConnection(void* opaque,
                                 SDKCtlSocket* sdkctl,
                                 SdkCtlPortStatus status) {
    auto* state = static_cast<SdkCtlTestState*>(opaque);
    if (status == SDKCTL_HANDSHAKE_UNKNOWN_RESPONSE) {
        state->failedAfterHeader = true;
        looper_forceQuit(looper_getForThread());
    }
}

static void onTestMessage(void* opaque,
                          SDKCtlSocket* sdkctl,
                          SDKCtlMessage* message,
                          int msg_type,
                          void* msg_data,
                          int msg_size) {}

static void expectHeaderRejected(const void* payload, size_t payloadSize) {
    Looper* looper = looper_getForThread();
    ASSERT_NE(nullptr, looper);

    int serverFd = socket_loopback4_server(0, SOCKET_STREAM);
    ASSERT_GE(serverFd, 0);
    SockAddress addr;
    ASSERT_EQ(0, socket_get_address(serverFd, &addr));
    int port = sock_address_get_port(&addr);
    ASSERT_GT(port, 0);

    SdkCtlTestState state;
    SDKCtlSocket* sdkctl =
            sdkctl_socket_new(1000, "test-service", onTestSocketConnection,
                              onTestPortConnection, onTestMessage, &state);
    ASSERT_NE(nullptr, sdkctl);

    sdkctl_socket_connect(sdkctl, port, 1000);

    int peerFd = -1;
    for (int i = 0; i < 50 && peerFd < 0; ++i) {
        looper_runWithDeadline(looper, looper_now(looper) + 10);
        peerFd = socket_accept_any(serverFd);
    }
    ASSERT_GE(peerFd, 0);

    for (int i = 0; i < 50 && !state.connected; ++i) {
        looper_runWithDeadline(looper, looper_now(looper) + 10);
    }
    EXPECT_TRUE(state.connected);

    EXPECT_EQ(static_cast<int>(payloadSize),
              socket_send(peerFd, payload, static_cast<int>(payloadSize)));

    looper_runWithDeadline(looper, looper_now(looper) + 500);
    EXPECT_TRUE(state.failedAfterHeader);

    sdkctl_socket_disconnect(sdkctl);
    sdkctl_socket_release(sdkctl);
    socket_close(peerFd);
    socket_close(serverFd);
}

TEST(SdkControllerSocket, RejectsUnderflowPacketSize) {
    // Packet size = 0 (< sizeof(SDKCtlPacketHeader) == 12)
    const int32_t headerSize0[3] = {0x53444B43, 0, 1};
    expectHeaderRejected(headerSize0, sizeof(headerSize0));

    // Message packet (type = 1) with size = 12 (< sizeof(SDKCtlMessageHeader) == 16)
    const int32_t msgHeaderSize12[3] = {0x53444B43, 12, 1};
    expectHeaderRejected(msgHeaderSize12, sizeof(msgHeaderSize12));

    // Query reply packet (type = 3) with size = 12 (< sizeof(SDKCtlQueryReplyHeader) == 16)
    const int32_t replyHeaderSize12[3] = {0x53444B43, 12, 3};
    expectHeaderRejected(replyHeaderSize12, sizeof(replyHeaderSize12));

    // Unknown packet type (type = 99) with size = 12
    const int32_t unknownTypeSize12[3] = {0x53444B43, 12, 99};
    expectHeaderRejected(unknownTypeSize12, sizeof(unknownTypeSize12));

    // Handshake query reply (type = 3, query_id = 1) with size = 16
    // (0 bytes of response payload)
    const int32_t emptyHandshakeReply[4] = {0x53444B43, 16, 3, 1};
    expectHeaderRejected(emptyHandshakeReply, sizeof(emptyHandshakeReply));

    // Handshake query reply (type = 3, query_id = 1) with size = 17
    // (1 byte of response payload < sizeof(int))
    const int32_t truncatedHandshakeReply[5] = {0x53444B43, 17, 3, 1, 0};
    expectHeaderRejected(truncatedHandshakeReply, 17);
}

}  // namespace
