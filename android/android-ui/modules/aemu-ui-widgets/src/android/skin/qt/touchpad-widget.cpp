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
#include "android/console.h"
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

TrailRingBuffer::TrailRingBuffer(size_t maxSize)
    : mMaxSize(maxSize), mMinDiffSq(4), mBuffer(maxSize), mWeights(maxSize) {}

QPointF TrailRingBuffer::get(size_t index) const {
    Q_ASSERT(index < mMaxSize);
    size_t realIndex = (mStartIndex + index) % mMaxSize;
    return mBuffer[realIndex];
}

QPointF TrailRingBuffer::last() const {
    Q_ASSERT(mCurrentSize > 0);
    size_t lastIndex = (mStartIndex + mCurrentSize - 1) % mMaxSize;
    return mBuffer[lastIndex];
}

bool TrailRingBuffer::append(const QPointF& point) {
    size_t nextIndex = (mStartIndex + mCurrentSize) % mMaxSize;
    size_t lastIndex = (mStartIndex + mCurrentSize - 1) % mMaxSize;

    // Make room for the new point
    if (mCurrentSize == mMaxSize) {
        // Buffer is full, advance start index
        mAdjustedSize -= mWeights[mStartIndex];
        mStartIndex = (mStartIndex + 1) % mMaxSize;
        mCurrentSize--;
    }
    if (mAdjustedSize == mMaxSize) {
        mWeights[mStartIndex]--;
        mAdjustedSize--;
        if (mWeights[mStartIndex] == 0) {
            mStartIndex = (mStartIndex + 1) % mMaxSize;
            mCurrentSize--;
        }
    }

    if (mCurrentSize > 0 && mMinDiffSq > 0) {
        // If the new point is close to the last point, skip adding it
        QPointF lastPoint = mBuffer[lastIndex];
        QPointF diff = point - lastPoint;

        qreal distSq = diff.x() * diff.x() + diff.y() * diff.y();
        if (distSq < mMinDiffSq) {
            mWeights[lastIndex]++;
            mAdjustedSize++;
            return false;
        }
    }
    mBuffer[nextIndex] = point;
    mWeights[nextIndex] = 1;
    mCurrentSize++;
    mAdjustedSize++;
    return true;
}

void TrailRingBuffer::clear() {
    mAdjustedSize = 0;
    mStartIndex = 0;
    mCurrentSize = 0;
}

size_t TrailRingBuffer::size() const {
    return mCurrentSize;
}

size_t TrailRingBuffer::adjustedSize() const {
    return mAdjustedSize;
}

TouchpadWidget::TouchpadWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(50, 50);

    mNumFingers = 1;
    for (int i = 0; i < mMaxFingers; i++) {
        mTracking.append(false);
        mTrailPoints.append(TrailRingBuffer(mMaxTrailPoints));
    }
    mTouchpadWidth = 100;
    mTouchpadHeight = 100;
}

TouchpadWidget::~TouchpadWidget() {}

void TouchpadWidget::setTouchpadDimensions(int width, int height) {
    mTouchpadWidth = width;
    mTouchpadHeight = height;
}

float TouchpadWidget::getScale() const {
    return static_cast<float>(this->width()) /
           static_cast<float>(mTouchpadWidth);
}

int TouchpadWidget::heightForWidth(int width) const {
    return width * mTouchpadHeight / mTouchpadWidth;
}

bool TouchpadWidget::hasHeightForWidth() const {
    return true;
}

// Apparently the QT layout is not set up to respect heightForWidth
void TouchpadWidget::resizeEvent(QResizeEvent* event) {
    this->setFixedHeight(heightForWidth(this->width()));
    updatePixmaps();
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

    // add new fingers
    for (int i = mNumFingers; i < num_fingers; i++) {
        if (i > 0 && mTracking[i - 1]) {
            QPointF last_pos = mTrailPoints[i - 1].last();
            QPointF new_finger_pos =
                    last_pos + getScale() * mFingerSeperation * i;

            if (this->rect().contains(new_finger_pos.toPoint())) {
                doTouchBegin(new_finger_pos, i);
            }
        }
    }
    mNumFingers = num_fingers;
}

int TouchpadWidget::getMultiFinger() const {
    return mNumFingers;
}

void TouchpadWidget::mousePressEvent(QMouseEvent* event) {
    for (int i = 0; i < mNumFingers; i++) {
        QPointF current_finger_pos =
                event->position() + getScale() * mFingerSeperation * i;
        if (!mTracking[i]) {
            if (this->rect().contains(current_finger_pos.toPoint())) {
                doTouchBegin(current_finger_pos, i);
            }
        }
    }
}

void TouchpadWidget::mouseReleaseEvent(QMouseEvent* event) {
    for (int i = 0; i < mNumFingers; i++) {
        if (mTracking[i]) {
            doTouchEnd(i);
        }
    }
}

void TouchpadWidget::mouseMoveEvent(QMouseEvent* event) {
    for (int i = 0; i < mNumFingers; i++) {
        QPointF current_finger_pos =
                event->position() + i * getScale() * mFingerSeperation;
        if (this->rect().contains(current_finger_pos.toPoint())) {
            if (!mTracking[i]) {
                doTouchBegin(current_finger_pos, i);
            } else {
                doTouchUpdate(current_finger_pos, i);
            }
        } else if (mTracking[i]) {
            doTouchEnd(i);
            return;
        }
    }
}

