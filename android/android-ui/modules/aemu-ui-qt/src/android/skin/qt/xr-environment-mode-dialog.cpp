// Copyright (C) 2024 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#include "xr-environment-mode-dialog.h"
#include "ui_xr-environment-mode-dialog.h"

#include <algorithm>
#include <cmath>

#include "aemu/base/Log.h"
#include "android/console.h"
#include "android/hw-sensors.h"
#include "host-common/hw-config.h"

constexpr int kSliderScale = 1000;

XrEnvironmentModeDialog::XrEnvironmentModeDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::XrEnvironmentModeDialog) {
    ui->setupUi(this);
    setWindowFlags(Qt::Popup);

    const char* levelsStr =
            getConsoleAgents()->settings->hw()->hw_dimmingLevels;
    if (levelsStr && levelsStr[0] != '\0') {
        QString qLevels = QString::fromUtf8(levelsStr);
        QStringList list = qLevels.split(',', Qt::SkipEmptyParts);
        for (const QString& s : list) {
            bool ok;
            float v = s.trimmed().toFloat(&ok);
            if (ok) {
                mDimmingLevels.push_back(v);
            }
        }
        std::sort(mDimmingLevels.begin(), mDimmingLevels.end());
    }

    if (mDimmingLevels.empty()) {
        ui->slider_xr_dimming_value->hide();
        this->adjustSize();
    } else {
        ui->slider_xr_dimming_value->setRange(0, kSliderScale);
    }
}
void XrEnvironmentModeDialog::setDimmingValue(float value) {
    mCurrentDiscreteValue = value;
    mLastSliderValue = static_cast<int>(value * kSliderScale);
    QSignalBlocker blocker(ui->slider_xr_dimming_value);
    ui->slider_xr_dimming_value->setValue(mLastSliderValue);
}

void XrEnvironmentModeDialog::on_slider_xr_dimming_value_valueChanged(
        int value) {
    if (mDimmingLevels.empty())
        return;

    float fVal = value / static_cast<float>(kSliderScale);
    float nextDiscreteValue = mCurrentDiscreteValue;

    if (fVal > mCurrentDiscreteValue) {
        // Dragging up: stay at current until next higher level is reached
        auto it = std::upper_bound(mDimmingLevels.begin(), mDimmingLevels.end(),
                                   fVal);
        if (it != mDimmingLevels.begin()) {
            nextDiscreteValue = *std::prev(it);
        }
    } else if (fVal < mCurrentDiscreteValue) {
        // Dragging down: stay at current until next lower level is reached
        auto it = std::lower_bound(mDimmingLevels.begin(), mDimmingLevels.end(),
                                   fVal);
        if (it != mDimmingLevels.end()) {
            nextDiscreteValue = *it;
        }
    }

    int snappedInt = static_cast<int>(nextDiscreteValue * kSliderScale);
    if (snappedInt != value) {
        QSignalBlocker blocker(ui->slider_xr_dimming_value);
        ui->slider_xr_dimming_value->setValue(snappedInt);
    }

    if (snappedInt != mLastSliderValue) {
        mLastSliderValue = snappedInt;
        mCurrentDiscreteValue = nextDiscreteValue;
        emit onXrDimmingValueRequested(mCurrentDiscreteValue);
    }
}

void XrEnvironmentModeDialog::on_btn_xr_environment_passthrough_on_clicked() {
    emit onXrEnvironmentModeRequested(XR_ENVIRONMENT_MODE_PASSTHROUGH_ON);
    accept();  // hides dialog
}

void XrEnvironmentModeDialog::on_btn_xr_environment_passthrough_off_clicked() {
    emit onXrEnvironmentModeRequested(XR_ENVIRONMENT_MODE_PASSTHROUGH_OFF);
    accept();  // hides dialog
}

XrEnvironmentModeDialog::~XrEnvironmentModeDialog() {
    delete ui;
}
