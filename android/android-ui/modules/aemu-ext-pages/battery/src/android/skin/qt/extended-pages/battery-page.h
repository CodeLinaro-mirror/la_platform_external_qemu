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

#include "android/skin/qt/extended-pages/battery-controller.h"
#include "android/metrics/UiEventTracker.h"

namespace Ui {
class BatteryPage;
}
namespace android_studio {
class EmulatorUiEvent;
}

using android::metrics::UiEventTracker;
struct QAndroidBatteryAgent;

class BatteryPage : public QWidget {
    Q_OBJECT
public:
    explicit BatteryPage(QWidget* parent = nullptr);
    ~BatteryPage();

    // The battery agent is now passed in, not set statically.
    void setBatteryAgent(const QAndroidBatteryAgent* agent);
    void setControllerForTest(std::unique_ptr<BatteryController> controller);

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void on_bat_chargerBox_activated(int index);
    void on_bat_levelSlider_valueChanged(int value);
    void on_bat_healthBox_activated(int index);
    void on_bat_statusBox_activated(int index);

private:
    void populateListBox(
            class QComboBox* list,
            std::initializer_list<std::pair<int, const char*>> associations);
    void saveAndSendState();
    void updateUiFromState();
    void initializeController(const QAndroidBatteryAgent* agent);

    std::unique_ptr<Ui::BatteryPage> mUi;
    std::shared_ptr<UiEventTracker> mDropDownTracker;
    std::unique_ptr<BatteryController> mController;
    BatteryState mState;
    bool mInitialized = false;
};
