// Copyright (C) 2025 The Android Open Source Project
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

#include "android/skin/qt/extended-pages/legacy-battery-controller.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <chrono>
#include <thread>

#include "android/base/testing/TestLooper.h"
#include "android/emulation/control/battery_agent.h"
#include "android/utils/looper.h"

using ::testing::_;

// A mockable struct to hold the agent function pointers.
struct MockBatteryAgent {
    MOCK_METHOD(void, setChargeLevel, (int level));
    MOCK_METHOD(void, setCharger, (BatteryCharger charger));
    MOCK_METHOD(void, setHealth, (BatteryHealth health));
    MOCK_METHOD(void, setStatus, (BatteryStatus status));
};

static constexpr android::base::Looper::Duration kTimeoutMs = 100;

MockBatteryAgent* g_mockAgentPtr;

class LegacyBatteryControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_mockAgentPtr = &m_mockAgent;
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char* argv[] = {(char*)"test"};
            new QApplication(argc, argv);
        }

        mLooper = std::make_unique<android::base::TestLooper>();
        // TestLooper -> base::Looper -> C Looper
        android_registerMainLooper(reinterpret_cast<::Looper*>(
                static_cast<android::base::Looper*>(mLooper.get())));

        // Wire up the QAndroidBatteryAgent to call our mock methods.
        m_qAgent.setChargeLevel = [](int level) {
            g_mockAgentPtr->setChargeLevel(level);
        };
        m_qAgent.setCharger = [](BatteryCharger c) {
            g_mockAgentPtr->setCharger(c);
        };
        m_qAgent.setHealth = [](BatteryHealth h) { g_mockAgentPtr->setHealth(h); };
        m_qAgent.setStatus = [](BatteryStatus s) { g_mockAgentPtr->setStatus(s); };

        m_controller = std::make_unique<LegacyBatteryController>(&m_qAgent);
        testing::Mock::AllowLeak(&m_mockAgent);
    }

    void TearDown() override {
        m_controller.reset();
        android_registerMainLooper(nullptr);
        mLooper.reset();
    }

    void pumpLooper() {
        constexpr android::base::Looper::Duration kStep = 50;  // 50 ms.

        android::base::Looper::Duration current = mLooper->nowMs();
        const android::base::Looper::Duration deadline =
                mLooper->nowMs() + kTimeoutMs;

        while (current + kStep < deadline) {
            mLooper->runOneIterationWithDeadlineMs(current + kStep);
            current = mLooper->nowMs();
        }
    }

    MockBatteryAgent m_mockAgent;
    std::unique_ptr<LegacyBatteryController> m_controller;
    QAndroidBatteryAgent m_qAgent = {};
    std::unique_ptr<android::base::TestLooper> mLooper;
};

TEST_F(LegacyBatteryControllerTest, SetBatteryCallsAllAgentMethods) {
    BatteryState state = {88,
                          BATTERY_CHARGER_AC,
                          BATTERY_HEALTH_GOOD,
                          BATTERY_STATUS_CHARGING,
                          true,
                          true};

    EXPECT_CALL(m_mockAgent, setChargeLevel(88));
    EXPECT_CALL(m_mockAgent, setCharger(BATTERY_CHARGER_AC));
    EXPECT_CALL(m_mockAgent, setHealth(BATTERY_HEALTH_GOOD));
    EXPECT_CALL(m_mockAgent, setStatus(BATTERY_STATUS_CHARGING));

    m_controller->setBattery(state);
    QCoreApplication::processEvents();  // Process the async calls
    pumpLooper();
}

TEST_F(LegacyBatteryControllerTest, SetBatteryOnlyCallsChangedMethods) {
    BatteryState initialState = {88,
                                 BATTERY_CHARGER_AC,
                                 BATTERY_HEALTH_GOOD,
                                 BATTERY_STATUS_CHARGING,
                                 true,
                                 true};
    BatteryState nextState = initialState;
    nextState.chargeLevel = 50;  // Only change one field.

    // First call should set everything.
    EXPECT_CALL(m_mockAgent, setChargeLevel(_));
    EXPECT_CALL(m_mockAgent, setCharger(_));
    EXPECT_CALL(m_mockAgent, setHealth(_));
    EXPECT_CALL(m_mockAgent, setStatus(_));
    m_controller->setBattery(initialState);
    QCoreApplication::processEvents();
    pumpLooper();

    // Second call should only invoke the method for the changed field.
    EXPECT_CALL(m_mockAgent, setChargeLevel(50));
    EXPECT_CALL(m_mockAgent, setCharger(_)).Times(0);
    EXPECT_CALL(m_mockAgent, setHealth(_)).Times(0);
    EXPECT_CALL(m_mockAgent, setStatus(_)).Times(0);
    m_controller->setBattery(nextState);
    QCoreApplication::processEvents();
    pumpLooper();
}
