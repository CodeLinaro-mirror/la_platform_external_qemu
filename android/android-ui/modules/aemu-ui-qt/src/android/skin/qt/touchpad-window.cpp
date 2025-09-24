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

#include "android/skin/qt/touchpad-window.h"
#include <qwidget.h>
#include "aemu/base/Log.h"
#include "android/console.h"
#include "android/skin/qt/touchpad-holder.h"
#include "android/skin/qt/touchpad-widget.h"
#include "host-common/hw-config.h"

#include "android/skin/qt/emulator-qt-window.h"
#include "android/skin/qt/tool-window.h"
#include "android/skin/qt/touchpad-holder.h"

#include <QComboBox>
#include <QEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QSpacerItem>
#include <QVariant>
#include <QWidget>
#include <Qt>

TouchpadWindow::TouchpadWindow(QWidget* parent)
    : TouchpadWindow::TouchpadWindow(nullptr, nullptr, parent) {}

TouchpadWindow::TouchpadWindow(EmulatorQtWindow* emulatorWindow,
                               ToolWindow* toolWindow,
                               QWidget* parent)
    : TouchpadHolder(emulatorWindow ? emulatorWindow->containerWindow()
                                    : parent),
      mToolWindow(toolWindow),
      mEmulatorWindow(emulatorWindow) {
// If we want to be able to display as a docked window, we need to set some
// window flags "Tool" type windows live in another layer on top of everything
// in OSX, which is undesirable because it means the extended window must be on
// top of the emulator window. However, on Windows and Linux, "Tool" type
// windows are the only way to make a window that does not have its own taskbar
// item.
#ifdef __APPLE__
    Qt::WindowFlags flags = Qt::Dialog;
#else
    Qt::WindowFlags flags = Qt::Tool;
#endif
    flags |= Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint;
    this->setWindowFlags(flags);
    dockMainWindow();
}

void TouchpadWindow::dockMainWindow() {
    if (!mEmulatorWindow)
        return;

    const int parentWidgetWidth = parentWidget()->frameGeometry().width() -
                                  mEmulatorWindow->getLeftTransparency() -
                                  mEmulatorWindow->getRightTransparency();

    setWidth(parentWidgetWidth);
    move(parentWidget()->frameGeometry().left() +
                 mEmulatorWindow->getLeftTransparency(),
         parentWidget()->geometry().bottom() -
                 mEmulatorWindow->getBottomTransparency());
}

void TouchpadWindow::closeEvent(QCloseEvent* event) {
    // make sure the toolWindow's close button is the only exit point
    // from qt: we could get closeEvent directly from host windowing
    // framework.
    if (mEmulatorWindow && mEmulatorWindow->isMainThreadRunning() &&
        !mEmulatorWindow->toolWindow()->closeButtonClicked()) {
        mEmulatorWindow->toolWindow()->on_close_button_clicked();
        event->ignore();
        return;
    }
}

// If we're running docked, handle keypresses via the common path
void TouchpadWindow::keyPressEvent(QKeyEvent* e) {
    if (mToolWindow) {
        mToolWindow->handleQtKeyEvent(*e, QtKeyEventSource::TouchpadWindow);
    }
}

void TouchpadWindow::keyReleaseEvent(QKeyEvent* event) {
    if (mToolWindow) {
        mToolWindow->handleQtKeyEvent(*event, QtKeyEventSource::TouchpadWindow);
    }
}
