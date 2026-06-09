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
#ifndef XRENVIRONMENTMODEDIALOG_H
#define XRENVIRONMENTMODEDIALOG_H

#include <QDialog>
#include <thread>

#include <vector>
#include "aemu/base/EventNotificationSupport.h"
#include "android/xr/XrService.h"
#include "xr_emulator_conn.pb.h"

namespace Ui {
class XrEnvironmentModeDialog;
}

class XrEnvironmentModeDialog : public QDialog {
    Q_OBJECT

public:
    explicit XrEnvironmentModeDialog(QWidget* parent = nullptr);
    ~XrEnvironmentModeDialog();

    void setDimmingValue(float value);

signals:
    void onXrEnvironmentModeRequested(int control);
    void onXrPassthroughCoefficientRequested(float value);
    void onXrDimmingValueRequested(float value, bool fromGuest = false);
    void _externalXROptionsChanged(xr_emulator_proto::XrOptions options);

private slots:
    void on_btn_xr_environment_passthrough_on_clicked();
    void on_btn_xr_environment_passthrough_off_clicked();
    void on_slider_xr_dimming_value_valueChanged(int value);
    // Signal handler for external changes
    void onExternalXROptionsChanged(xr_emulator_proto::XrOptions options);

private:
    Ui::XrEnvironmentModeDialog* ui;
    bool mShown = false;
    std::vector<float> mDimmingLevels;
    float mCurrentDiscreteValue = 0.0f;
    int mLastSliderValue = -1;
    void register_listener();
    android::xr::xr_service::Handle* xrServiceCallbackHandle =
            android::xr::xr_service::kInvalidHandleValue;

    friend void XrEnvironmentModeDialogHandleXrOptionsEvent(
            void* user_data,
            const xr_emulator_proto::EmulatorResponse& response);
};

#endif  // XRENVIRONMENTMODEDIALOG_H
