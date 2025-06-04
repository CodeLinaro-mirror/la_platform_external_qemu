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
#include "xr-input-mode-dialog.h"
#include "ui_xr-input-mode-dialog.h"

#include "android/hw-sensors.h"
#include "android/console.h"
#include "android/cmdline-definitions.h"

XrInputModeDialog::XrInputModeDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::XrInputModeDialog) {
    ui->setupUi(this);
    setWindowFlags(Qt::Popup);

    const AndroidOptions* opts = getConsoleAgents()->settings->android_cmdLineOptions();
    bool support_multi = opts->support_multiple_input_modalities;
    bool hands_in_space_enabled = opts->hands_in_space;

    if (hands_in_space_enabled) {
        // Hide eye, show only Mouse and Hand-raycast
        ui->btn_xr_input_eye_tacking->hide();
    } else if (support_multi) {
        // Show Mouse, Hand-raycast, and Eye
    } else {
        // Show only Mouse
        ui->btn_xr_input_hand_raycast->hide();
        ui->btn_xr_input_eye_tacking->hide();
    }
}

void XrInputModeDialog::on_btn_xr_input_keyboard_mouse_clicked() {
    emit onXrInputModeRequested(XR_INPUT_MODE_MOUSE_KEYBOARD);
    accept();  // hides dialog
}

void XrInputModeDialog::on_btn_xr_input_hand_raycast_clicked() {
    emit onXrInputModeRequested(XR_INPUT_MODE_HAND_RAYCAST);
    accept();  // hides dialog
}

void XrInputModeDialog::on_btn_xr_input_eye_tacking_clicked() {
    emit onXrInputModeRequested(XR_INPUT_MODE_EYE_TRACKING);
    accept();  // hides dialog
}

XrInputModeDialog::~XrInputModeDialog() {
    delete ui;
}
