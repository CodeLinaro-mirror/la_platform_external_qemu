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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

#include "aemu/base/Log.h"
#include "android/console.h"
#include "android/hw-sensors.h"
#include "host-common/hw-config.h"

namespace xr_service = android::xr::xr_service;

constexpr int kSliderScale = 1000;
void XrEnvironmentModeDialogHandleXrOptionsEvent(
        void* user_data,
        const xr_emulator_proto::EmulatorResponse& response);

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
        connect(this, &XrEnvironmentModeDialog::_externalXROptionsChanged, this,
                &XrEnvironmentModeDialog::onExternalXROptionsChanged);
        // Enable the slider once we have received the initial value from the
        // guest, and will be able to respond to changes
        ui->slider_xr_dimming_value->setEnabled(false);
        xrServiceCallbackHandle = xr_service::registerCallback(
                XrEnvironmentModeDialogHandleXrOptionsEvent, this);
    }
}

void XrEnvironmentModeDialogHandleXrOptionsEvent(
        void* user_data,
        const xr_emulator_proto::EmulatorResponse& response) {
    XrEnvironmentModeDialog* self =
            static_cast<XrEnvironmentModeDialog*>(user_data);
    if (response.response_case() !=
        xr_emulator_proto::EmulatorResponse::kXrOptions) {
        return;
    }

    // Only emit when either the dimming or passthrough coefficient is
    // changed, we dont want to notify when the environment mode changes
    const xr_emulator_proto::XrOptions& xr_options = response.xr_options();
    emit self->_externalXROptionsChanged(xr_options);
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
    emit onXrPassthroughCoefficientRequested(1.0f);
    accept();  // hides dialog
}

void XrEnvironmentModeDialog::on_btn_xr_environment_passthrough_off_clicked() {
    emit onXrPassthroughCoefficientRequested(0.0f);
    accept();  // hides dialog
}

void XrEnvironmentModeDialog::onExternalXROptionsChanged(
        xr_emulator_proto::XrOptions options) {
    // Enable the element once the listener is connected and we receive an
    // update with the initial value
    if (!ui->slider_xr_dimming_value->isEnabled()) {
        ui->slider_xr_dimming_value->setEnabled(true);
    }
    if (options.has_dimming_value()) {
        float val = options.dimming_value();
        int value = static_cast<int>(val * kSliderScale);

        // Update the UI to match the external state change
        {
            const QSignalBlocker blocker(ui->slider_xr_dimming_value);
            ui->slider_xr_dimming_value->setValue(value);
        }
        if (value != mLastSliderValue) {
            mLastSliderValue = value;
            mCurrentDiscreteValue = val;
            emit onXrDimmingValueRequested(mCurrentDiscreteValue, true);
        }
    }
}

XrEnvironmentModeDialog::~XrEnvironmentModeDialog() {
    xr_service::unregisterCallback(xrServiceCallbackHandle);
    delete ui;
}
