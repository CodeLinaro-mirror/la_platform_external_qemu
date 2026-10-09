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

#include "android-qemu2-glue/netsim/NetsimWifiForwarder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "aemu/base/IOVector.h"
#include "aemu/base/sockets/ScopedSocket.h"
#include "aemu/base/sockets/SocketUtils.h"
#include "android/network/GenericNetlinkMessage.h"
#include "android/network/mac80211_hwsim.h"
#include "backend/packet_streamer_client.h"
#include "grpcpp/grpcpp.h"
#include "gtest/gtest.h"
#include "netsim/common.pb.h"
#include "netsim/packet_streamer.grpc.pb.h"
#include "netsim/packet_streamer.pb.h"

extern "C" {
void qemu_chr_be_write(void* /*s*/, uint8_t* /*buf*/, int /*len*/) {}

void register_module_init(void (* /*fn*/)(void), int /*type*/) {}

void* type_register_static(const void* /*info*/) {
    return nullptr;
}

void* object_dynamic_cast_assert(void* obj,
                                 const char* /*typename_*/,
                                 const char* /*file*/,
                                 int /*line*/,
                                 const char* /*func*/) {
    return obj;
}

void* object_class_dynamic_cast_assert(void* klass,
                                       const char* /*typename_*/,
                                       const char* /*file*/,
                                       int /*line*/,
                                       const char* /*func*/) {
    return klass;
}
}  // extern "C"

namespace android {
namespace qemu2 {
namespace {

using android::base::IOVector;
using android::network::GenericNetlinkMessage;
using netsim::packet::PacketRequest;
using netsim::packet::PacketResponse;
using netsim::packet::PacketStreamer;
using std::chrono::milliseconds;

// The test servers listen on IPv4 loopback only, so address them by IP
// literal: "localhost" resolves to ::1 first on Windows, and the resulting
// fallback exhausts the short per-attempt deadlines used below.
std::string LoopbackEndpoint(int port) {
    return "127.0.0.1:" + std::to_string(port);
}

std::unique_ptr<GenericNetlinkMessage> MakeHwsimMessage(
        uint8_t cmd,
        const std::string& payload) {
    auto msg = std::make_unique<GenericNetlinkMessage>(
            GenericNetlinkMessage::NL_AUTO_PORT,
            GenericNetlinkMessage::NL_AUTO_SEQ,
            GenericNetlinkMessage::NLMSG_MIN_TYPE, 0, 0, cmd, 1);
    if (!payload.empty()) {
        msg->putAttribute(HWSIM_ATTR_FRAME, payload.data(), payload.size());
    }
    return msg;
}

class ControllablePacketStreamerService final
    : public PacketStreamer::CallbackService {
public:
    class ServerStream
        : public grpc::ServerBidiReactor<PacketRequest, PacketResponse> {
    public:
        explicit ServerStream(ControllablePacketStreamerService* parent)
            : mParent(parent) {
            StartRead(&mIncoming);
        }

        void OnReadDone(bool ok) override {
            if (!ok) {
                CloseStream(grpc::Status::OK);
                return;
            }
            mParent->OnRequestReceived(mIncoming);
            StartRead(&mIncoming);
        }

        void OnCancel() override { CloseStream(grpc::Status::CANCELLED); }

        void OnWriteDone(bool /*ok*/) override {
            std::lock_guard<std::mutex> lock(mWriteMutex);
            mWriting = false;
        }

        void OnDone() override {
            mParent->ClearActiveStream(this);
            delete this;
        }

        void SendPacket(const std::string& bytes) {
            std::lock_guard<std::mutex> lock(mWriteMutex);
            if (mFinished || mWriting)
                return;
            mOutgoing.Clear();
            mOutgoing.set_packet(bytes);
            mWriting = true;
            StartWrite(&mOutgoing);
        }

        void CloseStream(grpc::Status status) {
            std::lock_guard<std::mutex> lock(mWriteMutex);
            if (!mFinished) {
                mFinished = true;
                Finish(status);
            }
        }

    private:
        ControllablePacketStreamerService* mParent;
        PacketRequest mIncoming;
        PacketResponse mOutgoing;
        std::mutex mWriteMutex;
        bool mWriting = false;
        bool mFinished = false;
    };

    grpc::ServerBidiReactor<PacketRequest, PacketResponse>* StreamPackets(
            grpc::CallbackServerContext* /*context*/) override {
        auto* stream = new ServerStream(this);
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mActiveStream = stream;
        }
        mCv.notify_all();
        return stream;
    }

