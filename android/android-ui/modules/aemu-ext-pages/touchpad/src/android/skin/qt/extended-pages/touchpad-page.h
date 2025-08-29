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

#include <QEvent>
#include <QObject>      // for Q_OBJECT, slots
#include <QSpacerItem>  // for QSpacerItem
#include <QString>      // for QString
#include <QWidget>      // for QWidget
#include <memory>       // for shared_ptr, unique_ptr

#include "ui_touchpad-page.h"  // for TouchpadPage

namespace android {
namespace metrics {
class UiEventTracker;
}  // namespace metrics
}  // namespace android

class QObject;
class EmulatorQtWindow;

class TouchpadPage : public QWidget {
    Q_OBJECT

public:
    explicit TouchpadPage(QWidget* parent = 0);
    void setEmulatorWindow(EmulatorQtWindow* eW);

private slots:
    void on_tp_addSecondFinger_toggled(bool checked);

private:
    std::unique_ptr<Ui::TouchpadPage> mUi;
    QSpacerItem mButtonSpacer;
};