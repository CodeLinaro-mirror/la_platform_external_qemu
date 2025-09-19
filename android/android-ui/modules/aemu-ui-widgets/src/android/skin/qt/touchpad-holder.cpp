// Copyright (C) 2025 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/skin/qt/touchpad-holder.h"
#include "aemu/base/Log.h"
#include "android/console.h"
#include "android/skin/qt/touchpad-widget.h"
#include "host-common/hw-config.h"

#include <QComboBox>
#include <QEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QVariant>
#include <QWidget>
#include <Qt>

TouchpadHolder::TouchpadHolder(QWidget* parent)
    : QWidget(parent),
      mUi(new Ui::TouchpadHolder()) {
    mUi->setupUi(this);

    constexpr int margin = 20;
    float touchpad_width =
            getConsoleAgents()->settings->hw()->hw_touchpad0_width;
    float touchpad_height =
            getConsoleAgents()->settings->hw()->hw_touchpad0_height;
    float allowed_width = this->size().width() - margin * 2;
    float allowed_height = this->size().height() - margin * 2;

    float scale = std::min(allowed_width / touchpad_width,
                           allowed_height / touchpad_height);
    mUi->touchpadBox->setFixedSize(scale * touchpad_width,
                                   scale * touchpad_height);
    mUi->touchpadBox->setScale(scale);
    mUi->touchpadBox->installEventFilter(this);

    // This will force the buttons to the left
    mUi->gridLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding), 2, 3);
}

void TouchpadHolder::on_tp_addSecondFinger_toggled(bool checked) {
    if (checked) {
        mUi->touchpadBox->setMultiFinger(2);
    } else {
        mUi->touchpadBox->setMultiFinger(1);
    }
}