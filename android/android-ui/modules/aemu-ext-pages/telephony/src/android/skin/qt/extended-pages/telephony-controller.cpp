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

#include <mutex>

#include "android/cmdline-definitions.h"
#include "android/console.h"
#include "android/skin/qt/extended-pages/grpc-telephony-controller.h"
#include "android/skin/qt/extended-pages/legacy-telephony-controller.h"

#ifdef _WIN32
#undef ERROR
#endif

namespace {

class NoOpTelephonyController : public TelephonyController {
public:
    void initCallAsync(const std::string&,
                       TelephonyResultCallback cb) override {
        if (cb)
            cb(TelephonyResponseStatus::ERROR);
    }
    void disconnectCallAsync(const std::string&,
                             TelephonyResultCallback cb) override {
        if (cb)
            cb(TelephonyResponseStatus::ERROR);
    }
    void holdCallAsync(const std::string&,
                       TelephonyResultCallback cb) override {
        if (cb)
            cb(TelephonyResponseStatus::ERROR);
    }
    void unholdCallAsync(const std::string&,
                         TelephonyResultCallback cb) override {
        if (cb)
            cb(TelephonyResponseStatus::ERROR);
    }
    void sendSmsAsync(const std::string&,
                      const std::string&,
                      TelephonyResultCallback cb) override {
        if (cb)
            cb(TelephonyResponseStatus::ERROR);
    }
    void setCallStateCallback(std::function<void(int)>) override {}
    void updateTimeAsync(TelephonyResultCallback cb) override {
        if (cb)
            cb(TelephonyResponseStatus::ERROR);
    }
};

static NoOpTelephonyController sNoOpInstance;
static TelephonyController* sInstance = nullptr;
static std::mutex sMutex;

}  // namespace

// static
TelephonyController* TelephonyController::get() {
    std::lock_guard<std::mutex> lock(sMutex);
    if (!sInstance) {
        auto agents = getConsoleAgents();
        if (agents) {
            bool isGrpcUi = agents->settings &&
                            agents->settings->android_cmdLineOptions()->grpc_ui;

            if (isGrpcUi) {
                sInstance = new GrpcTelephonyController();
            } else if (agents->telephony) {
                sInstance = new LegacyTelephonyController(agents->telephony);
            } else {
                sInstance = new NoOpTelephonyController();
            }
        }
    }
    return sInstance ? sInstance : &sNoOpInstance;
}

#ifdef ENABLE_QT_TESTS
// static
void TelephonyController::setForTest(
        std::unique_ptr<TelephonyController> controller) {
    std::lock_guard<std::mutex> lock(sMutex);
    static std::unique_ptr<TelephonyController> sTestInstance;
    sTestInstance = std::move(controller);
    sInstance = sTestInstance.get();
}

// static
void TelephonyController::resetForTest() {
    std::lock_guard<std::mutex> lock(sMutex);
    sInstance = nullptr;
}
#endif
