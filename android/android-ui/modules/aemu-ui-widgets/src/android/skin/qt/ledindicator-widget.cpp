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

#include "android/skin/qt/ledindicator-widget.h"
#include <QPainter>

LedIndicatorWidget::LedIndicatorWidget(QWidget* parent)
    : QWidget(parent), mColor(Qt::transparent) {}

void LedIndicatorWidget::setColor(uint32_t argb) {
    int r8 = (argb >> 16) & 0xFF;
    int g8 = (argb >> 8) & 0xFF;
    int b8 = argb & 0xFF;

    if (argb == 0 || (r8 == 0 && g8 == 0 && b8 == 0)) {
        mColor = Qt::transparent;
    } else {
        // Colored value: scale to full brightness and use max_val as alpha for
        // color -> transparency breathing
        int max_val = std::max({r8, g8, b8});
        int r_scaled = (r8 * 255) / max_val;
        int g_scaled = (g8 * 255) / max_val;
        int b_scaled = (b8 * 255) / max_val;
        mColor = QColor(r_scaled, g_scaled, b_scaled, max_val);
    }

    update();
}

void LedIndicatorWidget::updateCache(const QSize& size, qreal dpr) {
    if (mMaskPixmap.size() == size * dpr && mCachedDpr == dpr) {
        return;
    }
    mCachedDpr = dpr;

    // Create mask (white circle)
    mMaskPixmap = QPixmap(size * dpr);
    mMaskPixmap.setDevicePixelRatio(dpr);
    mMaskPixmap.fill(Qt::transparent);
    {
        QPainter painter(&mMaskPixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawEllipse(1, 1, size.width() - 2, size.height() - 2);
    }

    // Create border
    mBorderPixmap = QPixmap(size * dpr);
    mBorderPixmap.setDevicePixelRatio(dpr);
    mBorderPixmap.fill(Qt::transparent);
    {
        QPainter painter(&mBorderPixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Qt::gray, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(1, 1, size.width() - 2, size.height() - 2);
    }

    // Prepare temp image
    mBufferImage = QImage(size * dpr, QImage::Format_ARGB32_Premultiplied);
    mBufferImage.setDevicePixelRatio(dpr);
}

void LedIndicatorWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    updateCache(size(), devicePixelRatioF());

    mBufferImage.fill(Qt::transparent);
    {
        QPainter tmpPainter(&mBufferImage);
        QRect logicalRect = rect();

        tmpPainter.drawPixmap(0, 0, mMaskPixmap);

        // Fill with dark grey color on the background
        tmpPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tmpPainter.fillRect(logicalRect, QColor(140, 140, 140));

        // If the LED is lit or fading, composite its glowing color over the
        // dark grey bulb
        if (mColor != Qt::transparent && mColor.alpha() > 0) {
            tmpPainter.setCompositionMode(QPainter::CompositionMode_SourceAtop);
            tmpPainter.fillRect(rect(), mColor);
        }

        // Draw the border
        tmpPainter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        tmpPainter.drawPixmap(0, 0, mBorderPixmap);
    }

    painter.drawImage(0, 0, mBufferImage);
}
