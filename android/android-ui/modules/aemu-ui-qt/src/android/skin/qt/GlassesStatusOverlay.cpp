// Copyright 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "aemu/base/Log.h"
#include "android/skin/qt/GlassesStatusOverlay.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QShowEvent>

#ifdef __APPLE__
#include "android/skin/qt/mac-native-window.h"  // for getNSWindow, nsWindow...
#endif

GlassesStatusOverlay::GlassesStatusOverlay(QWidget* parent)
    : QWidget(parent) {
    // "Tool" type windows live in another layer on top of everything in OSX,
    // which is undesirable because it means the extended window must be on top
    // of the emulator window. However, on Windows and Linux, "Tool" type
    // windows are the only way to make a window that does not have its own
    // taskbar item.
#ifdef __APPLE__
    Qt::WindowFlags flag = Qt::Dialog;
#else
    Qt::WindowFlags flag = Qt::Tool;
#endif

    setWindowFlags(flag | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);

    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);

    // Create a top-level vertical layout to hold the UI
    auto vLayout = new QVBoxLayout(this);
    vLayout->setContentsMargins(0, 0, 0, 0);

    // Create a frame for the rounded background
    auto backgroundFrame = new QFrame(this);
    backgroundFrame->setObjectName("GlassesStatusOverlayFrame");
    vLayout->addWidget(backgroundFrame);

    // Create the inner horizontal layout for the content
    auto innerLayout = new QHBoxLayout(backgroundFrame);
    innerLayout->setContentsMargins(8, 5, 8, 5);

    // Create the status message text label
    mTextLabel = new QLabel();
    QFont font = mTextLabel->font();
    font.setPixelSize(16);
    mTextLabel->setFont(font);
    innerLayout->addWidget(mTextLabel);

    backgroundFrame->setStyleSheet(
            "#GlassesStatusOverlayFrame {"
            "  background-color: #EBEBEB;"
            "  border-radius: 5px;"
            "}"
            "QLabel {"
            "  color: black;"
            "  background: transparent;"
            "}");

    hide();
}

void GlassesStatusOverlay::setStatusNoDisplay() {
    mShouldDisplay = true;
    mTextLabel->setText(tr("No Display"));
    adjustSize();
}

void GlassesStatusOverlay::showOverlay() {
    if (!mShouldDisplay) return;
    if (!mIsShown) {
        showNormal();
        mIsShown = true;
    }
    setWindowOpacity(1.0);
    LOG(DEBUG) << "Show glasses overlay";
}

void GlassesStatusOverlay::hideOverlay() {
    if (!mShouldDisplay) return;
    // We cannot use Show / Hide as this doesn't work for when the emulator in minimized.
    // See emulator-container.cpp for more details on this issue.
    setWindowOpacity(0.0);
    LOG(DEBUG) << "Hide glasses overlay";
}

void GlassesStatusOverlay::showEvent(QShowEvent* event) {
#ifdef __APPLE__
    // See EmulatorContainer::showEvent() for explanation on why this is needed
    WId parentWid = parentWidget()->effectiveWinId();
    parentWid = (WId)getNSWindow((void*)parentWid);

    WId wid = effectiveWinId();
    Q_ASSERT(wid && parentWid);
    wid = (WId)getNSWindow((void*)wid);
    nsWindowAdopt((void*)parentWid, (void*)wid);
#endif
    QWidget::showEvent(event);
}
