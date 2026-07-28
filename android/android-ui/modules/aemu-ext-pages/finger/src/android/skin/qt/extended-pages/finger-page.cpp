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

#include "android/skin/qt/extended-pages/finger-page.h"

#include <QComboBox>
#include <QLabel>
#include <QVariant>
#include <Qt>

#include "aemu/base/async/Looper.h"
#include "aemu/base/async/ThreadLooper.h"
#include "android/avd/info.h"
#include "android/cmdline-definitions.h"
#include "android/console.h"
#include "android/emulation/control/finger_agent.h"
#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "android/metrics/UiEventTracker.h"
#include "android/metrics/studio_stats_wrapper.pb.h"
#include "android/skin/qt/extended-pages/grpc-finger-controller.h"
#include "android/skin/qt/extended-pages/legacy-finger-controller.h"

FingerPage::FingerPage(QWidget* parent)
    : QWidget(parent),
      mUi(new Ui::FingerPage()),
      mFingerTracker(new UiEventTracker(
              android_studio::EmulatorUiEvent::BUTTON_PRESS,
              android_studio::EmulatorUiEvent::EXTENDED_FINGER_TAB)) {
    mUi->setupUi(this);
    initializeController();

    int apiLevel = avdInfo_getApiLevel(getConsoleAgents()->settings->avdInfo());

    if (apiLevel >= 23) {
        mUi->finger_pickBox->addItem("Finger 1", 45146572);
        mUi->finger_pickBox->addItem("Finger 2", 192618075);
        mUi->finger_pickBox->addItem("Finger 3", 84807873);
        mUi->finger_pickBox->addItem("Finger 4", 189675793);
        mUi->finger_pickBox->addItem("Finger 5", 132710472);
        mUi->finger_pickBox->addItem("Finger 6", 36321043);
        mUi->finger_pickBox->addItem("Finger 7", 139425534);
        mUi->finger_pickBox->addItem("Finger 8", 15301340);
        mUi->finger_pickBox->addItem("Finger 9", 105702233);
        mUi->finger_pickBox->addItem("Finger 10", 87754286);

        mUi->finger_noFinger_mask->hide();
    } else {
        QString dessertName = avdInfo_getApiDessertName(apiLevel);
        if (!dessertName.isEmpty()) {
            dessertName = " (" + dessertName + ")";
        }

        QString badApi =
                tr("This emulated device is using API level %1%2.<br>"
                   "Fingerprint recognition is available with API level "
                   "23 (Marshmallow) and higher only.")
                        .arg(avdInfo_getApiLevelStr(getConsoleAgents()->settings->avdInfo()))
                        .arg(dessertName);

        mUi->finger_noFinger_mask->setTextFormat(Qt::RichText);
        mUi->finger_noFinger_mask->setText(badApi);
        mUi->finger_noFinger_mask->raise();
    }
}

FingerPage::~FingerPage() = default;

void FingerPage::initializeController() {
    if (getConsoleAgents()->settings->android_cmdLineOptions()->grpc_ui) {
        mController = std::make_unique<GrpcFingerController>();
    } else {
        mController = std::make_unique<LegacyFingerController>(
                getConsoleAgents()->finger);
    }
}

void FingerPage::on_finger_touchButton_pressed() {
    mFingerTracker->increment("FINGER");

    int fingerID = mUi->finger_pickBox->currentData().toInt();

    if (mController) {
        mController->sendTouchEvent(true, fingerID);
    }
}

void FingerPage::on_finger_touchButton_released() {
    if (mController) {
        mController->sendTouchEvent(false, 0);
    }
}
