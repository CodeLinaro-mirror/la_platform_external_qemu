#include "android/skin/qt/extended-pages/grpc-cellular-controller.h"

#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "android/utils/debug.h"
#include "modem_service.pb.h"

using android::emulation::control::EmulatorGrpcClient;
using android::emulation::control::ModemClient;
using android::emulation::control::incubating::CellInfo;
using android::emulation::control::incubating::CellSignalStrength;

namespace {

CellInfo::CellStandard toProto(CellularStandard std) {
    switch (std) {
        case Cellular_Std_GSM:
            return CellInfo::CELL_STANDARD_GSM;
        case Cellular_Std_HSCSD:
            return CellInfo::CELL_STANDARD_HSCSD;
        case Cellular_Std_GPRS:
            return CellInfo::CELL_STANDARD_GPRS;
        case Cellular_Std_EDGE:
            return CellInfo::CELL_STANDARD_EDGE;
        case Cellular_Std_UMTS:
            return CellInfo::CELL_STANDARD_UMTS;
        case Cellular_Std_HSDPA:
            return CellInfo::CELL_STANDARD_HSDPA;
        case Cellular_Std_LTE:
            return CellInfo::CELL_STANDARD_LTE;
        case Cellular_Std_full:
            return CellInfo::CELL_STANDARD_FULL;
        case Cellular_Std_5G:
            return CellInfo::CELL_STANDARD_5G;
        default:
            return CellInfo::CELL_STANDARD_UNKNOWN;
    }
}

CellSignalStrength::CellSignalLevel toProto(CellularSignal signal) {
    switch (signal) {
        case Cellular_Signal_None:
            return CellSignalStrength::SIGNAL_STRENGTH_NONE_OR_UNKNOWN;
        case Cellular_Signal_Poor:
            return CellSignalStrength::SIGNAL_STRENGTH_POOR;
        case Cellular_Signal_Moderate:
            return CellSignalStrength::SIGNAL_STRENGTH_MODERATE;
        case Cellular_Signal_Good:
            return CellSignalStrength::SIGNAL_STRENGTH_GOOD;
        case Cellular_Signal_Great:
            return CellSignalStrength::SIGNAL_STRENGTH_GREAT;
        default:
            return CellSignalStrength::SIGNAL_STRENGTH_NONE_OR_UNKNOWN;
    }
}

CellInfo::CellStatus toProto(CellularStatus status) {
    switch (status) {
        case Cellular_Stat_Home:
            return CellInfo::CELL_STATUS_HOME;
        case Cellular_Stat_Roaming:
            return CellInfo::CELL_STATUS_ROAMING;
        case Cellular_Stat_Searching:
            return CellInfo::CELL_STATUS_SEARCHING;
        case Cellular_Stat_Denied:
            return CellInfo::CELL_STATUS_DENIED;
        case Cellular_Stat_Unregistered:
            return CellInfo::CELL_STATUS_UNREGISTERED;
        default:
            return CellInfo::CELL_STATUS_UNKNOWN;
    }
}

CellInfo::CellMeterStatus toProto(CellularMeterStatus status) {
    switch (status) {
        case Cellular_Metered:
            return CellInfo::CELL_METER_STATUS_METERED;
        case Cellular_Temporarily_Not_Metered:
            return CellInfo::CELL_METER_STATUS_TEMPORARILY_NOT_METERED;
        default:
            return CellInfo::CELL_METER_STATUS_UNKNOWN;
    }
}

}  // namespace

#define DEBUG 0
/* set  >1 for very verbose debugging */
#if DEBUG <= 1
#define DD(...) (void)0
#else
#define DD(...) dinfo(__VA_ARGS__)
#endif

GrpcCellularController::GrpcCellularController() {
    mModemClient = std::make_unique<ModemClient>(EmulatorGrpcClient::me());
}

GrpcCellularController::GrpcCellularController(
        std::shared_ptr<ModemClient> client)
    : mModemClient(std::move(client)) {}

void GrpcCellularController::setCellular(const CellularState& state) {
    CellInfo pbState;

    pbState.set_cell_standard(toProto(state.networkType));
    pbState.mutable_cell_signal_strength()->set_level(
            toProto(state.signalStrength));
    pbState.set_cell_status_voice(toProto(state.voiceStatus));
    pbState.set_cell_status_data(toProto(state.dataStatus));
    pbState.set_cell_meter_status(toProto(state.meterStatus));

    DD("Setting state to: %s", pbState.DebugString().c_str());

    mModemClient->setCellInfoAsync(pbState, [](auto _ignored) {});
}
