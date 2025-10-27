// Copyright (C) 2015 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#pragma once

#include <QWidget>
#include <memory>

#include "android/skin/qt/extended-pages/cellular-controller.h"
#include "android/metrics/UiEventTracker.h"

namespace Ui {
class CellularPage;
}
namespace android_studio {
class EmulatorUiEvent;
}

using android::metrics::UiEventTracker;
struct QAndroidCellularAgent;

class CellularPage : public QWidget {
    Q_OBJECT

public:
    explicit CellularPage(QWidget* parent = nullptr);
    ~CellularPage();

    void setCellularAgent(const QAndroidCellularAgent* agent);
    void setControllerForTest(std::unique_ptr<CellularController> controller);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void on_cell_dataStatusBox_currentIndexChanged(int index);
    void on_cell_standardBox_currentIndexChanged(int index);
    void on_cell_voiceStatusBox_currentIndexChanged(int index);
    void on_cell_signalStatusBox_currentIndexChanged(int index);
    void on_cell_meterStatusBox_currentIndexChanged(int index);

private:
    void saveAndSendState();
    void updateUiFromState();
    void initializeController(const QAndroidCellularAgent* agent);

    void saveDataStatus(int status);
    void saveNetworkType(int type);
    void saveSignalStrength(int strength);
    void saveVoiceStatus(int status);
    void saveMeterStatus(int status);
    int getSavedDataStatus();
    int getSavedMeterStatus();
    int getSavedNetworkType();
    int getSavedSignalStrength();
    int getSavedVoiceStatus();

    std::unique_ptr<Ui::CellularPage> mUi;
    std::shared_ptr<UiEventTracker> mDropDownTracker;
    std::unique_ptr<CellularController> mController;
    CellularState mState;
    bool mInitialized = false;
};
