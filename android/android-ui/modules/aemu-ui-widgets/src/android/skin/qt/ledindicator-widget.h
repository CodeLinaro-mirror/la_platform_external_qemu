// Copyright (C) 2026 The Android Open Source Project
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

#include <QColor>
#include <QImage>
#include <QPaintEvent>
#include <QPixmap>
#include <QWidget>

class LedIndicatorWidget : public QWidget {
    Q_OBJECT
public:
    explicit LedIndicatorWidget(QWidget* parent = nullptr);

    void setColor(uint32_t argb);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void updateCache(const QSize& size, qreal dpr);

    QColor mColor;
    QPixmap mMaskPixmap;  // Used as mask pixmap
    QPixmap mBorderPixmap;
    QImage mBufferImage;
    qreal mCachedDpr = 0.0;
};