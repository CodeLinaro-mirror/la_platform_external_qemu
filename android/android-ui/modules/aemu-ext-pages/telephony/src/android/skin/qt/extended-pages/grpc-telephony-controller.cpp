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

#include "android/skin/qt/extended-pages/grpc-telephony-controller.h"

#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "android/skin/qt/function-runner.h"
#include "android/utils/debug.h"

using android::emulation::control::EmulatorGrpcClient;
using android::emulation::control::ModemClient;
using android::emulation::control::incubating::Call;
using android::emulation::control::incubating::PhoneEvent;
using android::emulation::control::incubating::SmsMessage;

#ifdef _WIN32
#undef ERROR
#endif

#define DEBUG 0
/* set  >1 for very verbose debugging */
#if DEBUG <= 1
#define DD(...) (void)0
#else
#define DD(...) dinfo(__VA_ARGS__)
#endif

namespace {
TelephonyResponseStatus toStatus(const absl::Status& status) {
    if (status.ok()) {
        return TelephonyResponseStatus::OK;
    }
    // We can try to be more specific if the error message contains "radio" or
    // if we use specific gRPC error codes.
    std::string msg = std::string(status.message());
    if (msg.find("radio") != std::string::npos ||
        msg.find("Radio") != std::string::npos) {
        return TelephonyResponseStatus::RADIO_OFF;
    }
    return TelephonyResponseStatus::ERROR;
}

void invokeCallback(TelephonyResultCallback cb,
                    TelephonyResponseStatus status) {
    if (cb) {
        runOnEmuUiThread([cb, status]() { cb(status); });
    }
}
}  // namespace

GrpcTelephonyController::GrpcTelephonyController() {
    mModemClient = std::make_shared<ModemClient>(EmulatorGrpcClient::me());
}

GrpcTelephonyController::GrpcTelephonyController(
        std::shared_ptr<ModemClient> client)
    : mModemClient(std::move(client)) {}

void GrpcTelephonyController::initCallAsync(const std::string& number,
                                            TelephonyResultCallback cb) {
    Call call;
    call.set_number(number);
    call.set_direction(Call::CALL_DIRECTION_INBOUND);
    mModemClient->createCallAsync(call, [cb](auto statusOr) {
        invokeCallback(cb, toStatus(statusOr.status()));
    });
}

void GrpcTelephonyController::disconnectCallAsync(const std::string& number,
                                                  TelephonyResultCallback cb) {
    Call call;
    call.set_number(number);
    mModemClient->deleteCallAsync(call, [cb](auto statusOr) {
        invokeCallback(cb, toStatus(statusOr.status()));
    });
}

void GrpcTelephonyController::holdCallAsync(const std::string& number,
                                            TelephonyResultCallback cb) {
    Call call;
    call.set_number(number);
    call.set_state(Call::CALL_STATE_HELD);
    mModemClient->updateCallAsync(call, [cb](auto statusOr) {
        invokeCallback(cb, toStatus(statusOr.status()));
    });
}

void GrpcTelephonyController::unholdCallAsync(const std::string& number,
                                              TelephonyResultCallback cb) {
    Call call;
    call.set_number(number);
    call.set_state(Call::CALL_STATE_ACTIVE);
    mModemClient->updateCallAsync(call, [cb](auto statusOr) {
        invokeCallback(cb, toStatus(statusOr.status()));
    });
}

void GrpcTelephonyController::sendSmsAsync(const std::string& sender,
                                           const std::string& message,
                                           TelephonyResultCallback cb) {
    SmsMessage sms;
    sms.set_number(sender);
    sms.set_text(message);
    mModemClient->receiveSmsAsync(sms, [cb](auto statusOr) {
        invokeCallback(cb, toStatus(statusOr.status()));
    });
}

void GrpcTelephonyController::setCallStateCallback(
        std::function<void(int activeCalls)> callback) {
    mCallback = std::move(callback);
    mModemClient->receivePhoneEvents(
            [this](const PhoneEvent* event) { handlePhoneEvent(event); },
            [](auto _status) {
                DD("Phone events stream finished with status: %s",
                   _status.ToString().c_str());
            });
}

void GrpcTelephonyController::updateTimeAsync(TelephonyResultCallback cb) {
    mModemClient->updateTimeAsync([cb](auto statusOr) {
        invokeCallback(cb, toStatus(statusOr.status()));
    });
}

void GrpcTelephonyController::handlePhoneEvent(const PhoneEvent* event) {
    if (event->type() == PhoneEvent::PHONE_EVENT_TYPE_ACTIVE &&
        event->has_active()) {
        int activeCalls = event->active().calls_size();
        if (mCallback) {
            mCallback(activeCalls);
        }
    }
}
