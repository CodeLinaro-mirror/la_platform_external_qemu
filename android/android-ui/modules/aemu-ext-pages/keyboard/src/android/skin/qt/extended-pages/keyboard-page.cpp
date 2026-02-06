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

#include "android/skin/qt/extended-pages/keyboard-page.h"
#include <QEvent>
#include <QPushButton>
#include "android/skin/event.h"
#include "android/skin/keycode.h"
#include "android/skin/qt/emulator-qt-window.h"

KeyboardPage::KeyboardPage(QWidget* parent)
    : QWidget(parent), mUi(new Ui::KeyboardPage) {
    mUi->setupUi(this);

    const struct {
        QPushButton* keyButton;
        SkinKeyCode key_code;
    } keyButtons[] = {
            {mUi->forwardButton, kKeyCodeForward},
            {mUi->refreshButton, kKeyCodeRefresh},
            {mUi->fullScreenButton, kKeyCodeFullScreen},
            {mUi->screenCaptureButton, kKeyCodeSelectiveScreenshot},
            {mUi->brightnessDownButton, kKeyCodeBrightnessDown},
            {mUi->brightnessUpButton, kKeyCodeBrightnessUp},
            {mUi->mediaPreviousButton, kKeyCodePreviousSong},
            {mUi->mediaPlayPauseButton, kKeyCodePlaypause},
            {mUi->mediaNextButton, kKeyCodeNextSong},
            {mUi->micMuteButton, kKeyCodeMicMute},
            {mUi->volumeMuteButton, kKeyCodeMute},
            {mUi->screenLockButton, kKeyCodeScreenLock},
    };

    for (const auto& button_info : keyButtons) {
        QPushButton* button = button_info.keyButton;
        const SkinKeyCode key_code = button_info.key_code;
        connect(button, &QPushButton::pressed,
                [key_code, this]() { toggleKeyButtonDown(key_code, true); });
        connect(button, &QPushButton::released,
                [key_code, this]() { toggleKeyButtonDown(key_code, false); });
    }
}

void KeyboardPage::setEmulatorWindow(EmulatorQtWindow* eW) {
    mEmulatorWindow = eW;
}

void KeyboardPage::toggleKeyButtonDown(const SkinKeyCode key_code,
                                       const bool down) {
    if (mEmulatorWindow) {
        SkinEvent skin_event =
                createSkinEvent(down ? kEventKeyDown : kEventKeyUp);
        skin_event.u.key.keycode = key_code;
        skin_event.u.key.mod = 0;
        mEmulatorWindow->queueSkinEvent(skin_event);
    }
}

void KeyboardPage::on_keyComboSendButton_clicked() {
    Qt::KeyboardModifiers mods;

    if (mUi->modifierCtrlButton->isChecked()) {
        mods |= Qt::ControlModifier;
    }
    if (mUi->modifierAltButton->isChecked()) {
        mods |= Qt::AltModifier;
    }
    if (mUi->modifierShiftButton->isChecked()) {
        mods |= Qt::ShiftModifier;
    }
    if (mUi->modifierMetaButton->isChecked()) {
        mods |= Qt::MetaModifier;
    }

    int keycode;
    QKeySequence keySequence = mUi->keyKaptureForm->keySequence();
    if (!keySequence.isEmpty()) {
        keycode = keySequence[0];
    }

    QKeyEvent event = QKeyEvent(QEvent::KeyPress, keycode, mods, nullptr);

    // The ideal behavior is sending the shortcut input to a virtual device
    // directly. Since the current implementation uses the public method of
    // EmulatorQtWindow, however, it can trigger the AEMU UI shortcuts depending
    // on feature flags and settings.
    // Bypassing the UI side shortcut handling will require refactoring of
    // EmulatorQtWindow.
    mEmulatorWindow->handleKeyEvent(kEventTextInput, event);
}
