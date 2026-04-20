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

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "android/android-ui/apps/fishtank/fishtank_agents.h"
#include "android/emulation/control/user_event_agent.h"
#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "android/grpc/utils/SimpleAsyncGrpc.h"
#include "android/multitouch-screen.h"
#include "android/skin/event.h"
#include "emulator_controller.pb.h"

using android::emulation::control::InputEvent;
using android::emulation::control::Touch;
using android::emulation::control::TouchEvent;
using testing::_;

// Define a capturing writer that overrides the virtual Write method.
class CapturingInputEventWriter : public SimpleClientWriter<InputEvent> {
public:
    CapturingInputEventWriter() : SimpleClientWriter<InputEvent>(nullptr) {}

    void Write(const InputEvent& msg) override {
        captured.push_back(msg);
    }

    std::vector<InputEvent> captured;
};

class MockEmulatorControlClient : public android::emulation::control::EmulatorControlClient {
public:
    MockEmulatorControlClient() : EmulatorControlClient() {}
    MOCK_METHOD(SimpleClientWriter<InputEvent>*, asyncInputEventWriter, (), (override));
};

class UserEventAgentTest : public ::testing::Test {
protected:
    void SetUp() override {
        mCapturingWriter = new CapturingInputEventWriter();
        EXPECT_CALL(mMockClient, asyncInputEventWriter())
                .WillRepeatedly(testing::Return(mCapturingWriter));

        initializeGrpcUserEventAgent(&mMockClient);
        mAgent = &sFishtankQAndroidUserEventAgent;
    }

    void TearDown() override {
        initializeGrpcUserEventAgent(nullptr);
        delete mCapturingWriter;
    }

    MockEmulatorControlClient mMockClient;
    CapturingInputEventWriter* mCapturingWriter;
    const QAndroidUserEventAgent* mAgent;
};

TEST_F(UserEventAgentTest, SendTouchEventsBuffersUntilSync) {
    SkinEvent ev1 = {};
    ev1.type = kEventTouchUpdate;
    ev1.u.multi_touch_point.x = 100;
    ev1.u.multi_touch_point.y = 200;
    ev1.u.multi_touch_point.id = 0;
    ev1.u.multi_touch_point.pressure = 100;
    ev1.u.multi_touch_point.skip_sync = true;

    mAgent->sendTouchEvents(&ev1, 0);
    EXPECT_EQ(mCapturingWriter->captured.size(), 0);

    SkinEvent ev2 = {};
    ev2.type = kEventTouchUpdate;
    ev2.u.multi_touch_point.x = 300;
    ev2.u.multi_touch_point.y = 400;
    ev2.u.multi_touch_point.id = 1;
    ev2.u.multi_touch_point.pressure = 100;
    ev2.u.multi_touch_point.skip_sync = false; // Sync here!

    mAgent->sendTouchEvents(&ev2, 0);
    ASSERT_EQ(mCapturingWriter->captured.size(), 1);

    const auto& msg = mCapturingWriter->captured[0];
    ASSERT_TRUE(msg.has_touch_event());
    EXPECT_EQ(msg.touch_event().touches_size(), 2);
    EXPECT_EQ(msg.touch_event().touches(0).x(), 100);
    EXPECT_EQ(msg.touch_event().touches(1).x(), 300);
    EXPECT_EQ(msg.touch_event().display(), 0);
}

TEST_F(UserEventAgentTest, SendTouchEventsBuffersPerDisplay) {
    SkinEvent ev1 = {};
    ev1.type = kEventTouchUpdate;
    ev1.u.multi_touch_point.x = 100;
    ev1.u.multi_touch_point.display_id = 0;
    ev1.u.multi_touch_point.skip_sync = true;

    SkinEvent ev2 = {};
    ev2.type = kEventTouchUpdate;
    ev2.u.multi_touch_point.x = 500;
    ev2.u.multi_touch_point.display_id = 1;
    ev2.u.multi_touch_point.skip_sync = true;

    mAgent->sendTouchEvents(&ev1, 0);
    mAgent->sendTouchEvents(&ev2, 1);
    EXPECT_EQ(mCapturingWriter->captured.size(), 0);

    // Sync display 0. Display 1 should still be buffered and NOT sent.
    ev1.u.multi_touch_point.skip_sync = false;
    mAgent->sendTouchEvents(&ev1, 0);
    ASSERT_EQ(mCapturingWriter->captured.size(), 1);
    EXPECT_EQ(mCapturingWriter->captured[0].touch_event().display(), 0);
    EXPECT_EQ(mCapturingWriter->captured[0].touch_event().touches_size(), 2);

    // Add another point to display 1, still skip_sync.
    ev2.u.multi_touch_point.x = 600;
    ev2.u.multi_touch_point.skip_sync = true;
    mAgent->sendTouchEvents(&ev2, 1);
    EXPECT_EQ(mCapturingWriter->captured.size(), 1);

    // Sync display 1. It should now send all 3 points (original ev2 + second ev2 + sync ev2).
    ev2.u.multi_touch_point.skip_sync = false;
    mAgent->sendTouchEvents(&ev2, 1);
    ASSERT_EQ(mCapturingWriter->captured.size(), 2);
    EXPECT_EQ(mCapturingWriter->captured[1].touch_event().display(), 1);
    EXPECT_EQ(mCapturingWriter->captured[1].touch_event().touches_size(), 3);
}

TEST_F(UserEventAgentTest, SendMouseEventHandlesSimulatedMultiTouch) {
    // Simulated finger 1 (skip_sync = true, secondary = false) -> buttons = 1 | 2 = 3
    mAgent->sendMouseEvent(100, 200, 0, kShiftIsTouchDown | kShiftShouldSkipSync, 0, MOUSE_EVENT_MODE_ABS);
    EXPECT_EQ(mCapturingWriter->captured.size(), 0);

    // Simulated finger 2 (skip_sync = false, secondary = true) -> buttons = 1 | 4 = 5
    mAgent->sendMouseEvent(300, 400, 0, kShiftIsTouchDown | kShiftSecondaryTouch, 0, MOUSE_EVENT_MODE_ABS);
    ASSERT_EQ(mCapturingWriter->captured.size(), 1);

    const auto& msg = mCapturingWriter->captured[0];
    ASSERT_TRUE(msg.has_touch_event());
    EXPECT_EQ(msg.touch_event().touches_size(), 2);
    EXPECT_EQ(msg.touch_event().touches(0).identifier(), 0);
    EXPECT_EQ(msg.touch_event().touches(0).x(), 100);
    EXPECT_EQ(msg.touch_event().touches(1).identifier(), 1);
    EXPECT_EQ(msg.touch_event().touches(1).x(), 300);
}

TEST_F(UserEventAgentTest, NormalMouseEventSendsMouseEvent) {
    // Normal left click down (buttons = 1)
    mAgent->sendMouseEvent(50, 60, 0, 1, 0, MOUSE_EVENT_MODE_ABS);
    ASSERT_EQ(mCapturingWriter->captured.size(), 1);

    const auto& msg = mCapturingWriter->captured[0];
    ASSERT_TRUE(msg.has_mouse_event());
    EXPECT_EQ(msg.mouse_event().x(), 50);
    EXPECT_EQ(msg.mouse_event().y(), 60);
    EXPECT_EQ(msg.mouse_event().buttons(), 1);
}