    void OnRequestReceived(const PacketRequest& req) {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mReceivedRequests.push_back(req);
        }
        mCv.notify_all();
    }

    void ClearActiveStream(ServerStream* stream) {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mActiveStream == stream) {
                mActiveStream = nullptr;
            }
        }
        mCv.notify_all();
    }

    bool WaitForRequests(size_t count, milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mMutex);
        return mCv.wait_for(lock, timeout, [this, count] {
            return mReceivedRequests.size() >= count;
        });
    }

    std::vector<PacketRequest> ReceivedRequests() {
        std::lock_guard<std::mutex> lock(mMutex);
        return mReceivedRequests;
    }

    void SendResponseToClient(const std::string& bytes) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mActiveStream) {
            mActiveStream->SendPacket(bytes);
        }
    }

    void FinishActiveStream(grpc::Status status) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mActiveStream) {
            mActiveStream->CloseStream(status);
        }
    }

private:
    std::mutex mMutex;
    std::condition_variable mCv;
    ServerStream* mActiveStream = nullptr;
    std::vector<PacketRequest> mReceivedRequests;
};

class NetsimWifiForwarderTest : public ::testing::Test {
protected:
    // Success-path tests get a generous per-attempt deadline so they do not
    // depend on connect latency; the wedged-endpoint test shortens it.
    void SetUp() override {
        netsim::packet::SetConnectionBudgetForTesting(std::chrono::seconds(1),
                                                      milliseconds(10));
    }

    void TearDown() override {
        netsim::packet::SetPacketStreamEndpoint("default");
        netsim::packet::SetConnectionBudgetForTesting(std::chrono::seconds(1),
                                                      std::chrono::seconds(1));
    }
};

#ifndef _WIN32
TEST_F(NetsimWifiForwarderTest,
       InitFailsCleanlyWithinBudgetWhenEndpointUnreachable) {
    // A loopback listener that never completes the HTTP/2 handshake stands in
    // for a wedged netsimd.
    android::base::ScopedSocket wedged(
            android::base::socketTcp4LoopbackServer(0));
    ASSERT_TRUE(wedged.valid());
    int port = android::base::socketGetPort(wedged.get());
    ASSERT_GT(port, 0);

    netsim::packet::SetPacketStreamEndpoint(LoopbackEndpoint(port));
    netsim::packet::SetConnectionBudgetForTesting(milliseconds(40),
                                                  milliseconds(10));

    NetsimWifiForwarder forwarder([](const uint8_t*, size_t) { return 0; },
                                  [](size_t) { return true; });
    EXPECT_FALSE(forwarder.init());
    EXPECT_EQ(forwarder.getNic(), nullptr);

    auto frame = MakeHwsimMessage(HWSIM_CMD_FRAME, "test-frame");
    struct iovec iov_entry = {frame->data(), frame->dataLen()};
    IOVector iov(&iov_entry, &iov_entry + 1);
    EXPECT_EQ(forwarder.send(iov), 0);
    EXPECT_EQ(forwarder.hostapd_send(iov), 0);
    EXPECT_EQ(forwarder.libslirp_send(iov), 0);
    EXPECT_EQ(forwarder.recv(iov), 0);
    forwarder.stop();
}
#endif

