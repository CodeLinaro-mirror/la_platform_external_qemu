// Copyright (C) 2015 The Android Open Source Project
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

#include <QObject>           // for Q_OBJECT, slots
#include <QString>           // for QString
#include <QWidget>           // for QWidget
#include <memory>            // for shared_ptr, unique_ptr

#include "ui_finger-page.h"  // for FingerPage
#include "android/skin/qt/extended-pages/finger-controller.h"

namespace android {
namespace metrics {
class UiEventTracker;
}  // namespace metrics
}  // namespace android

using android::metrics::UiEventTracker;
class QObject;
struct QAndroidFingerAgent; // Keep for LegacyFingerController constructor

class FingerPage : public QWidget
{
    Q_OBJECT

public:
    explicit FingerPage(QWidget *parent = 0);
    ~FingerPage(); // Add destructor to properly manage unique_ptr

private slots:
    void on_finger_touchButton_pressed();
    void on_finger_touchButton_released();

private:
    void initializeController(); // Factory method for controller

    std::unique_ptr<Ui::FingerPage> mUi;
    std::shared_ptr<UiEventTracker> mFingerTracker;
    std::unique_ptr<FingerController> mController;
};
