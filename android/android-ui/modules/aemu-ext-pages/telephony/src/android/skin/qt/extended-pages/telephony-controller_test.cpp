// Copyright (C) 2026 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/skin/qt/extended-pages/telephony-controller.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "android/emulation/control/telephony_agent.h"

using namespace testing;

class TelephonyControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset the singleton before each test.
        TelephonyController::resetForTest();
    }

    void TearDown() override { TelephonyController::resetForTest(); }
};

TEST_F(TelephonyControllerTest, NoOpBehavior) {
    // Lazy initialization should yield NoOp if no agents are set up in this
    // test context
    auto* controller = TelephonyController::get();
    ASSERT_NE(controller, nullptr);

    bool called = false;
    controller->initCallAsync("123", [&](TelephonyResponseStatus status) {
        EXPECT_EQ(status, TelephonyResponseStatus::ERROR);
        called = true;
    });
    EXPECT_TRUE(called);

    called = false;
    controller->sendSmsAsync("555", "msg", [&](TelephonyResponseStatus status) {
        EXPECT_EQ(status, TelephonyResponseStatus::ERROR);
        called = true;
    });
    EXPECT_TRUE(called);

    called = false;
    controller->updateTimeAsync([&](TelephonyResponseStatus status) {
        EXPECT_EQ(status, TelephonyResponseStatus::ERROR);
        called = true;
    });
    EXPECT_TRUE(called);
}

TEST_F(TelephonyControllerTest, SetForTest) {
    class MockController : public TelephonyController {
    public:
        MOCK_METHOD(void,
                    initCallAsync,
                    (const std::string&, TelephonyResultCallback),
                    (override));
        MOCK_METHOD(void,
                    disconnectCallAsync,
                    (const std::string&, TelephonyResultCallback),
                    (override));
        MOCK_METHOD(void,
                    holdCallAsync,
                    (const std::string&, TelephonyResultCallback),
                    (override));
        MOCK_METHOD(void,
                    unholdCallAsync,
                    (const std::string&, TelephonyResultCallback),
                    (override));
        MOCK_METHOD(void,
                    sendSmsAsync,
                    (const std::string&,
                     const std::string&,
                     TelephonyResultCallback),
                    (override));
        MOCK_METHOD(void,
                    setCallStateCallback,
                    (std::function<void(int)>),
                    (override));
        MOCK_METHOD(void,
                    updateTimeAsync,
                    (TelephonyResultCallback),
                    (override));
    };

    auto mock = std::make_unique<MockController>();
    EXPECT_CALL(*mock, updateTimeAsync(_)).Times(1);

    TelephonyController::setForTest(std::move(mock));
    TelephonyController::get()->updateTimeAsync(nullptr);
}
