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

#include "android/grpc/utils/SimpleAsyncGrpc.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "gtest/gtest.h"

namespace {

struct TrackedMessage {
    int id = 0;
    std::shared_ptr<int> alive_token;
};

template <typename W, typename R = int>
class FakeBidiReactor {
public:
    virtual ~FakeBidiReactor() = default;
    virtual void OnWriteDone(bool /*ok*/) {}
    virtual void OnReadDone(bool /*ok*/) {}

    void StartWrite(const W* msg) {
        in_flight_ptr = msg;
        written_ids.push_back(msg->id);
    }

    void StartRead(R* msg) {
        last_read_ptr = msg;
        ++start_read_calls;
    }

    const W* in_flight_ptr = nullptr;
    std::vector<int> written_ids;
    R* last_read_ptr = nullptr;
    int start_read_calls = 0;
};

template <typename R>
class FakeServerReadReactor : public grpc::internal::ServerReactor {
public:
    virtual ~FakeServerReadReactor() = default;
    virtual void OnReadDone(bool /*ok*/) {}
    void OnDone() override {}
    void OnCancel() override {}

    void StartRead(R* msg) {
        last_read_ptr = msg;
        ++start_read_calls;
    }

    void Finish(grpc::Status s) {
        finished = true;
        finish_status = s;
    }

