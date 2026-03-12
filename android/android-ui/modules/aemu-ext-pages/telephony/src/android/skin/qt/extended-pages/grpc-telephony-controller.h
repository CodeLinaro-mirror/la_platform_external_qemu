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

#pragma once

#include <map>
#include <memory>
#include "android/emulation/control/utils/ModemClient.h"
#include "android/skin/qt/extended-pages/telephony-controller.h"

class GrpcTelephonyController : public TelephonyController {
public:
    GrpcTelephonyController();
    // Constructor for testing purposes.
    explicit GrpcTelephonyController(
            std::shared_ptr<android::emulation::control::ModemClient> client);
    ~GrpcTelephonyController() override = default;

    void initCallAsync(const std::string& number,
                       TelephonyResultCallback cb = nullptr) override;
    void disconnectCallAsync(const std::string& number,
                             TelephonyResultCallback cb = nullptr) override;
    void holdCallAsync(const std::string& number,
                       TelephonyResultCallback cb = nullptr) override;
    void unholdCallAsync(const std::string& number,
                         TelephonyResultCallback cb = nullptr) override;
    void sendSmsAsync(const std::string& sender,
                      const std::string& message,
                      TelephonyResultCallback cb = nullptr) override;

    void setCallStateCallback(
            std::function<void(int activeCalls)> callback) override;

    void updateTimeAsync(TelephonyResultCallback cb = nullptr) override;

private:
    void handlePhoneEvent(
            const android::emulation::control::incubating::PhoneEvent* event);

    std::shared_ptr<android::emulation::control::ModemClient> mModemClient;
    std::function<void(int activeCalls)> mCallback;
};
