#include "android/skin/qt/extended-pages/legacy-battery-controller.h"

#include "aemu/base/Log.h"
#include "aemu/base/async/ThreadLooper.h"
#include "android/emulation/control/battery_agent.h"

#define DEBUG 2
/* set  >1 for very verbose debugging */
#if DEBUG <= 1
#define DD(...) (void)0
#else
#define DD(...) dinfo(__VA_ARGS__)
#endif

LegacyBatteryController::LegacyBatteryController(
        const QAndroidBatteryAgent* agent)
    : mAgent(agent) {
    // Initialize with a default state that will force an update on the first
    // setBattery call.
    mCurrentState = {-1,
                     BATTERY_CHARGER_NUM_ENTRIES,
                     BATTERY_HEALTH_NUM_ENTRIES,
                     BATTERY_STATUS_NUM_ENTRIES,
                     false,
                     false};
}

void LegacyBatteryController::setBattery(const BatteryState& state) {
    if (!mAgent) {
        DD("No battery agent present, ignoring");
        return;
    }

    DD("Scheduling update");
    android::base::ThreadLooper::runOnMainLooper([this, state]() {
        DD("Processing update..");
        if (mCurrentState.chargeLevel != state.chargeLevel &&
            mAgent->setChargeLevel) {
            DD("Setting charge level to: %d", state.chargeLevel);
            mAgent->setChargeLevel(state.chargeLevel);
        }
        if (mCurrentState.charger != state.charger && mAgent->setCharger) {
            DD("Setting charger to: %d", state.charger);
            mAgent->setCharger(state.charger);
        }
        if (mCurrentState.health != state.health && mAgent->setHealth) {
            DD("Setting health to: %d", state.health);
            mAgent->setHealth(state.health);
        }
        if (mCurrentState.status != state.status && mAgent->setStatus) {
            DD("Setting status to: %d", state.status);
            mAgent->setStatus(state.status);
        }
        // hasBattery and isPresent are not part of the legacy agent's setters.

        mCurrentState = state;
    });
}
