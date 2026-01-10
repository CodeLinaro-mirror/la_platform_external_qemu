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

#include <QCloseEvent>
#include <QEvent>
#include <QObject>      // for Q_OBJECT, slots
#include <QSpacerItem>  // for QSpacerItem
#include <QString>      // for QString
#include <QWidget>      // for QWidget
#include <memory>       // for shared_ptr, unique_ptr

#include "android/skin/qt/qt-ui-commands.h"
#include "ui_touchpad-holder.h"

namespace android {
namespace metrics {
class UiEventTracker;
}  // namespace metrics
}  // namespace android

class QObject;
class EmulatorQtWindow;
class ToolWindow;

class TouchpadHolder : public QWidget {
    Q_OBJECT

public:
    explicit TouchpadHolder(QWidget* parent = nullptr);

    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    bool handleQtKeyEvent(const QKeyEvent& event, QtKeyEventSource source);
    void setWidth(int width);

protected:
    std::unique_ptr<Ui::TouchpadHolder> mUi;

private:
    int mTouchpadWidth;
    int mTouchpadHeight;
    static constexpr int mMargin = 10;
};