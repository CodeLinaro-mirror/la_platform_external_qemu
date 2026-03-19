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

#include "android/skin/qt/extended-pages/telephony-controller.h"

struct QAndroidTelephonyAgent;

class LegacyTelephonyController : public TelephonyController {
public:
    explicit LegacyTelephonyController(const QAndroidTelephonyAgent* agent);
    ~LegacyTelephonyController() override;

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
    const QAndroidTelephonyAgent* mAgent;
    std::function<void(int activeCalls)> mCallback;
    static void cCallback(void* userData, int numActiveCalls);
};
