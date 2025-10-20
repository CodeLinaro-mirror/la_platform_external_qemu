#pragma once

#include "android/skin/qt/extended-pages/battery-controller.h"

struct QAndroidBatteryAgent;

class LegacyBatteryController : public BatteryController {
public:
    explicit LegacyBatteryController(const QAndroidBatteryAgent* agent);
    void setBattery(const BatteryState& state) override;

private:
    const QAndroidBatteryAgent* mAgent;
    BatteryState mCurrentState;  // To track changes
};
