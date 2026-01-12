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

    this->setFocusPolicy(Qt::StrongFocus);

    mTouchpadWidth = getConsoleAgents()->settings->hw()->hw_touchpad0_width;
    mTouchpadHeight = getConsoleAgents()->settings->hw()->hw_touchpad0_height;
    mUi->gridLayout->setContentsMargins(mMargin, mMargin, mMargin, mMargin);

    auto policy = mUi->touchpadBox->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Expanding);
    policy.setVerticalPolicy(QSizePolicy::Minimum);
    policy.setHeightForWidth(true);
    mUi->touchpadBox->setSizePolicy(policy);

    mUi->touchpadBox->setTouchpadDimensions(mTouchpadWidth, mTouchpadHeight);
    mUi->touchpadBox->installEventFilter(this);

    mUi->tp_twoFingerLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    mUi->tp_twoFingerLabel->setAlignment(Qt::AlignCenter);
    mUi->tp_twoFingerLabel->lower();
    mUi->tp_twoFingerLabel->setGeometry(
            mUi->touchpadBox->pos().x(), mUi->touchpadBox->pos().y(),
            mUi->touchpadBox->width(), mUi->touchpadBox->height());
}

void TouchpadHolder::setWidth(int width) {
    int touchpadWidth = width - 2 * mMargin;
    int touchpadHeight = mUi->touchpadBox->heightForWidth(touchpadWidth);
    mUi->touchpadBox->setFixedWidth(touchpadWidth);
    this->setFixedSize(width, 2 * mMargin + touchpadHeight);
}

bool TouchpadHolder::handleQtKeyEvent(const QKeyEvent& event,
                                      QtKeyEventSource source) {
    const bool down = event.type() == QEvent::KeyPress;
    // Trigger when the Shift key but no other modifiers is held.

    if (event.key() == Qt::Key_Shift) {
        if (down && mUi->touchpadBox->getMultiFinger() != 2 &&
            event.modifiers() == Qt::ShiftModifier) {
            mUi->touchpadBox->setMultiFinger(2);
        } else if (!down && mUi->touchpadBox->getMultiFinger() == 2) {
            mUi->touchpadBox->setMultiFinger(1);
        }
        return true;
    }

    // Look out for other modifier presses and cancel the capture.
    if (mUi->touchpadBox->getMultiFinger() == 2 && down &&
        event.modifiers() != Qt::ShiftModifier) {
        mUi->touchpadBox->setMultiFinger(1);
    }

    return false;
}

void TouchpadHolder::keyPressEvent(QKeyEvent* e) {
    TouchpadHolder::handleQtKeyEvent(*e, QtKeyEventSource::TouchpadWindow);
}

void TouchpadHolder::keyReleaseEvent(QKeyEvent* e) {
    TouchpadHolder::handleQtKeyEvent(*e, QtKeyEventSource::TouchpadWindow);
}
