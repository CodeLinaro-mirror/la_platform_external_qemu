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

#pragma once

#include "android/skin/event.h"

#include <QList>
#include <QPointF>
#include <QWidget>

class TouchpadWidget : public QWidget {
    Q_OBJECT

public:
    // Constructor
    explicit TouchpadWidget(QWidget* parent = nullptr);
    ~TouchpadWidget() override;

    void setScale(float scale);
    void setMultiFinger(int num_fingers);

    void doTouchBegin(QPointF p, int i);
    void doTouchEnd(int i);
    void doTouchUpdate(QPointF p, int i);

    void addTrailPoint(QPointF p, int i);
    void clearTrailPoints(int i);

protected:
    // This event is called whenever the mouse moves over the widget.
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

    // This event is called whenever the widget needs to be repainted.
    void paintEvent(QPaintEvent* event) override;

private:
    void doTouch(QPointF p, int i, SkinEventType type);

    int mNumFingers;
    float mScale;

    QList<bool> mTracking;
    QList<QList<QPointF>> mTrailPoints;

    // When duplicating fingers, they will be separated by this amount
    const QPointF mFingerSeperation = QPointF(200, 0);

    // How many points to store for the trail
    const int mMaxTrailPoints = 30;
    // Maximum number of duplicated fingers allowed
    const int mMaxFingers = 3;
};
