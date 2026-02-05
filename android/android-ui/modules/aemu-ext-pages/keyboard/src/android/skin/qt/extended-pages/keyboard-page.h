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

#include <QPushButton>
#include <QWidget>
#include <memory>

#include "android/skin/keycode.h"
#include "ui_keyboard-page.h"

class EmulatorQtWindow;

class KeyboardPage : public QWidget {
    Q_OBJECT

public:
    explicit KeyboardPage(QWidget* parent = nullptr);
    void setEmulatorWindow(EmulatorQtWindow* eW);

private:
    void toggleKeyButtonDown(const SkinKeyCode key_code, const bool down);

    std::unique_ptr<Ui::KeyboardPage> mUi;
    EmulatorQtWindow* mEmulatorWindow;
};
