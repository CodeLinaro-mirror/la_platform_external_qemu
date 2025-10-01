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
    } else {
        mUi->horizontalSpacerLeft->changeSize(mMargin, 0,
                                              QSizePolicy::Expanding);
        mUi->horizontalSpacerRight->changeSize(mMargin, 0,
                                               QSizePolicy::Expanding);
        mUi->verticalSpacerTop->changeSize(0, mMargin, QSizePolicy::Minimum,
                                           QSizePolicy::Expanding);
        mUi->verticalSpacerBottom->changeSize(0, mMargin, QSizePolicy::Minimum,
                                              QSizePolicy::Expanding);
    }

    auto policy = mUi->touchpadBox->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    policy.setVerticalPolicy(QSizePolicy::Minimum);
    policy.setHeightForWidth(true);
    mUi->touchpadBox->setSizePolicy(policy);

    mUi->touchpadBox->setTouchpadDimensions(mTouchpadWidth, mTouchpadHeight);
    mUi->touchpadBox->installEventFilter(this);

    // This will force the buttons to the left
    mUi->gridLayout->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding), 2, 3);
}

void TouchpadHolder::setWidth(int width) {
    // Setting the width will resize the touchpad
    this->setFixedWidth(width);
    // Use the new touchpad height to set the window height
    this->setFixedHeight(mMargin * 2 + 2 * mUi->tp_addSecondFinger->height() +
                         mUi->touchpadBox->height());
}

bool TouchpadHolder::handleQtKeyEvent(const QKeyEvent& event,
                                      QtKeyEventSource source) {
    const bool down = event.type() == QEvent::KeyPress;
    // Trigger when the Shift key but no other modifiers is held.

    if (event.key() == Qt::Key_Shift) {
        if (down && mUi->touchpadBox->getMultiFinger() != 2 &&
            event.modifiers() == Qt::ShiftModifier) {
            mUi->tp_addSecondFinger->setChecked(true);
        } else if (!down && mUi->touchpadBox->getMultiFinger() == 2) {
            mUi->tp_addSecondFinger->setChecked(false);
        }
        return true;
    }

    // Look out for other modifier presses and cancel the capture.
    if (mUi->touchpadBox->getMultiFinger() == 2 && down &&
        event.modifiers() != Qt::ShiftModifier) {
        mUi->tp_addSecondFinger->setChecked(false);
    }

    return false;
}

void TouchpadHolder::keyPressEvent(QKeyEvent* e) {
    TouchpadHolder::handleQtKeyEvent(*e, QtKeyEventSource::TouchpadWindow);
}

void TouchpadHolder::keyReleaseEvent(QKeyEvent* e) {
    TouchpadHolder::handleQtKeyEvent(*e, QtKeyEventSource::TouchpadWindow);
}

void TouchpadHolder::on_tp_addSecondFinger_toggled(bool checked) {
    if (checked) {
        mUi->touchpadBox->setMultiFinger(2);
    } else {
        mUi->touchpadBox->setMultiFinger(1);
    }
}
