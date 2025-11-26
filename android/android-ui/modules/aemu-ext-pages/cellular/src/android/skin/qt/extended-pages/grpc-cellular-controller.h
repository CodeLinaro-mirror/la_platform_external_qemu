#pragma once

#include <memory>
#include "android/emulation/control/utils/ModemClient.h"
#include "android/skin/qt/extended-pages/cellular-controller.h"

class GrpcCellularController : public CellularController {
public:
    GrpcCellularController();
    // Constructor for testing purposes.
    explicit GrpcCellularController(
            std::shared_ptr<android::emulation::control::ModemClient>
                    client);
    void setCellular(const CellularState& state) override;

private:
    std::shared_ptr<android::emulation::control::ModemClient>
            mModemClient;
};
