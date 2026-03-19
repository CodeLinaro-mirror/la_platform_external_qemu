// Copyright (C) 2015-2016 The Android Open Source Project
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

#include <limits.h>            // for CHAR_BIT
#include <qchar.h>             // for operator==
#include <stddef.h>            // for NULL
#include <QChar>               // for QChar
#include <QComboBox>           // for QComboBox
#include <QCoreApplication>    // for QCoreApplication
#include <QIcon>               // for QIcon
#include <QPlainTextEdit>      // for QPlainTextEdit
#include <QRegularExpression>  // for QRegularExpre...
#include <cassert>             // for assert
#include <string>              // for basic_string

#include "aemu/base/async/Looper.h"
#include "aemu/base/async/ThreadLooper.h"

#include "android/avd/info.h"  // for avdInfo_getAv...
#include "android/avd/util.h"  // for AVD_ANDROID_AUTO
#include "android/cmdline-definitions.h"
#include "android/console.h"   // for getConsoleAgents()->settings->avdInfo()
#include "android/emulation/control/telephony_agent.h"  // for QAndroidTelep...
#include "android/metrics/UiEventTracker.h"
#include "android/settings-agent.h"                 // for SettingsTheme
#include "android/skin/qt/error-dialog.h"           // for showErrorDialog
#include "android/skin/qt/extended-pages/common.h"  // for setButtonEnabled
#include "android/skin/qt/extended-pages/telephony-controller.h"
#include "android/skin/qt/function-runner.h"
#include "android/skin/qt/raised-material-button.h"  // for RaisedMateria...
#include "android/telephony/modem.h"                 // for amodem_get_ra...
#include "android/telephony/sms.h"                   // for SmsPDU, is_in...
#include "android_modem_v2.h"                        // for TelephonyPage
#include "host-common/VmLock.h"                      // for RecursiveScop...
#include "ui_telephony-page.h"                       // for TelephonyPage

class QString;
class QWidget;

#define MAX_SMS_MSG_SIZE 1024  // Arbitrary emulator limitation
#define MAX_SMS_MSG_SIZE_STRING "1024"

static const QAndroidTelephonyAgent* sTelephonyAgent = nullptr;

class TelephonyEvent : public QEvent {
public:
    TelephonyEvent(QEvent::Type typeId, int nActive)
        : QEvent(typeId), numActiveCalls(nActive) {}

public:
    int numActiveCalls;
};

TelephonyPage::TelephonyPage(QWidget* parent)
    : QWidget(parent),
      mUi(new Ui::TelephonyPage()),
      mPhoneTracker(new UiEventTracker(
              android_studio::EmulatorUiEvent::BUTTON_PRESS,
              android_studio::EmulatorUiEvent::EXTENDED_TELEPHONY_TAB)),
      mCallActivity(TelephonyPage::CallActivity::Inactive) {
    mUi->setupUi(this);
    mUi->tel_numberBox->setValidator(new PhoneNumberValidator());
    mCustomEventType = (QEvent::Type)QEvent::registerEventType();

    // Disable sms button and box for Automotive, since it's not supported
    if ((getConsoleAgents()->settings->avdInfo() &&
         (avdInfo_getAvdFlavor(getConsoleAgents()->settings->avdInfo()) ==
          AVD_ANDROID_AUTO))) {
        SettingsTheme theme = getSelectedTheme();
        setButtonEnabled(mUi->sms_sendButton, theme, false);
        mUi->sms_messageBox->setReadOnly(true);
    }
}

TelephonyPage::~TelephonyPage() {
    TelephonyController::get()->setCallStateCallback(nullptr);
}

void TelephonyPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    TelephonyController::get()->setCallStateCallback(
            [this](int numActiveCalls) { eventLauncher(numActiveCalls); });
}

