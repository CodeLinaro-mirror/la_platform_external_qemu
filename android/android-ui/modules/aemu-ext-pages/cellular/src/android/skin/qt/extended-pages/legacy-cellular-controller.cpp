#include "android/skin/qt/extended-pages/legacy-cellular-controller.h"

#include "aemu/base/Log.h"
#include "aemu/base/async/ThreadLooper.h"
#include "android/emulation/control/cellular_agent.h"

#define DEBUG 0
/* set  >1 for very verbose debugging */
#if DEBUG <= 1
#define DD(...) (void)0
#else
#define DD(...) dinfo(__VA_ARGS__)
#endif

LegacyCellularController::LegacyCellularController(
        const QAndroidCellularAgent* agent)
    : mAgent(agent) {
    // Initialize with a default state that will force an update on the first
    // setCellular call.
    mCurrentState = {
        (CellularStandard)-1,
        (CellularSignal)-1,
        (CellularStatus)-1,
        (CellularStatus)-1,
        (CellularMeterStatus)-1,
    };
}

void LegacyCellularController::setCellular(const CellularState& state) {
    if (!mAgent) {
        DD("No cellular agent present, ignoring");
        return;
    }

    DD("Scheduling update");
    android::base::ThreadLooper::runOnMainLooper([this, state]() {
        DD("Processing update..");
        if (mCurrentState.networkType != state.networkType &&
            mAgent->setStandard) {
            DD("Setting network type to: %d", state.networkType);
            mAgent->setStandard(state.networkType);
        }
        if (mCurrentState.signalStrength != state.signalStrength &&
            mAgent->setSignalStrengthProfile) {
            DD("Setting signal strength to: %d", state.signalStrength);
            mAgent->setSignalStrengthProfile(state.signalStrength);
        }
        if (mCurrentState.voiceStatus != state.voiceStatus &&
            mAgent->setVoiceStatus) {
            DD("Setting voice status to: %d", state.voiceStatus);
            mAgent->setVoiceStatus(state.voiceStatus);
        }
        if (mCurrentState.dataStatus != state.dataStatus &&
            mAgent->setDataStatus) {
            DD("Setting data status to: %d", state.dataStatus);
            mAgent->setDataStatus(state.dataStatus);
        }
        if (mCurrentState.meterStatus != state.meterStatus &&
            mAgent->setMeterStatus) {
            DD("Setting meter status to: %d", state.meterStatus);
            mAgent->setMeterStatus(state.meterStatus);
        }

        mCurrentState = state;
    });
}
