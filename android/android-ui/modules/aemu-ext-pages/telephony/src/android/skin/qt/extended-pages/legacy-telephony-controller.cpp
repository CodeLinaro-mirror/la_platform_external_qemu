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

#include "aemu/base/async/ThreadLooper.h"
#include "android/emulation/control/telephony_agent.h"
#include "android/skin/qt/function-runner.h"
#include "android/telephony/modem.h"
#include "android/telephony/sms.h"
#include "android_modem_v2.h"

#ifdef _WIN32
#undef ERROR
#endif

namespace {
TelephonyResponseStatus toStatus(TelephonyResponse tResp) {
    switch (tResp) {
        case Tel_Resp_OK:
            return TelephonyResponseStatus::OK;
        case Tel_Resp_Radio_Off:
            return TelephonyResponseStatus::RADIO_OFF;
        default:
            return TelephonyResponseStatus::ERROR;
    }
}

void invokeCallback(TelephonyResultCallback cb,
                    TelephonyResponseStatus status) {
    if (cb) {
        runOnEmuUiThread([cb, status]() { cb(status); });
    }
}
}  // namespace

LegacyTelephonyController::LegacyTelephonyController(
        const QAndroidTelephonyAgent* agent)
    : mAgent(agent) {}

LegacyTelephonyController::~LegacyTelephonyController() {}

void LegacyTelephonyController::initCallAsync(const std::string& number,
                                              TelephonyResultCallback cb) {
    if (mAgent && mAgent->telephonyCmd) {
        auto* agent = mAgent;
        android::base::ThreadLooper::runOnMainLooper([agent, number, cb]() {
            auto resp = agent->telephonyCmd(Tel_Op_Init_Call, number.c_str());
            invokeCallback(cb, toStatus(resp));
        });
    } else {
        invokeCallback(cb, TelephonyResponseStatus::ERROR);
    }
}

void LegacyTelephonyController::disconnectCallAsync(
        const std::string& number,
        TelephonyResultCallback cb) {
    if (mAgent && mAgent->telephonyCmd) {
        auto* agent = mAgent;
        android::base::ThreadLooper::runOnMainLooper([agent, number, cb]() {
            auto resp =
                    agent->telephonyCmd(Tel_Op_Disconnect_Call, number.c_str());
            invokeCallback(cb, toStatus(resp));
        });
    } else {
        invokeCallback(cb, TelephonyResponseStatus::ERROR);
    }
}

void LegacyTelephonyController::holdCallAsync(const std::string& number,
                                              TelephonyResultCallback cb) {
    if (mAgent && mAgent->telephonyCmd) {
        auto* agent = mAgent;
        android::base::ThreadLooper::runOnMainLooper([agent, number, cb]() {
            auto resp = agent->telephonyCmd(Tel_Op_Place_Call_On_Hold,
                                            number.c_str());
            invokeCallback(cb, toStatus(resp));
        });
    } else {
        invokeCallback(cb, TelephonyResponseStatus::ERROR);
    }
}

void LegacyTelephonyController::unholdCallAsync(const std::string& number,
                                                TelephonyResultCallback cb) {
    if (mAgent && mAgent->telephonyCmd) {
        auto* agent = mAgent;
        android::base::ThreadLooper::runOnMainLooper([agent, number, cb]() {
            auto resp = agent->telephonyCmd(Tel_Op_Take_Call_Off_Hold,
                                            number.c_str());
            invokeCallback(cb, toStatus(resp));
        });
    } else {
        invokeCallback(cb, TelephonyResponseStatus::ERROR);
    }
}

void LegacyTelephonyController::sendSmsAsync(const std::string& senderStr,
                                             const std::string& message,
                                             TelephonyResultCallback cb) {
    if (!mAgent || !mAgent->getModem) {
        invokeCallback(cb, TelephonyResponseStatus::ERROR);
        return;
    }

    AModem modem = mAgent->getModem();
    if (!modem) {
        invokeCallback(cb, TelephonyResponseStatus::ERROR);
        return;
    }

    SmsAddressRec sender;
    int retVal = sms_address_from_str(&sender, senderStr.c_str(),
                                      senderStr.length());
    if (retVal < 0 || sender.len <= 0) {
        invokeCallback(cb, TelephonyResponseStatus::ERROR);
        return;
    }

    const unsigned char* utf8Message =
            reinterpret_cast<const unsigned char*>(message.data());
    int nUtf8Chars = message.size();

    SmsPDU* pdus = smspdu_create_deliver_utf8(utf8Message, nUtf8Chars, &sender,
                                              nullptr);
    if (!pdus) {
        invokeCallback(cb, TelephonyResponseStatus::ERROR);
        return;
    }

    for (int idx = 0; pdus[idx] != nullptr; idx++) {
        amodem_receive_sms_vx(modem, pdus[idx]);
    }

    smspdu_free_list(pdus);
    invokeCallback(cb, TelephonyResponseStatus::OK);
}

void LegacyTelephonyController::setCallStateCallback(
        std::function<void(int activeCalls)> callback) {
    mCallback = std::move(callback);
    if (mAgent && mAgent->setNotifyCallback) {
        auto* agent = mAgent;
        android::base::ThreadLooper::runOnMainLooper([agent, this]() {
            agent->setNotifyCallback(cCallback, (void*)this);
        });
    }
}

void LegacyTelephonyController::updateTimeAsync(TelephonyResultCallback cb) {
    if (!mAgent || !mAgent->getModem) {
        invokeCallback(cb, TelephonyResponseStatus::ERROR);
        return;
    }

    auto* agent = mAgent;
    android::base::ThreadLooper::runOnMainLooper([agent, cb]() {
        AModem modem = agent->getModem();
        if (modem) {
            amodem_update_time(modem);
            invokeCallback(cb, TelephonyResponseStatus::OK);
        } else {
            invokeCallback(cb, TelephonyResponseStatus::ERROR);
        }
    });
}

// static
void LegacyTelephonyController::cCallback(void* userData, int numActiveCalls) {
    auto* self = static_cast<LegacyTelephonyController*>(userData);
    if (self && self->mCallback) {
        self->mCallback(numActiveCalls);
    }
}
