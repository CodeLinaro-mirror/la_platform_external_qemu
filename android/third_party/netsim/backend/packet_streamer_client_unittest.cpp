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

#include "backend/packet_streamer_client.h"

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "aemu/base/sockets/ScopedSocket.h"
#include "aemu/base/sockets/SocketUtils.h"
#include "grpcpp/grpcpp.h"
#include "gtest/gtest.h"
#include "netsim/packet_streamer.grpc.pb.h"

namespace netsim::packet {
namespace {

using android::base::ScopedSocket;
using android::base::socketGetPort;
using android::base::socketTcp4LoopbackServer;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

// The test servers listen on IPv4 loopback only, so address them by IP
// literal: "localhost" resolves to ::1 first on Windows, and the resulting
// fallback exhausts the short per-attempt deadlines used below.
std::string LoopbackEndpoint(int port) {
    return "127.0.0.1:" + std::to_string(port);
}

// Binds a loopback TCP listener that never completes an HTTP/2 handshake,
// simulating a wedged netsimd process that still holds its port open.
class WedgedTcpListener {
public:
    WedgedTcpListener() : mSocket(socketTcp4LoopbackServer(0)) {}

    int port() const { return socketGetPort(mSocket.get()); }
    std::string endpoint() const { return LoopbackEndpoint(port()); }

private:
    ScopedSocket mSocket;
};

class FakePacketStreamerService final : public PacketStreamer::CallbackService {
public:
    grpc::ServerBidiReactor<PacketRequest, PacketResponse>* StreamPackets(
            grpc::CallbackServerContext* /*context*/) override {
        class Reactor
            : public grpc::ServerBidiReactor<PacketRequest, PacketResponse> {
        public:
            Reactor() { Finish(grpc::Status::OK); }
            void OnDone() override { delete this; }
        };
        return new Reactor();
    }
};

// Reserves an ephemeral loopback port and releases it again so a server can
// be started on it later. Returns -1 if no port could be reserved.
int ReserveLoopbackPort() {
    ScopedSocket reserved(socketTcp4LoopbackServer(0));
    return reserved.valid() ? socketGetPort(reserved.get()) : -1;
}

// Starts |service| on |port| from a background thread after |delay|, so the
// first connection attempt fails and a later retry succeeds.
class DelayedServer {
public:
    DelayedServer(FakePacketStreamerService* service,
                  int port,
                  milliseconds delay)
        : mThread([this, service, port, delay] {
              std::this_thread::sleep_for(delay);
              grpc::ServerBuilder builder;
              builder.AddListeningPort("127.0.0.1:" + std::to_string(port),
                                       grpc::InsecureServerCredentials());
              builder.RegisterService(service);
              mServer = builder.BuildAndStart();
          }) {}

    ~DelayedServer() {
        mThread.join();
        if (mServer) {
            mServer->Shutdown();
        }
    }

private:
    std::unique_ptr<grpc::Server> mServer;
    std::thread mThread;
};

class PacketStreamerClientTest : public ::testing::Test {
protected:
    // Success-path tests get a generous per-attempt deadline so they do not
    // depend on connect latency; the wedged-endpoint tests shorten it.
    void SetUp() override {
        SetConnectionBudgetForTesting(std::chrono::seconds(1),
                                      milliseconds(10));
    }