TEST_F(NetsimWifiForwarderTest,
       ForwardsHwsimFramesAndReceivesResponsesUntilStopped) {
    ControllablePacketStreamerService service;
    int selected_port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &selected_port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    netsim::packet::SetPacketStreamEndpoint(LoopbackEndpoint(selected_port));

    std::mutex rx_mutex;
    std::condition_variable rx_cv;
    std::vector<std::string> received_frames;
    std::atomic<bool> allow_receive{true};

    NetsimWifiForwarder forwarder(
            [&](const uint8_t* data, size_t size) -> size_t {
                {
                    std::lock_guard<std::mutex> lock(rx_mutex);
                    received_frames.emplace_back(
                            reinterpret_cast<const char*>(data), size);
                }
                rx_cv.notify_all();
                return size;
            },
            [&](size_t /*queueIdx*/) -> bool { return allow_receive.load(); });

    ASSERT_TRUE(forwarder.init());
    ASSERT_TRUE(service.WaitForRequests(1, milliseconds(1000)));
    auto reqs = service.ReceivedRequests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_TRUE(reqs[0].has_initial_info());
    EXPECT_EQ(reqs[0].initial_info().chip().kind(),
              netsim::common::ChipKind::WIFI);

    // Non-HWSIM_CMD_FRAME messages must be filtered out and return 0.
    auto register_msg = MakeHwsimMessage(HWSIM_CMD_REGISTER, "");
    struct iovec reg_iov_entry = {register_msg->data(),
                                  register_msg->dataLen()};
    IOVector reg_iov(&reg_iov_entry, &reg_iov_entry + 1);
    EXPECT_EQ(forwarder.send(reg_iov), 0);

    // Valid HWSIM_CMD_FRAME via send(), hostapd_send(), and libslirp_send().
    auto frame_msg = MakeHwsimMessage(HWSIM_CMD_FRAME, "wifi-payload-1");
    struct iovec frame_iov_entry = {frame_msg->data(), frame_msg->dataLen()};
    IOVector frame_iov(&frame_iov_entry, &frame_iov_entry + 1);
    EXPECT_EQ(forwarder.send(frame_iov),
              static_cast<int>(frame_msg->dataLen()));
    EXPECT_EQ(forwarder.hostapd_send(frame_iov),
              static_cast<int>(frame_msg->dataLen()));
    EXPECT_EQ(forwarder.libslirp_send(frame_iov),
              static_cast<int>(frame_msg->dataLen()));

    ASSERT_TRUE(service.WaitForRequests(4, milliseconds(1000)));
    reqs = service.ReceivedRequests();
    ASSERT_EQ(reqs.size(), 4u);
    EXPECT_EQ(reqs[1].packet(),
              std::string(reinterpret_cast<const char*>(frame_msg->data()),
                          frame_msg->dataLen()));

    // Server -> Client frame when canReceive is true.
    service.SendResponseToClient("downlink-frame-1");
    {
        std::unique_lock<std::mutex> lock(rx_mutex);
        ASSERT_TRUE(rx_cv.wait_for(lock, milliseconds(1000),
                                   [&] { return !received_frames.empty(); }));
        EXPECT_EQ(received_frames[0], "downlink-frame-1");
    }

    forwarder.stop();
    server->Shutdown();
}

TEST_F(NetsimWifiForwarderTest,
       SuppressesWritesAfterServerStreamFinishesMidSession) {
    ControllablePacketStreamerService service;
    int selected_port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &selected_port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    netsim::packet::SetPacketStreamEndpoint(LoopbackEndpoint(selected_port));

    NetsimWifiForwarder forwarder([](const uint8_t*, size_t) { return 0; },
                                  [](size_t) { return true; });
    ASSERT_TRUE(forwarder.init());
    ASSERT_TRUE(service.WaitForRequests(1, milliseconds(1000)));

    // Simulate netsimd crashing / finishing the stream mid-session.
    service.FinishActiveStream(grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                            "netsimd vanished mid-session"));

    auto frame_msg = MakeHwsimMessage(HWSIM_CMD_FRAME, "post-crash-frame");
    struct iovec frame_iov_entry = {frame_msg->data(), frame_msg->dataLen()};
    IOVector frame_iov(&frame_iov_entry, &frame_iov_entry + 1);

    // Poll until OnDone marks the transport done, verifying no write ever
    // crashes with std::bad_function_call or reactor UAF.
    bool saw_zero_return = false;
    for (int i = 0; i < 50; ++i) {
        if (forwarder.send(frame_iov) == 0) {
            saw_zero_return = true;
            break;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    EXPECT_TRUE(saw_zero_return);
    EXPECT_EQ(forwarder.send(frame_iov), 0);

    forwarder.stop();
    server->Shutdown();
}

}  // namespace
}  // namespace qemu2
}  // namespace android