void TelephonyPage::on_tel_startEndButton_clicked() {
    mPhoneTracker->increment("CALL");

    if (mCallActivity == CallActivity::Inactive) {
        // Start a call
        // Get rid of spurious characters from the phone number
        // (Allow only '+' and '0'..'9')
        // Note: phoneNumberValidator validates the user's input, but
        // allows some human-readable characters like '.' and ')'.
        // Here we remove that meaningless punctuation.
        QString cleanNumber = mUi->tel_numberBox->currentText().remove(
                QRegularExpression("[^+0-9]"));

        TelephonyController::get()->initCallAsync(
                cleanNumber.toStdString(), [this, cleanNumber](auto status) {
                    runOnEmuUiThread([this, status, cleanNumber] {
                        if (status != TelephonyResponseStatus::OK) {
                            const char* errMsg =
                                    (status ==
                                     TelephonyResponseStatus::RADIO_OFF)
                                            ? "The call failed: radio is off."
                                            : "The call failed.";
                            showErrorDialog(tr(errMsg), tr("Telephony"));
                            return;
                        }

                        // Success: Update the state and the UI buttons
                        mCallActivity = CallActivity::Active;
                        mPhoneNumber = cleanNumber;

                        mUi->tel_numberBox->setEnabled(false);

                        SettingsTheme theme = getSelectedTheme();
                        mUi->tel_startEndButton->setIcon(
                                QIcon(":/resources/phone_hangup_white.png"));
                        mUi->tel_startEndButton->setText(tr("Hang up"));
                        mUi->tel_startEndButton->setToolTip(tr("Hang up"));
                        setButtonEnabled(mUi->tel_holdCallButton, theme, true);
                    });
                });
    } else {
        // Hang up (all) active calls
        TelephonyController::get()->disconnectCallAsync(
                mPhoneNumber.toStdString(), [this](auto status) {
                    runOnEmuUiThread([this, status] {
                        if (status != TelephonyResponseStatus::OK) {
                            showErrorDialog(tr("The call could not be ended."),
                                            tr("Telephony"));
                            return;
                        }

                        // Success: Update the state and the UI buttons
                        mCallActivity = CallActivity::Inactive;
                        mPhoneNumber = "";

                        mUi->tel_numberBox->setEnabled(true);

                        SettingsTheme theme = getSelectedTheme();
                        mUi->tel_startEndButton->setIcon(
                                QIcon(":/resources/phone_white.png"));
                        mUi->tel_startEndButton->setText(tr("Call device"));
                        mUi->tel_startEndButton->setToolTip(tr("Call device"));
                        mUi->tel_holdCallButton->setText(tr("Hold call"));
                        mUi->tel_holdCallButton->setToolTip(tr("Hold call"));
                        setButtonEnabled(mUi->tel_holdCallButton, theme, false);
                    });
                });
    }
}

void TelephonyPage::on_tel_holdCallButton_clicked() {
    mPhoneTracker->increment("HOLD");
    if (mCallActivity == CallActivity::Active) {
        // Place call on hold
        TelephonyController::get()->holdCallAsync(
                mPhoneNumber.toStdString(), [this](auto status) {
                    runOnEmuUiThread([this, status] {
                        if (status != TelephonyResponseStatus::OK) {
                            showErrorDialog(tr("The call could not be held."),
                                            tr("Telephony"));
                            return;
                        }

                        mCallActivity = CallActivity::Held;
                        mUi->tel_holdCallButton->setText(tr("Resume call"));
                        mUi->tel_holdCallButton->setToolTip(tr("Resume call"));
                    });
                });
    } else if (mCallActivity == CallActivity::Held) {
        // Take call off hold
        TelephonyController::get()->unholdCallAsync(
                mPhoneNumber.toStdString(), [this](auto status) {
                    runOnEmuUiThread([this, status] {
                        if (status != TelephonyResponseStatus::OK) {
                            showErrorDialog(
                                    tr("The call could not be resumed."),
                                    tr("Telephony"));
                            return;
                        }

                        mCallActivity = CallActivity::Active;
                        mUi->tel_holdCallButton->setText(tr("Hold call"));
                        mUi->tel_holdCallButton->setToolTip(tr("Hold call"));
                    });
                });
    }
}

