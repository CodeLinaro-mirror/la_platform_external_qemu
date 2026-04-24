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

constexpr int kSliderScale = 1000;

XrEnvironmentModeDialog::XrEnvironmentModeDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::XrEnvironmentModeDialog), mKeepRunning(true) {
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
        mListenerRegisterThread =
                std::thread(&XrEnvironmentModeDialog::register_listener, this);
    }
}

void XrEnvironmentModeDialog::register_listener() {
    while (mKeepRunning) {
        // Currently, this is not available when we initialize. The codepath is
        // being actively refactored, and the existing one can't be moved
        // earlier, so for now, wait for it to be available.
        auto notifier = static_cast<android::base::EventNotificationSupport<
                xr_emulator_proto::XrOptions>*>(
                android_get_xr_options_publisher());
        if (notifier) {
            mXROptionsListener =
                    std::make_unique<android::base::RaiiEventListener<
                            android::base::EventNotificationSupport<
                                    xr_emulator_proto::XrOptions>,
                            xr_emulator_proto::XrOptions>>(
                            notifier,
                            [this](xr_emulator_proto::XrOptions options) {
                                emit this->_externalXROptionsChanged(options);
                            });
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
    mKeepRunning = false;
    if (mListenerRegisterThread.joinable()) {
        mListenerRegisterThread.join();
    }
    // On reset, this unregisters the callback under lock. The callbacks on
    // events are handled under the same lock, so it is impossible to recieve
    // callbacks after the reset returns. This is important as the callback
    // would access ui.
    mXROptionsListener.reset();
    delete ui;
}
