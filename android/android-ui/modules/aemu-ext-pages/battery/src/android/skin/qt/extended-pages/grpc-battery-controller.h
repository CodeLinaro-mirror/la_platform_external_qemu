#pragma once

#include <memory>
#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "android/skin/qt/extended-pages/battery-controller.h"

class GrpcBatteryController : public BatteryController {
public:
    GrpcBatteryController();
    // Constructor for testing purposes.
    explicit GrpcBatteryController(
            std::shared_ptr<android::emulation::control::EmulatorControlClient>
                    client);
    void setBattery(const BatteryState& state) override;

private:
    std::shared_ptr<android::emulation::control::EmulatorControlClient>
            mEmulatorControl;
};