void TouchpadWidget::addTrailPoint(QPointF p, int i) {
    if (mTrailPoints[i].append(p)) {
        update();
    }
}

void TouchpadWidget::clearTrailPoints(int i) {
    mTrailPoints[i].clear();
    update();
}

void TouchpadWidget::updatePixmaps() {
    mCachedScale = getScale();
    mCachedTheme = getSelectedTheme();

    // Determine color
    QColor fingerColor;
    switch (mCachedTheme) {
        case SETTINGS_THEME_DARK:
        case SETTINGS_THEME_STUDIO_DARK:
            fingerColor = mFingerColorDark;
            break;
        case SETTINGS_THEME_LIGHT:
        case SETTINGS_THEME_STUDIO_LIGHT:
        default:
            fingerColor = mFingerColorLight;
            break;
    }

    // Draw Glow Pixmap
    float radius = mCachedScale * mFingerGlow;
    int side = qCeil(radius * 2);
    mGlowPixmap = QPixmap(side, side);
    mGlowPixmap.fill(Qt::transparent);

    QPainter p(&mGlowPixmap);
    p.setRenderHint(QPainter::Antialiasing);
    QRadialGradient gradient(side / 2.0, side / 2.0, radius);
    gradient.setColorAt(0, fingerColor);
    gradient.setColorAt(1.0, Qt::transparent);
    p.setBrush(QBrush(gradient));
    p.setPen(Qt::NoPen);
    p.drawEllipse(0, 0, side, side);

    // Draw Finger Pixmap
    radius = getScale() * mFingerWidth * 0.5;
    side = qCeil(radius * 2);
    mFingerPixmap = QPixmap(side, side);
    mFingerPixmap.fill(Qt::transparent);

    QPainter p2(&mFingerPixmap);
    p2.setRenderHint(QPainter::Antialiasing);
    p2.setBrush(QBrush(fingerColor));
    p2.setPen(Qt::NoPen);
    p2.drawEllipse(0, 0, side, side);
}

void TouchpadWidget::drawPixmapAt(QPainter& painter,
                                  const QPixmap& pixmap,
                                  const QPointF& center) {
    if (pixmap.isNull()) {
        updatePixmaps();
    }
    QPointF drawPos =
            center - QPointF(pixmap.width() / 2.0, pixmap.height() / 2.0);
    painter.drawPixmap(drawPos, pixmap);
}

void TouchpadWidget::drawGlowAt(QPainter& painter, const QPointF& center) {
    drawPixmapAt(painter, mGlowPixmap, center);
}

void TouchpadWidget::drawFingerAt(QPainter& painter, const QPointF& center) {
    drawPixmapAt(painter, mFingerPixmap, center);
}

void TouchpadWidget::paintEvent(QPaintEvent* event) {
    if (mCachedScale != getScale() || mCachedTheme != getSelectedTheme()) {
        updatePixmaps();
    }
    QPainter painter(this);

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
        auto current_loc = mTrailPoints[i].last();

        drawGlowAt(painter, current_loc);

        if (mTrailPoints[i].size() < 2) {
            drawFingerAt(painter, current_loc);
            continue;
        }

        // Draw touch trail
        QPen pen;
        pen.setColor(fingerColor);
        pen.setWidth(getScale() * mFingerWidth);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        QPainterPath p(current_loc);
        for (size_t j = 1; j <= mTrailPoints[i].size(); ++j) {
            p.lineTo(mTrailPoints[i].get(mTrailPoints[i].size() - j));
        }
        painter.drawPath(p);
    }
}

void TouchpadWidget::doTouchBegin(QPointF p, int i) {
    doTouch(p, i, kEventTouchBegin);
    mTracking[i] = true;
}

void TouchpadWidget::doTouchUpdate(QPointF p, int i) {
    doTouch(p, i, kEventTouchUpdate);
}

void TouchpadWidget::doTouchEnd(int i) {
    doTouch(QPointF(0, 0), i, kEventTouchEnd);
    mTracking[i] = false;
}

void TouchpadWidget::doTouch(QPointF p, int i, SkinEventType type) {
    int x = p.x() / getScale();
    int y = (this->rect().height() - p.y()) / getScale();

    // Adjust for any rounding issue for touchpad dimensions
    x = std::min(x, mTouchpadWidth);
    y = std::min(y, mTouchpadHeight);
    SkinEvent skin_event = createSkinEvent(type);

    skin_event.u.multi_touch_point.id = i + 1;
    skin_event.u.multi_touch_point.x = x;
    skin_event.u.multi_touch_point.y = y;
    if (type == kEventTouchBegin || type == kEventTouchUpdate)
        skin_event.u.multi_touch_point.pressure = 0x400;

    getConsoleAgents()->user_event->sendTouchpadEvents(&skin_event, 0);

    if (type == kEventTouchBegin || type == kEventTouchUpdate) {
        addTrailPoint(p, i);
    } else {  // kEventTouchEnd
        clearTrailPoints(i);
    }
}