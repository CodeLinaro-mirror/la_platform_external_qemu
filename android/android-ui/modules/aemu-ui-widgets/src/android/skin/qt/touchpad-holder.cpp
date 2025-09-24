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

    mTouchpadWidth = getConsoleAgents()->settings->hw()->hw_touchpad0_width;
    mTouchpadHeight = getConsoleAgents()->settings->hw()->hw_touchpad0_height;

    if (mTouchpadWidth > mTouchpadHeight) {
        mUi->horizontalSpacerLeft->changeSize(mMargin, 0, QSizePolicy::Minimum);
        mUi->horizontalSpacerRight->changeSize(mMargin, 0,
                                               QSizePolicy::Minimum);
        mUi->verticalSpacerTop->changeSize(0, mMargin, QSizePolicy::Minimum,
                                           QSizePolicy::Expanding);
        mUi->verticalSpacerBottom->changeSize(0, mMargin, QSizePolicy::Minimum,
                                              QSizePolicy::Expanding);

        auto policy = mUi->touchpadBox->sizePolicy();
        policy.setHorizontalPolicy(QSizePolicy::Expanding);
        policy.setVerticalPolicy(QSizePolicy::Minimum);
        policy.setHeightForWidth(true);
        mUi->touchpadBox->setSizePolicy(policy);
    } else {
        mUi->horizontalSpacerLeft->changeSize(mMargin, 0,
                                              QSizePolicy::Expanding);
        mUi->horizontalSpacerRight->changeSize(mMargin, 0,
                                               QSizePolicy::Expanding);
        mUi->verticalSpacerTop->changeSize(0, mMargin, QSizePolicy::Minimum,
                                           QSizePolicy::Expanding);
        mUi->verticalSpacerBottom->changeSize(0, mMargin, QSizePolicy::Minimum,
                                              QSizePolicy::Expanding);

        auto policy = mUi->touchpadBox->sizePolicy();
        policy.setHorizontalPolicy(QSizePolicy::Expanding);
        policy.setVerticalPolicy(QSizePolicy::Minimum);
        policy.setHeightForWidth(true);
        mUi->touchpadBox->setSizePolicy(policy);
    }

    mUi->touchpadBox->setTouchpadDimensions(mTouchpadWidth, mTouchpadHeight);
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