    void TearDown() override {
        SetPacketStreamEndpoint("default");
        SetConnectionBudgetForTesting(std::chrono::seconds(1),
                                      std::chrono::seconds(1));
    }
};

// The timing-sensitive wedged-endpoint scenarios are only exercised on POSIX
// hosts, where their latency bounds have been validated.
#ifndef _WIN32
TEST_F(PacketStreamerClientTest, ReturnsNullptrWithinBudgetWhenEndpointWedged) {
    WedgedTcpListener wedged;
    ASSERT_GT(wedged.port(), 0);
    SetPacketStreamEndpoint(wedged.endpoint());
    SetConnectionBudgetForTesting(milliseconds(40), milliseconds(10));

    auto start = steady_clock::now();
    auto channel = CreateChannel(NetsimdOptions{});
    auto elapsed = std::chrono::duration_cast<milliseconds>(
            steady_clock::now() - start);

    EXPECT_EQ(channel, nullptr);
    // 4 attempts * 40ms + (10ms + 20ms + 40ms) backoff = ~230ms minimum.
    EXPECT_GE(elapsed.count(), 180);
    EXPECT_LT(elapsed.count(), 1500);
}

TEST_F(PacketStreamerClientTest,
       ConcurrentCallersShareSingleInFlightConnectionAttempt) {
    WedgedTcpListener wedged;
    ASSERT_GT(wedged.port(), 0);
    SetPacketStreamEndpoint(wedged.endpoint());
    SetConnectionBudgetForTesting(milliseconds(50), milliseconds(15));

    constexpr int kNumCallers = 4;
    std::promise<void> go;
    std::shared_future<void> ready = go.get_future().share();
    std::vector<std::future<std::shared_ptr<grpc::Channel>>> futures;
    futures.reserve(kNumCallers);

    for (int i = 0; i < kNumCallers; ++i) {
        futures.push_back(std::async(std::launch::async, [ready]() {
            ready.wait();
            return CreateChannel(NetsimdOptions{});
        }));
    }

    auto start = steady_clock::now();
    go.set_value();

    for (auto& fut : futures) {
        EXPECT_EQ(fut.get(), nullptr);
    }
    auto elapsed = std::chrono::duration_cast<milliseconds>(
            steady_clock::now() - start);

    // A single shared attempt takes ~4 * 50ms + (15 + 30 + 60)ms = ~305ms.
    // Four serialized attempts would take >1200ms.
    EXPECT_LT(elapsed.count(), 850);
}
#endif

TEST_F(PacketStreamerClientTest, ConnectsAndCachesReadyChannelAcrossCallers) {
    FakePacketStreamerService service;
    int selected_port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &selected_port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    ASSERT_GT(selected_port, 0);

    SetPacketStreamEndpoint(LoopbackEndpoint(selected_port));

    auto ch1 = CreateChannel(NetsimdOptions{});
    ASSERT_NE(ch1, nullptr);
    EXPECT_EQ(ch1->GetState(false), GRPC_CHANNEL_READY);

    auto ch2 = CreateChannel("deprecated_arg");
    EXPECT_EQ(ch1, ch2);

    server->Shutdown();
}

TEST_F(PacketStreamerClientTest, SetPacketStreamEndpointResetsCachedChannel) {
    FakePacketStreamerService service;
    int selected_port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &selected_port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);

    SetPacketStreamEndpoint(LoopbackEndpoint(selected_port));
    auto ch1 = CreateChannel(NetsimdOptions{});
    ASSERT_NE(ch1, nullptr);

#ifndef _WIN32
    WedgedTcpListener wedged;
    ASSERT_GT(wedged.port(), 0);
    SetPacketStreamEndpoint(wedged.endpoint());
    SetConnectionBudgetForTesting(milliseconds(40), milliseconds(10));
    auto ch2 = CreateChannel(NetsimdOptions{});
    EXPECT_EQ(ch2, nullptr);
#endif

    server->Shutdown();
}

#ifndef _WIN32
TEST_F(PacketStreamerClientTest, DelayedServerStartupSucceedsOnRetry) {
    int port = ReserveLoopbackPort();
    ASSERT_GT(port, 0);
    SetPacketStreamEndpoint(LoopbackEndpoint(port));
    SetConnectionBudgetForTesting(milliseconds(50), milliseconds(80));

    FakePacketStreamerService service;
    DelayedServer server(&service, port, milliseconds(60));

    auto channel = CreateChannel(NetsimdOptions{});

    ASSERT_NE(channel, nullptr);
    EXPECT_EQ(channel->GetState(false), GRPC_CHANNEL_READY);
}

TEST_F(PacketStreamerClientTest,
       ConcurrentFollowersReceiveLeaderChannelOnceServerStarts) {
    int port = ReserveLoopbackPort();
    ASSERT_GT(port, 0);
    SetPacketStreamEndpoint(LoopbackEndpoint(port));
    SetConnectionBudgetForTesting(milliseconds(50), milliseconds(80));

    FakePacketStreamerService service;
    DelayedServer server(&service, port, milliseconds(60));

    // The first caller leads the retrying attempt; the others block on
    // channel_cv and must be handed the channel the leader publishes.
    constexpr int kNumCallers = 4;
    std::vector<std::future<std::shared_ptr<grpc::Channel>>> futures;
    futures.reserve(kNumCallers);
    for (int i = 0; i < kNumCallers; ++i) {
        futures.push_back(std::async(std::launch::async, [] {
            return CreateChannel(NetsimdOptions{});
        }));
    }

    std::vector<std::shared_ptr<grpc::Channel>> channels;
    for (auto& fut : futures) {
        channels.push_back(fut.get());
    }

    ASSERT_NE(channels[0], nullptr);
    EXPECT_EQ(channels[0]->GetState(false), GRPC_CHANNEL_READY);
    for (const auto& channel : channels) {
        EXPECT_EQ(channel, channels[0]);
    }
}
#endif

}  // namespace
}  // namespace netsim::packet
