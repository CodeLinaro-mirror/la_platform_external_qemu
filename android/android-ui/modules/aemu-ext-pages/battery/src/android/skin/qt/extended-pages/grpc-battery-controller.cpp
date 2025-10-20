#include "android/skin/qt/extended-pages/grpc-battery-controller.h"

#include "emulator_controller.grpc.pb.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "android/utils/debug.h"

using android::emulation::control::EmulatorControlClient;
using android::emulation::control::EmulatorGrpcClient;

GrpcBatteryController::GrpcBatteryController() {
    mEmulatorControl = std::make_unique<EmulatorControlClient>(EmulatorGrpcClient::me());
}

GrpcBatteryController::GrpcBatteryController(
        std::shared_ptr<EmulatorControlClient> client)
    : mEmulatorControl(std::move(client)) {}

void GrpcBatteryController::setBattery(const BatteryState& state) {
    ::android::emulation::control::BatteryState pbState;
    pbState.set_chargelevel(state.chargeLevel);
    pbState.set_charger(
            static_cast<
                    ::android::emulation::control::BatteryState::BatteryCharger>(
                    state.charger));
    pbState.set_health(
            static_cast<
                    ::android::emulation::control::BatteryState::BatteryHealth>(
                    state.health));
    pbState.set_status(
            static_cast<
                    ::android::emulation::control::BatteryState::BatteryStatus>(
                    state.status));
    pbState.set_hasbattery(state.hasBattery);
    pbState.set_ispresent(state.isPresent);

    mEmulatorControl->setBatteryAsync(pbState);
}
