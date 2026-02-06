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

#include "android/skin/qt/extended-pages/key-capture-form.h"
#include <QKeyEvent>
#include <QLineEdit>

KeyCaptureForm::KeyCaptureForm(QWidget* parent) : QKeySequenceEdit(parent) {
    setMaximumSequenceLength(1);

    // Override the default placeholder text "Press shortcut".
    QLineEdit* lineEdit = findChild<QLineEdit*>("qt_keysequenceedit_lineedit");
    if (lineEdit) {
        lineEdit->setPlaceholderText("Press key");
    }
}

bool KeyCaptureForm::event(QEvent* e) {
    // It disables switching the focus by Tab key and captures it as an input.
    if (e->type() == QEvent::KeyPress) {
        auto keyEvent = static_cast<QKeyEvent*>(e);
        if (keyEvent->key() == Qt::Key_Tab) {
            setKeySequence(QKeySequence(Qt::Key_Tab));
            keyEvent->accept();
            return true;
        }
    }

    return QKeySequenceEdit::event(e);
}
