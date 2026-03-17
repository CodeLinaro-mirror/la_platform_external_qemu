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

#include <functional>
#include <memory>
#include <string>

#ifdef _WIN32
#undef ERROR
#endif

struct QAndroidTelephonyAgent;

// Enum representing the result status of a telephony operation.
enum class TelephonyResponseStatus { OK, RADIO_OFF, ERROR };

// Callback type for receiving operation results on the UI thread.
using TelephonyResultCallback = std::function<void(TelephonyResponseStatus)>;

// An abstract base class that defines the interface for telephony operations.
class TelephonyController {
public:
    static TelephonyController* get();
#ifdef ENABLE_QT_TESTS
    static void setForTest(std::unique_ptr<TelephonyController> controller);
    static void resetForTest();
#endif

    virtual ~TelephonyController() = default;

    // Initializes a new call to the emulator.
    virtual void initCallAsync(const std::string& number,
                               TelephonyResultCallback cb = nullptr) = 0;

    // Disconnects an existing call.
    virtual void disconnectCallAsync(const std::string& number,
                                     TelephonyResultCallback cb = nullptr) = 0;

    // Places an active call on hold.
    virtual void holdCallAsync(const std::string& number,
                               TelephonyResultCallback cb = nullptr) = 0;

    // Takes a held call off hold.
    virtual void unholdCallAsync(const std::string& number,
                                 TelephonyResultCallback cb = nullptr) = 0;

    // Sends an SMS message to the emulator.
    virtual void sendSmsAsync(const std::string& sender,
                              const std::string& message,
                              TelephonyResultCallback cb = nullptr) = 0;

    // Registers a callback to be notified of call state changes.
    // The callback receives the number of active calls.
    virtual void setCallStateCallback(
            std::function<void(int activeCalls)> callback) = 0;

    // Updates the modem time.
    virtual void updateTimeAsync(TelephonyResultCallback cb = nullptr) = 0;
};
