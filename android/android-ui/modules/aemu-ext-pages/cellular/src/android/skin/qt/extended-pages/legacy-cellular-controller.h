#pragma once

#include "android/skin/qt/extended-pages/cellular-controller.h"

struct QAndroidCellularAgent;

class LegacyCellularController : public CellularController {
public:
    explicit LegacyCellularController(const QAndroidCellularAgent* agent);
    void setCellular(const CellularState& state) override;

private:
    const QAndroidCellularAgent* mAgent;
    CellularState mCurrentState;  // To track changes
};
