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
#include "android/emulation/control/keyboard/EventSender.h"

#include <gtest/gtest.h>

#include <memory>

#include "android/base/testing/TestLooper.h"
#include "android/utils/looper.h"

namespace android {
namespace emulation {
namespace control {
namespace {

class TestEventSender : public EventSender<int> {
public:
    TestEventSender(int* outValue, bool* outDestroyed)
        : EventSender<int>(nullptr),
          mOutValue(outValue),
          mOutDestroyed(outDestroyed) {}

    ~TestEventSender() override {
        mDerivedSentinel = 0;
        *mOutDestroyed = true;
    }

protected:
    void doSend(const int event) override {
        // Fails if doSend() runs on a destroyed (or partially destroyed)
        // derived instance.
        EXPECT_EQ(mDerivedSentinel, 0x12345678);
        *mOutValue = event;
    }

private:
    int mDerivedSentinel = 0x12345678;
    int* mOutValue;
    bool* mOutDestroyed;
};

class EventSenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        android::base::ThreadLooper::clearMainRunner();
        android_registerMainLooper(reinterpret_cast<::Looper*>(
                static_cast<android::base::Looper*>(&mLooper)));
    }

    void TearDown() override {
        android::base::ThreadLooper::clearMainRunner();
        android_registerMainLooper(nullptr);
    }

    void pumpLooper() {
        mLooper.runOneIterationWithDeadlineMs(mLooper.nowMs() + 10);
    }

    android::base::TestLooper mLooper;
};

}  // namespace

TEST_F(EventSenderTest, DispatchesOnMainLooper) {
    int received = 0;
    bool destroyed = false;
    auto sender = std::make_shared<TestEventSender>(&received, &destroyed);

    sender->send(7);
    EXPECT_EQ(received, 0);

    pumpLooper();
    EXPECT_EQ(received, 7);
    EXPECT_FALSE(destroyed);
}

TEST_F(EventSenderTest, SharedPtrExtendsLifetimeUntilMainLooperRuns) {
    int received = 0;
    bool destroyed = false;
    {
        auto sender = std::make_shared<TestEventSender>(&received, &destroyed);
        sender->send(42);
        // Dropping the owner while the closure is pending on the main looper
        // must keep the complete derived object alive until it has run.
    }
    EXPECT_FALSE(destroyed);
    EXPECT_EQ(received, 0);

    pumpLooper();
    EXPECT_EQ(received, 42);
    EXPECT_TRUE(destroyed);
}

TEST_F(EventSenderTest, NotSharedOwnedDropsEvent) {
    int received = 0;
    bool destroyed = false;
    {
        TestEventSender sender(&received, &destroyed);
        // Debug builds assert; release builds log and drop the event.
        EXPECT_DEBUG_DEATH(sender.send(99), "shared_ptr");
    }
    EXPECT_TRUE(destroyed);

    pumpLooper();
    EXPECT_EQ(received, 0);
}

}  // namespace control
}  // namespace emulation
}  // namespace android
