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

#include "android/skin/qt/extended-pages/telephony-page.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <QApplication>
#include <QComboBox>
#include <QIcon>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>

#include "android/skin/qt/extended-pages/telephony-controller.h"
#include "android/skin/qt/stylesheet.h"
#include "ui_telephony-page.h"

using namespace testing;

namespace Ui {
const QHash<QString, QString>& stylesheetValues(SettingsTheme theme) {
    static QHash<QString, QString> values;
    return values;
}
const char THEME_PATH_VAR[] = "theme_path";
}  // namespace Ui

// Mock showErrorDialog
void showErrorDialog(const QString& message, const QString& title) {
    (void)message;
    (void)title;
}

class MockTelephonyController : public TelephonyController {
public:
    MOCK_METHOD(void,
                initCallAsync,
                (const std::string& number, TelephonyResultCallback cb),
                (override));
    MOCK_METHOD(void,
                disconnectCallAsync,
                (const std::string& number, TelephonyResultCallback cb),
                (override));
    MOCK_METHOD(void,
                holdCallAsync,
                (const std::string& number, TelephonyResultCallback cb),
                (override));
    MOCK_METHOD(void,
                unholdCallAsync,
                (const std::string& number, TelephonyResultCallback cb),
                (override));
    MOCK_METHOD(void,
                sendSmsAsync,
                (const std::string& sender,
                 const std::string& message,
                 TelephonyResultCallback cb),
                (override));
    MOCK_METHOD(void,
                setCallStateCallback,
                (std::function<void(int activeCalls)> callback),
                (override));
    MOCK_METHOD(void,
                updateTimeAsync,
                (TelephonyResultCallback cb),
                (override));
};

class TelephonyPageTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char* argv[] = {(char*)"test"};
            new QApplication(argc, argv);
        }

        auto mock = std::make_unique<MockTelephonyController>();
        mController = mock.get();

        // The page will call setCallStateCallback during initialization/show.
        EXPECT_CALL(*mController, setCallStateCallback(_)).Times(AnyNumber());

        TelephonyController::setForTest(std::move(mock));
        mPage = std::make_unique<TelephonyPage>();
        mPage->show();

        mNumberBox = mPage->findChild<QComboBox*>("tel_numberBox");
        mStartEndButton = mPage->findChild<QPushButton*>("tel_startEndButton");
        mHoldButton = mPage->findChild<QPushButton*>("tel_holdCallButton");
        mSmsMessageBox = mPage->findChild<QPlainTextEdit*>("sms_messageBox");
        mSmsSendButton = mPage->findChild<QPushButton*>("sms_sendButton");
    }

    void TearDown() override {
        mPage.reset();
        TelephonyController::resetForTest();
    }

    std::unique_ptr<TelephonyPage> mPage;
    MockTelephonyController* mController;
    QComboBox* mNumberBox;
    QPushButton* mStartEndButton;
    QPushButton* mHoldButton;
    QPlainTextEdit* mSmsMessageBox;
    QPushButton* mSmsSendButton;
};

TEST_F(TelephonyPageTest, InitCallUpdatesUIOnSuccess) {
    mNumberBox->setCurrentText("123456");

    // Capture the callback and invoke it with OK
    EXPECT_CALL(*mController, initCallAsync("123456", _))
            .WillOnce([](const std::string&, TelephonyResultCallback cb) {
                if (cb)
                    cb(TelephonyResponseStatus::OK);
            });

    mStartEndButton->click();

    EXPECT_EQ(mStartEndButton->text().toStdString(), "Hang up");
    EXPECT_FALSE(mNumberBox->isEnabled());
    EXPECT_TRUE(mHoldButton->isEnabled());
}

TEST_F(TelephonyPageTest, InitCallDoesNotUpdateUIOnFailure) {
    mNumberBox->setCurrentText("123456");

    // Capture the callback and invoke it with RADIO_OFF
    EXPECT_CALL(*mController, initCallAsync("123456", _))
            .WillOnce([](const std::string&, TelephonyResultCallback cb) {
                if (cb)
                    cb(TelephonyResponseStatus::RADIO_OFF);
            });

    mStartEndButton->click();

    // UI should remain in "Call device" state
    EXPECT_EQ(mStartEndButton->text().toStdString(), "Call device");
    EXPECT_TRUE(mNumberBox->isEnabled());
    EXPECT_FALSE(mHoldButton->isEnabled());
}