    R* last_read_ptr = nullptr;
    int start_read_calls = 0;
    bool finished = false;
    grpc::Status finish_status{grpc::StatusCode::CANCELLED, ""};
};

using TestQueueWriter = WithSimpleQueueWriter<FakeBidiReactor<TrackedMessage>>;

class TestClientReader
    : public WithSimpleReader<FakeBidiReactor<TrackedMessage, int>> {
public:
    void Read(const int* read) override { received.push_back(*read); }
    std::vector<int> received;
};

class TestServerReader : public WithSimpleReader<FakeServerReadReactor<int>> {
public:
    void Read(const int* read) override { received.push_back(*read); }
    std::vector<int> received;
};

TEST(SimpleAsyncGrpcTest, SelectTypeHelperExtractsExpectedTemplateParameter) {
    static_assert(
            std::is_same_v<Select_t<0, FakeBidiReactor<TrackedMessage, int>>,
                           TrackedMessage>);
    static_assert(
            std::is_same_v<Select_t<1, FakeBidiReactor<TrackedMessage, int>>,
                           int>);
}

TEST(SimpleAsyncGrpcTest, SequentialAndQueuedWritesDrainInOrder) {
    TestQueueWriter writer;
    writer.Write(TrackedMessage{1, std::make_shared<int>(1)});
    writer.Write(TrackedMessage{2, std::make_shared<int>(2)});
    writer.Write(TrackedMessage{3, std::make_shared<int>(3)});

    // Only the first write is started immediately; 2 and 3 wait in queue.
    ASSERT_EQ(writer.written_ids, std::vector<int>({1}));

    writer.OnWriteDone(true);
    ASSERT_EQ(writer.written_ids, std::vector<int>({1, 2}));

    writer.OnWriteDone(true);
    ASSERT_EQ(writer.written_ids, std::vector<int>({1, 2, 3}));

    writer.OnWriteDone(true);
    EXPECT_EQ(writer.written_ids, std::vector<int>({1, 2, 3}));
}

TEST(SimpleAsyncGrpcTest,
     FailedWriteDoneClosesWriterAndSuppressesFurtherWrites) {
    TestQueueWriter writer;
    auto token1 = std::make_shared<int>(1);
    auto token2 = std::make_shared<int>(2);

    writer.Write(TrackedMessage{10, token1});
    writer.Write(TrackedMessage{20, token2});
    ASSERT_EQ(writer.written_ids, std::vector<int>({10}));

    // Simulate stream write failure (ok == false).
    writer.OnWriteDone(false);

    // Queue should be cleared and no second StartWrite issued.
    EXPECT_EQ(writer.written_ids, std::vector<int>({10}));
    EXPECT_EQ(token1.use_count(), 1);
    EXPECT_EQ(token2.use_count(), 1);

    // Subsequent Write() calls after failure must be dropped without
    // StartWrite.
    writer.Write(TrackedMessage{30, std::make_shared<int>(3)});
    EXPECT_EQ(writer.written_ids, std::vector<int>({10}));
}

TEST(SimpleAsyncGrpcTest, CloseWriterWhenIdleClearsQueueAndDropsNewWrites) {
    TestQueueWriter writer;
    writer.CloseWriter();

    auto token = std::make_shared<int>(42);
    writer.Write(TrackedMessage{42, token});
    EXPECT_TRUE(writer.written_ids.empty());
    EXPECT_EQ(token.use_count(), 1);
}

TEST(SimpleAsyncGrpcTest,
     CloseWriterDuringInFlightWritePreservesFrontUntilOnWriteDone) {
    TestQueueWriter writer;
    auto token1 = std::make_shared<int>(1);
    auto token2 = std::make_shared<int>(2);

    writer.Write(TrackedMessage{1, token1});
    writer.Write(TrackedMessage{2, token2});
    ASSERT_EQ(writer.written_ids, std::vector<int>({1}));
    ASSERT_NE(writer.in_flight_ptr, nullptr);

    // Both messages are in mWriteQueue while message 1 is in flight in gRPC.
    EXPECT_EQ(token1.use_count(), 2);
    EXPECT_EQ(token2.use_count(), 2);

    // Calling CloseWriter() while mWriting == true must NOT destroy
    // mWriteQueue.front() (token1), because gRPC still holds in_flight_ptr.
    writer.CloseWriter();
    EXPECT_EQ(token1.use_count(), 2);
    EXPECT_EQ(writer.in_flight_ptr->id, 1);

    // Writes arriving after CloseWriter() while mWriting == true are rejected.
    auto token3 = std::make_shared<int>(3);
    writer.Write(TrackedMessage{3, token3});
    EXPECT_EQ(token3.use_count(), 1);

    // Once gRPC finishes the in-flight write and invokes OnWriteDone(true),
    // the queue is cleared without starting message 2.
    writer.OnWriteDone(true);
    EXPECT_EQ(writer.written_ids, std::vector<int>({1}));
    EXPECT_EQ(token1.use_count(), 1);
    EXPECT_EQ(token2.use_count(), 1);
}

TEST(SimpleAsyncGrpcTest, ClientAndServerReadersHandleReadLifecycles) {
    TestClientReader client_reader;
    // Client reader does not auto-start in constructor.
    EXPECT_EQ(client_reader.start_read_calls, 0);

    client_reader.StartRead();
    ASSERT_EQ(client_reader.start_read_calls, 1);
    ASSERT_NE(client_reader.last_read_ptr, nullptr);

    *client_reader.last_read_ptr = 77;
    client_reader.OnReadDone(true);
    EXPECT_EQ(client_reader.received, std::vector<int>({77}));
    EXPECT_EQ(client_reader.start_read_calls, 2);

    client_reader.OnReadDone(false);
    EXPECT_EQ(client_reader.start_read_calls, 2);

    // Server reader auto-starts in constructor and calls Finish(OK) on EOF.
    TestServerReader server_reader;
    EXPECT_EQ(server_reader.start_read_calls, 1);
    ASSERT_NE(server_reader.last_read_ptr, nullptr);

    *server_reader.last_read_ptr = 99;
    server_reader.OnReadDone(true);
    EXPECT_EQ(server_reader.received, std::vector<int>({99}));
    EXPECT_EQ(server_reader.start_read_calls, 2);
    EXPECT_FALSE(server_reader.finished);

    server_reader.OnReadDone(false);
    EXPECT_TRUE(server_reader.finished);
    EXPECT_TRUE(server_reader.finish_status.ok());
}

template <typename W>
class ThreadSafeFakeReactor {
public:
    virtual ~ThreadSafeFakeReactor() = default;
    virtual void OnWriteDone(bool /*ok*/) {}
    void StartWrite(const W* msg) {
        std::lock_guard<std::mutex> lock(mu);
        ASSERT_NE(msg, nullptr);
        ASSERT_NE(msg->alive_token, nullptr);
        EXPECT_GE(*msg->alive_token, 0);
        in_flight = msg;
    }
    const W* TakeInFlight() {
        std::lock_guard<std::mutex> lock(mu);
        const W* ptr = in_flight;
        in_flight = nullptr;
        return ptr;
    }
    std::mutex mu;
    const W* in_flight = nullptr;
};

TEST(SimpleAsyncGrpcTest, CloseWriterConcurrentWithWritesAndOnWriteDone) {
    // Exercise concurrent Write(), OnWriteDone(), and CloseWriter() across
    // multiple threads to verify lock discipline and absence of data races or
    // in-flight element use-after-free under TSan/ASan.
    using ConcurrentWriter =
            WithSimpleQueueWriter<ThreadSafeFakeReactor<TrackedMessage>>;
    for (int iter = 0; iter < 50; ++iter) {
        ConcurrentWriter writer;
        std::atomic<bool> stop{false};

        std::thread producer([&]() {
            for (int i = 0; i < 200; ++i) {
                writer.Write(TrackedMessage{i, std::make_shared<int>(i)});
            }
        });

        std::thread completion_pump([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                if (const TrackedMessage* msg = writer.TakeInFlight()) {
                    // Verify the in-flight message in mWriteQueue.front() is
                    // still alive right before OnWriteDone pops it.
                    EXPECT_GE(*msg->alive_token, 0);
                    writer.OnWriteDone(true);
                }
            }
            if (const TrackedMessage* msg = writer.TakeInFlight()) {
                EXPECT_GE(*msg->alive_token, 0);
                writer.OnWriteDone(false);
            }
        });

        std::thread closer([&]() {
            for (int i = 0; i < 20; ++i) {
                std::this_thread::yield();
            }
            writer.CloseWriter();
        });

        producer.join();
        closer.join();
        stop.store(true, std::memory_order_release);
        completion_pump.join();
    }
}

}  // namespace
