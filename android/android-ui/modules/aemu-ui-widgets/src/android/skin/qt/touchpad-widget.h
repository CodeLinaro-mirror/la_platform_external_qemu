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

#include <QColor>
#include <QList>
#include <QPixmap>
#include <QPointF>
#include <QWidget>

// A ring buffer for the pointer trails.
// Nearby points are collapsed together when added to save on the path
// complexity.
class TrailRingBuffer {
public:
    explicit TrailRingBuffer(size_t maxSize);
    bool append(const QPointF& point);
    void clear();
    QPointF get(size_t index) const;
    QPointF last() const;
    size_t size() const;
    size_t adjustedSize() const;

private:
    QList<QPointF> mBuffer;
    QList<size_t> mWeights;
    size_t mAdjustedSize;
    size_t mMaxSize;
    size_t mStartIndex = 0;
    size_t mCurrentSize = 0;
    qreal mMinDiffSq;
};

class TouchpadWidget : public QWidget {
    Q_OBJECT

public:
    // Constructor
    explicit TouchpadWidget(QWidget* parent = nullptr);
    ~TouchpadWidget() override;

    void setScale(float scale);
    void setTouchpadDimensions(int width, int height);
    void setMultiFinger(int num_fingers);
    int getMultiFinger() const;

    void doTouchBegin(QPointF p, int i);
    void doTouchEnd(int i);
    void doTouchUpdate(QPointF p, int i);

    void addTrailPoint(QPointF p, int i);
    void clearTrailPoints(int i);
    int heightForWidth(int width) const override;

protected:
    // This event is called whenever the mouse moves over the widget.
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

    void resizeEvent(QResizeEvent* event) override;

    // This event is called whenever the widget needs to be repainted.
    void paintEvent(QPaintEvent* event) override;
    bool hasHeightForWidth() const override;

private:
    void doTouch(QPointF p, int i, SkinEventType type);
    float getScale() const;

    int mNumFingers;
    int mTouchpadWidth;
    int mTouchpadHeight;

    QList<bool> mTracking;
    QList<TrailRingBuffer> mTrailPoints;

    float mCachedScale = 1.0f;
    int mCachedTheme = -1;

    QPixmap mGlowPixmap;
    QPixmap mFingerPixmap;
    void updatePixmaps();
    void drawPixmapAt(QPainter& painter,
                      const QPixmap& pixmap,
                      const QPointF& center);
    void drawGlowAt(QPainter& painter, const QPointF& center);
    void drawFingerAt(QPainter& painter, const QPointF& center);

    // When duplicating fingers, they will be separated by this amount
    static constexpr QPointF mFingerSeperation = QPointF(520, 0);

    // How many points to store for the trail
    static constexpr int mMaxTrailPoints = 30;
    // Maximum number of duplicated fingers allowed
    static constexpr int mMaxFingers = 3;

    static constexpr QColor mFingerColorLight{0, 0, 200, 30};
    static constexpr QColor mFingerColorDark{50, 50, 250, 70};
    static constexpr int mFingerWidth = 200;
    static constexpr int mFingerGlow = 770;
};
