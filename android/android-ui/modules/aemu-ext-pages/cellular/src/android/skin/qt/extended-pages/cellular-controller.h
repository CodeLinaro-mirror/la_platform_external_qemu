#pragma once

#include "android/emulation/control/cellular_agent.h"
#include <string>
#include <sstream>

// Helper functions to convert enums to string
inline std::string to_string(CellularStandard val) {
    switch (val) {
        case Cellular_Std_GSM: return "GSM (0)";
        case Cellular_Std_HSCSD: return "HSCSD (1)";
        case Cellular_Std_GPRS: return "GPRS (2)";
        case Cellular_Std_EDGE: return "EDGE (3)";
        case Cellular_Std_UMTS: return "UMTS (4)";
        case Cellular_Std_HSDPA: return "HSDPA (5)";
        case Cellular_Std_LTE: return "LTE (6)";
        case Cellular_Std_full: return "full (7)";
        case Cellular_Std_5G: return "5G (8)";
        default: return "Unknown (" + std::to_string(val) + ")";
    }
}

inline std::string to_string(CellularSignal val) {
    switch (val) {
        case Cellular_Signal_None: return "None (0)";
        case Cellular_Signal_Poor: return "Poor (1)";
        case Cellular_Signal_Moderate: return "Moderate (2)";
        case Cellular_Signal_Good: return "Good (3)";
        case Cellular_Signal_Great: return "Great (4)";
        default: return "Unknown (" + std::to_string(val) + ")";
    }
}

inline std::string to_string(CellularStatus val) {
    switch (val) {
        case Cellular_Stat_Home: return "Home (0)";
        case Cellular_Stat_Roaming: return "Roaming (1)";
        case Cellular_Stat_Searching: return "Searching (2)";
        case Cellular_Stat_Denied: return "Denied (3)";
        case Cellular_Stat_Unregistered: return "Unregistered (4)";
        default: return "Unknown (" + std::to_string(val) + ")";
    }
}

inline std::string to_string(CellularMeterStatus val) {
    switch (val) {
        case Cellular_Metered: return "Metered (0)";
        case Cellular_Temporarily_Not_Metered: return "Temporarily Not Metered (1)";
        default: return "Unknown (" + std::to_string(val) + ")";
    }
}


// A struct that holds the state of the cellular settings.
struct CellularState {
    CellularStandard networkType;
    CellularSignal signalStrength;
    CellularStatus voiceStatus;
    CellularStatus dataStatus;
    CellularMeterStatus meterStatus;

    std::string toString() const {
        std::stringstream ss;
        ss << "CellularState {"
           << " networkType: " << to_string(networkType)
           << ", signalStrength: " << to_string(signalStrength)
           << ", voiceStatus: " << to_string(voiceStatus)
           << ", dataStatus: " << to_string(dataStatus)
           << ", meterStatus: " << to_string(meterStatus)
           << " }";
        return ss.str();
    }
};

// An abstract base class that defines the interface for setting the cellular
// state.
class CellularController {
public:
    virtual ~CellularController() = default;
    virtual void setCellular(const CellularState& state) = 0;
};
