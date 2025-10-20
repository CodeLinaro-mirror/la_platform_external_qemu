#pragma once

#include "android/emulation/control/battery_agent.h" // For enums

// A simple struct to hold the battery state, mirroring the protobuf message.
struct BatteryState {
    int chargeLevel;
    BatteryCharger charger;
    BatteryHealth health;
    BatteryStatus status;
    bool hasBattery;
    bool isPresent;
};

// Interface for sending battery state to the emulator.
class BatteryController {
public:
    virtual ~BatteryController() = default;

    // Sends the full battery state to the emulator.
    virtual void setBattery(const BatteryState& state) = 0;
};