TEST_F(TelephonyPageTest, DisconnectCallUpdatesUIOnSuccess) {
    // Start a call first (successful)
    mNumberBox->setCurrentText("123456");
    EXPECT_CALL(*mController, initCallAsync(_, _))
            .WillOnce([](const std::string&, TelephonyResultCallback cb) {
                if (cb)
                    cb(TelephonyResponseStatus::OK);
            });
    mStartEndButton->click();

    // Disconnect should update UI after callback
    EXPECT_CALL(*mController, disconnectCallAsync("123456", _))
            .WillOnce([](const std::string&, TelephonyResultCallback cb) {
                if (cb)
                    cb(TelephonyResponseStatus::OK);
            });
    mStartEndButton->click();

    EXPECT_EQ(mStartEndButton->text().toStdString(), "Call device");
    EXPECT_TRUE(mNumberBox->isEnabled());
    EXPECT_FALSE(mHoldButton->isEnabled());
}

TEST_F(TelephonyPageTest, HoldToggle) {
    // Start a call first
    mNumberBox->setCurrentText("123");
    EXPECT_CALL(*mController, initCallAsync(_, _))
            .WillOnce([](const std::string&, TelephonyResultCallback cb) {
                if (cb)
                    cb(TelephonyResponseStatus::OK);
            });
    mStartEndButton->click();

    // To Hold - should change text to "Resume call" after callback
    EXPECT_CALL(*mController, holdCallAsync("123", _))
            .WillOnce([](const std::string&, TelephonyResultCallback cb) {
                if (cb)
                    cb(TelephonyResponseStatus::OK);
            });
    mHoldButton->click();
    EXPECT_EQ(mHoldButton->text().toStdString(), "Resume call");

    // To Unhold - should change text back to "Hold call" after callback
    EXPECT_CALL(*mController, unholdCallAsync("123", _))
            .WillOnce([](const std::string&, TelephonyResultCallback cb) {
                if (cb)
                    cb(TelephonyResponseStatus::OK);
            });
    mHoldButton->click();
    EXPECT_EQ(mHoldButton->text().toStdString(), "Hold call");
}

TEST_F(TelephonyPageTest, SendSms) {
    mNumberBox->setCurrentText("555");
    mSmsMessageBox->setPlainText("Hello");

    EXPECT_CALL(*mController, sendSmsAsync("555", "Hello", _));
    mSmsSendButton->click();
}

TEST_F(TelephonyPageTest, InputCleaning) {
    mNumberBox->setCurrentText("(555) 123-4567");

    // Expect only digits and plus
    EXPECT_CALL(*mController, initCallAsync("5551234567", _));

    mStartEndButton->click();
}

TEST_F(TelephonyPageTest, StatefulInputCleaning) {
    // 1. Initial call with messy number
    mNumberBox->setCurrentText("+1 (650) 555-1212");

    // Verify init call gets clean number
    EXPECT_CALL(*mController, initCallAsync("+16505551212", _))
            .WillOnce([](const std::string&, TelephonyResultCallback cb) {
                if (cb)
                    cb(TelephonyResponseStatus::OK);
            });
    mStartEndButton->click();

    // 2. Verify hold gets clean number
    EXPECT_CALL(*mController, holdCallAsync("+16505551212", _))
            .WillOnce([](const std::string&, TelephonyResultCallback cb) {
                if (cb)
                    cb(TelephonyResponseStatus::OK);
            });
    mHoldButton->click();

    // 3. Verify unhold gets clean number
    EXPECT_CALL(*mController, unholdCallAsync("+16505551212", _))
            .WillOnce([](const std::string&, TelephonyResultCallback cb) {
                if (cb)
                    cb(TelephonyResponseStatus::OK);
            });
    mHoldButton->click();

    // 4. Verify disconnect gets clean number
    EXPECT_CALL(*mController, disconnectCallAsync("+16505551212", _))
            .WillOnce([](const std::string&, TelephonyResultCallback cb) {
                if (cb)
                    cb(TelephonyResponseStatus::OK);
            });
    mStartEndButton->click();
}
