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

#include "android/skin/qt/touchpad-widget.h"

#include "aemu/base/Log.h"
#include "android-qemu2-glue/emulation/virtio-input-multi-touch.h"
#include "android/skin/event.h"
#include "android/skin/qt/extended-pages/common.h"

#include "host-common/qt_ui_defs.h"

#include <QBrush>
#include <QColor>
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QWidget>

TouchpadWidget::TouchpadWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(50, 50);

    mNumFingers = 1;
    for (int i = 0; i < mMaxFingers; i++) {
        mTracking.append(false);
        mTrailPoints.append(QList<QPointF>());
    }
    mScale = 1;
}

TouchpadWidget::~TouchpadWidget() {}

void TouchpadWidget::setScale(float scale) {
    mScale = scale;
}

void TouchpadWidget::setMultiFinger(int num_fingers) {
    if (num_fingers > mMaxFingers) {
        num_fingers = mMaxFingers;
    } else if (num_fingers < 0) {
        num_fingers = 0;
    }

    // clear extra fingers
    for (int i = mNumFingers; i >= num_fingers; i--) {
        if (mTracking[i]) {
            doTouchEnd(i);
        }
    }
    mNumFingers = num_fingers;
}

void TouchpadWidget::mousePressEvent(QMouseEvent* event) {
    for (int i = 0; i < mNumFingers; i++) {
        if (!mTracking[i]) {
            mTracking[i] = true;
            doTouchBegin(event->position() + mFingerSeperation * i, i);
        }
    }
}

void TouchpadWidget::mouseReleaseEvent(QMouseEvent* event) {
    for (int i = 0; i < mNumFingers; i++) {
        if (mTracking[i]) {
            mTracking[i] = false;
            doTouchEnd(i);
        }
    }
}

void TouchpadWidget::mouseMoveEvent(QMouseEvent* event) {
    for (int i = 0; i < mNumFingers; i++) {
        QPointF current_finger_pos = event->position() + i * mFingerSeperation;
        if (this->rect().contains(current_finger_pos.toPoint())) {
            if (!mTracking[i]) {
                mTracking[i] = true;
                doTouchBegin(current_finger_pos, i);
                return;
            }
            doTouchUpdate(current_finger_pos, i);
        } else if (!this->rect().contains(current_finger_pos.toPoint()) &&
                   mTracking[i]) {
            mTracking[i] = false;
            doTouchEnd(i);
            return;
        }
    }
}

void TouchpadWidget::addTrailPoint(QPointF p, int i) {
    mTrailPoints[i].append(p);
    if (mTrailPoints[i].size() > mMaxTrailPoints) {
        mTrailPoints[i].pop_front();
    }
    update();
}

void TouchpadWidget::clearTrailPoints(int i) {
    mTrailPoints[i].clear();
    update();
}

void TouchpadWidget::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QPen borderPen;
    QColor fingerColor;
    SettingsTheme current_theme = getSelectedTheme();
    switch (getSelectedTheme()) {
        case SETTINGS_THEME_DARK:
        case SETTINGS_THEME_STUDIO_DARK:
            borderPen.setColor(QColor("#272727"));
            fingerColor = mFingerColorDark;
            break;
        case SETTINGS_THEME_LIGHT:
        case SETTINGS_THEME_STUDIO_LIGHT:
        default:
            borderPen.setColor(QColor("#d8d8d8"));
            fingerColor = mFingerColorLight;
            break;
    }

    borderPen.setWidth(2);
    painter.setPen(borderPen);
    painter.drawRect(this->rect().adjusted(1, 1, -1, -1));

    for (int i = 0; i < mNumFingers; i++) {
        if (mTrailPoints[i].size() < 1) {
            continue;
        }

        // Draw Current touch location
        auto current_loc = mTrailPoints[i][mTrailPoints[i].size() - 1];

        QRadialGradient gradient(current_loc, mScale * mFingerGlow);
        gradient.setColorAt(0, fingerColor);
        gradient.setColorAt(1.0, Qt::transparent);
        painter.setBrush(QBrush(gradient));
        painter.setPen(Qt::NoPen);

        painter.drawEllipse(current_loc, mScale * mFingerGlow, mScale * mFingerGlow);

        if (mTrailPoints[i].size() < 2) {
            painter.setBrush(QBrush(fingerColor));
            painter.drawEllipse(current_loc, mScale * mFingerWidth * 0.5, mScale * mFingerWidth * 0.5);
            continue;
        }

        // Draw touch trail
        QPen pen;
        pen.setColor(fingerColor);
        pen.setWidth(mScale * mFingerWidth);
        pen.capStyle();
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        QPainterPath p(current_loc);
        for (int j = 1; j <= mTrailPoints[i].size(); ++j) {
            p.lineTo(mTrailPoints[i][mTrailPoints[i].size() - j]);
        }
        painter.drawPath(p);
    }
}

void TouchpadWidget::doTouchBegin(QPointF p, int i) {
    doTouch(p, i, kEventTouchBegin);
}

void TouchpadWidget::doTouchUpdate(QPointF p, int i) {
    doTouch(p, i, kEventTouchUpdate);
}

void TouchpadWidget::doTouchEnd(int i) {
    doTouch(QPointF(0, 0), i, kEventTouchEnd);
}

void TouchpadWidget::doTouch(QPointF p, int i, SkinEventType type) {
    int x = p.x() / mScale;
    int y = (this->rect().height() - p.y()) / mScale;
    SkinEvent skin_event = createSkinEvent(type);

    skin_event.u.multi_touch_point.id = i + 1;
    skin_event.u.multi_touch_point.x = x;
    skin_event.u.multi_touch_point.y = y;

    if (type == kEventTouchBegin || type == kEventTouchUpdate) {
        addTrailPoint(p, i);
        skin_event.u.multi_touch_point.pressure = 0x400;
    } else {  // kEventTouchEnd
        clearTrailPoints(i);
    }

    android_virtio_touchpad_event(&skin_event, 0);
}