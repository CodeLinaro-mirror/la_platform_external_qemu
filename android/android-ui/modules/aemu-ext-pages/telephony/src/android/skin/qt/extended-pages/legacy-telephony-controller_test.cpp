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

#include "android/skin/qt/extended-pages/legacy-telephony-controller.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>

#include "android/base/testing/TestLooper.h"
#include "android/emulation/control/telephony_agent.h"
#include "android/skin/qt/function-runner.h"
#include "android/utils/looper.h"

#ifdef _WIN32
#undef ERROR
#endif

using namespace testing;

// Mock runOnEmuUiThread to execute the callback immediately in the test.
void runOnEmuUiThread(UIFunction fn, bool waitUntilFinished) {
    fn();
}

class LegacyTelephonyControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char* argv[] = {(char*)"test"};
            new QApplication(argc, argv);
        }

        mLooper = std::make_unique<android::base::TestLooper>();
        android_registerMainLooper(reinterpret_cast<::Looper*>(
                static_cast<android::base::Looper*>(mLooper.get())));

        mAgent.telephonyCmd = [](TelephonyOperation op,
                                 const char* number) -> TelephonyResponse {
            sLastOp = op;
            sLastNumber = number ? number : "";
            return sResponse;
        };
        mAgent.initModem = nullptr;
        mAgent.getModem = nullptr;
        mAgent.setNotifyCallback = [](ModemCallback cb, void* userData) {
            sCallback = cb;
            sUserData = userData;
        };

        mController = std::make_unique<LegacyTelephonyController>(&mAgent);

        sLastOp = (TelephonyOperation)-1;
        sLastNumber = "";
        sCallback = nullptr;
        sUserData = nullptr;
        sResponse = Tel_Resp_OK;
    }

    void TearDown() override {
        mController.reset();
        android_registerMainLooper(nullptr);
        mLooper.reset();
    }

    void pumpLooper() {
        mLooper->runOneIterationWithDeadlineMs(mLooper->nowMs() + 10);
    }

    QAndroidTelephonyAgent mAgent;
    std::unique_ptr<LegacyTelephonyController> mController;
    std::unique_ptr<android::base::TestLooper> mLooper;

    static TelephonyOperation sLastOp;
    static std::string sLastNumber;
    static TelephonyResponse sResponse;
    static ModemCallback* sCallback;
    static void* sUserData;
};

TelephonyOperation LegacyTelephonyControllerTest::sLastOp =
        (TelephonyOperation)-1;
std::string LegacyTelephonyControllerTest::sLastNumber = "";
TelephonyResponse LegacyTelephonyControllerTest::sResponse = Tel_Resp_OK;
ModemCallback* LegacyTelephonyControllerTest::sCallback = nullptr;
void* LegacyTelephonyControllerTest::sUserData = nullptr;

TEST_F(LegacyTelephonyControllerTest, InitCallSuccess) {
    TelephonyResponseStatus status = TelephonyResponseStatus::ERROR;
    mController->initCallAsync("123456", [&](auto s) { status = s; });
    pumpLooper();
    EXPECT_EQ(sLastOp, Tel_Op_Init_Call);
    EXPECT_EQ(sLastNumber, "123456");
    EXPECT_EQ(status, TelephonyResponseStatus::OK);
}

TEST_F(LegacyTelephonyControllerTest, InitCallRadioOff) {
    sResponse = Tel_Resp_Radio_Off;
    TelephonyResponseStatus status = TelephonyResponseStatus::OK;
    mController->initCallAsync("123456", [&](auto s) { status = s; });
    pumpLooper();
    EXPECT_EQ(status, TelephonyResponseStatus::RADIO_OFF);
}

TEST_F(LegacyTelephonyControllerTest, InitCallError) {
    sResponse = Tel_Resp_Invalid_Action;
    TelephonyResponseStatus status = TelephonyResponseStatus::OK;
    mController->initCallAsync("123456", [&](auto s) { status = s; });
    pumpLooper();
    EXPECT_EQ(status, TelephonyResponseStatus::ERROR);
}

TEST_F(LegacyTelephonyControllerTest, DisconnectCallSuccess) {
    TelephonyResponseStatus status = TelephonyResponseStatus::ERROR;
    mController->disconnectCallAsync("654321", [&](auto s) { status = s; });
    pumpLooper();
    EXPECT_EQ(sLastOp, Tel_Op_Disconnect_Call);
    EXPECT_EQ(sLastNumber, "654321");
    EXPECT_EQ(status, TelephonyResponseStatus::OK);
}

TEST_F(LegacyTelephonyControllerTest, DisconnectCallError) {
    sResponse = Tel_Resp_Action_Failed;
    TelephonyResponseStatus status = TelephonyResponseStatus::OK;
    mController->disconnectCallAsync("654321", [&](auto s) { status = s; });
    pumpLooper();
    EXPECT_EQ(status, TelephonyResponseStatus::ERROR);
}

TEST_F(LegacyTelephonyControllerTest, HoldCallSuccess) {
    TelephonyResponseStatus status = TelephonyResponseStatus::ERROR;
    mController->holdCallAsync("123", [&](auto s) { status = s; });
    pumpLooper();
    EXPECT_EQ(sLastOp, Tel_Op_Place_Call_On_Hold);
    EXPECT_EQ(status, TelephonyResponseStatus::OK);
}

TEST_F(LegacyTelephonyControllerTest, UnholdCallSuccess) {
    TelephonyResponseStatus status = TelephonyResponseStatus::ERROR;
    mController->unholdCallAsync("123", [&](auto s) { status = s; });
    pumpLooper();
    EXPECT_EQ(sLastOp, Tel_Op_Take_Call_Off_Hold);
    EXPECT_EQ(status, TelephonyResponseStatus::OK);
}

TEST_F(LegacyTelephonyControllerTest, SendSmsErrorNoModem) {
    TelephonyResponseStatus status = TelephonyResponseStatus::OK;
    // getModem is nullptr in SetUp
    mController->sendSmsAsync("555", "hello", [&](auto s) { status = s; });
    // pumpLooper not needed as it fails immediately before looper in this
    // implementation Wait, check implementation - sendSms is NOT scheduled on
    // main looper currently.
    EXPECT_EQ(status, TelephonyResponseStatus::ERROR);
}

TEST_F(LegacyTelephonyControllerTest, UpdateTimeAsyncErrorNoModem) {
    TelephonyResponseStatus status = TelephonyResponseStatus::OK;
    mAgent.getModem = []() -> AModem { return nullptr; };

    mController->updateTimeAsync([&](auto s) { status = s; });
    pumpLooper();
    EXPECT_EQ(status, TelephonyResponseStatus::ERROR);
}

TEST_F(LegacyTelephonyControllerTest, CallbackWorks) {
    int receivedCalls = -1;
    mController->setCallStateCallback(
            [&](int calls) { receivedCalls = calls; });
    pumpLooper();

    ASSERT_NE(sCallback, nullptr);
    sCallback(sUserData, 42);

    EXPECT_EQ(receivedCalls, 42);
}