void TelephonyPage::on_sms_sendButton_clicked() {
    mPhoneTracker->increment("SMS");
    QString theMessage = mUi->sms_messageBox->toPlainText();
    if (theMessage.length() > MAX_SMS_MSG_SIZE) {
        showErrorDialog(
                tr("The message was not sent because it was over " MAX_SMS_MSG_SIZE_STRING
                   " characters."),
                tr("Telephony"));
        return;
    }

    TelephonyController::get()->sendSmsAsync(
            mUi->tel_numberBox->currentText().toStdString(),
            theMessage.toStdString(), [this](auto status) {
                runOnEmuUiThread([this, status] {
                    if (status != TelephonyResponseStatus::OK) {
                        showErrorDialog(tr("The SMS message was not sent."),
                                        tr("Telephony"));
                    }
                });
            });
}

void TelephonyPage::customEvent(QEvent* event) {
    if (event->type() == mCustomEventType) {
        TelephonyEvent* telEvent = (TelephonyEvent*)event;
        if (telEvent) {
            bool hasActive = (telEvent->numActiveCalls > 0);
            if (!hasActive && mCallActivity != CallActivity::Inactive) {
                // If there are no more active calls, and our UI thinks there
                // are, update the UI to show everything is inactive.
                mCallActivity = CallActivity::Inactive;
                mPhoneNumber = "";

                mUi->tel_numberBox->setEnabled(true);

                SettingsTheme theme = getSelectedTheme();
                mUi->tel_startEndButton->setIcon(
                        QIcon(":/resources/phone_white.png"));
                mUi->tel_startEndButton->setText(tr("Call device"));
                mUi->tel_startEndButton->setToolTip(tr("Call device"));
                mUi->tel_holdCallButton->setText(tr("Hold call"));
                mUi->tel_holdCallButton->setToolTip(tr("Hold call"));
                setButtonEnabled(mUi->tel_holdCallButton, theme, false);
            }
        }
    }
}

void TelephonyPage::eventLauncher(int numActiveCalls) {
    QCoreApplication::postEvent(
            this, new TelephonyEvent(mCustomEventType, numActiveCalls));
}

// static
void TelephonyPage::setTelephonyAgent(const QAndroidTelephonyAgent* agent) {
    sTelephonyAgent = agent;
}

TelephonyPage::PhoneNumberValidator::State
TelephonyPage::PhoneNumberValidator::validate(QString& input, int& pos) const {
    if (input.isEmpty()) {
        return QValidator::Intermediate;
    }

    if (input.at(0).isDigit() || input.at(0) == '+') {
        return validateAsDigital(input);
    } else {
        return validateAsAlphanumeric(input);
    }
}

TelephonyPage::PhoneNumberValidator::State
TelephonyPage::PhoneNumberValidator::validateAsDigital(const QString& input) {
    int numDigits = 0;
    const int MAX_DIGITS = 16;
    static const QString acceptable_non_digits = "-.()/ +";

    if (input.length() >= 32) {
        return QValidator::Invalid;
    }

    for (int i = 0; i < input.length(); i++) {
        const QChar c = input[i];
        if (c.isDigit()) {
            numDigits++;
            if (numDigits > MAX_DIGITS) {
                return QValidator::Invalid;
            }
        } else if (c == '+' && i != 0) {
            // '+' is only allowed as the first character
            return QValidator::Invalid;
        } else if (!acceptable_non_digits.contains(c)) {
            return QValidator::Invalid;
        }
    }

    return ((numDigits > 0) ? QValidator::Acceptable
                            : QValidator::Intermediate);
}

TelephonyPage::PhoneNumberValidator::State
TelephonyPage::PhoneNumberValidator::validateAsAlphanumeric(
        const QString& input) {
    // Alphanumeric address is BITS_PER_SMS_CHAR bits per symbol

    if (input.length() == 0) {
        return QValidator::Intermediate;
    }

    if (input.length() >
        (SMS_ADDRESS_MAX_SIZE * CHAR_BIT / BITS_PER_SMS_CHAR)) {
        return QValidator::Invalid;
    }

    for (int i = 0; i < input.length(); i++) {
        if (!is_in_gsm_default_alphabet(input[i].unicode())) {
            return QValidator::Invalid;
        }
    }

    return QValidator::Acceptable;
}
