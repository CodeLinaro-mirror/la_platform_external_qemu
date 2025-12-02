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
#include "android/skin/qt/extended-pages/battery-page.h"

#include <qsettings.h>
#include <qstring.h>

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QSlider>
#include <QVariant>

#include "aemu/base/async/Looper.h"
#include "aemu/base/async/ThreadLooper.h"
#include "android/avd/info.h"
#include "android/avd/util.h"
#include "android/cmdline-definitions.h"
#include "android/console.h"
#include "android/emulation/control/battery_agent.h"
#include "android/metrics/UiEventTracker.h"
#include "android/skin/qt/extended-pages/grpc-battery-controller.h"
#include "android/skin/qt/extended-pages/legacy-battery-controller.h"
#include "android/skin/qt/qt-settings.h"
#include "android/utils/debug.h"
#include "ui_battery-page.h"

#define DEBUG 0
/* set  >1 for very verbose debugging */
#if DEBUG <= 1
#define DD(...) (void)0
#else
#define DD(...) dinfo(__VA_ARGS__)
#endif

class QComboBox;
class QWidget;

// Helper functions for saving/loading state to/from QSettings
static void saveChargeLevel(int chargeLevel);
static void saveCharger(BatteryCharger charger);
static void saveHealth(BatteryHealth health);
static void saveStatus(BatteryStatus status);

static int getSavedChargeLevel(bool allow_zero);
static BatteryCharger getSavedCharger();
static BatteryHealth getSavedHealth();
static BatteryStatus getSavedStatus();

static void saveBatteryState(const BatteryState& state) {
    saveChargeLevel(state.chargeLevel);
    saveCharger(state.charger);
    saveHealth(state.health);
    saveStatus(state.status);
}

static BatteryState loadBatteryState(bool allow_zero) {
    BatteryState state;
    state.hasBattery = true;
    state.isPresent = true;
    state.chargeLevel = getSavedChargeLevel(allow_zero);
    state.charger = getSavedCharger();
    state.health = getSavedHealth();
    state.status = getSavedStatus();
    return state;
}

#define STATE(p) \
    case (p):    \
        s = #p;  \
        break;

static std::string translate_idx(BatteryCharger value) {
    std::string s = "";
    switch (value) {
        STATE(BATTERY_CHARGER_NONE);
        STATE(BATTERY_CHARGER_AC);
        STATE(BATTERY_CHARGER_USB);
        STATE(BATTERY_CHARGER_WIRELESS);
        STATE(BATTERY_CHARGER_NUM_ENTRIES);
    }
    // Chop off BATTERY_
    return s.substr(8);
}
static std::string translate_idx(BatteryHealth value) {
    std::string s = "";
    switch (value) {
        STATE(BATTERY_HEALTH_GOOD);
        STATE(BATTERY_HEALTH_FAILED);
        STATE(BATTERY_HEALTH_DEAD);
        STATE(BATTERY_HEALTH_OVERVOLTAGE);
        STATE(BATTERY_HEALTH_OVERHEATED);
        STATE(BATTERY_HEALTH_UNKNOWN);
        STATE(BATTERY_HEALTH_NUM_ENTRIES);
    }
    // Chop off BATTERY_
    return s.substr(8);
}

static std::string translate_idx(BatteryStatus value) {
    std::string s = "";
    switch (value) {
        STATE(BATTERY_STATUS_UNKNOWN);
        STATE(BATTERY_STATUS_CHARGING);
        STATE(BATTERY_STATUS_DISCHARGING);
        STATE(BATTERY_STATUS_NOT_CHARGING);
        STATE(BATTERY_STATUS_FULL);
        STATE(BATTERY_STATUS_NUM_ENTRIES);
    }
    // Chop off BATTERY_
    return s.substr(8);
}

BatteryPage::BatteryPage(QWidget* parent)
    : QWidget(parent),
      mUi(new Ui::BatteryPage()),
      mDropDownTracker(std::make_shared<UiEventTracker>(
              android_studio::EmulatorUiEvent::OPTION_SELECTED,
              android_studio::EmulatorUiEvent::EXTENDED_BATTERY_TAB)) {
    DD("Creating the battery page!");
    mUi->setupUi(this);
    populateListBox(mUi->bat_chargerBox,
                    {
                            {BATTERY_CHARGER_NONE, "None"},
                            {BATTERY_CHARGER_AC, "AC charger"},
                    });
    populateListBox(mUi->bat_healthBox,
                    {
                            {BATTERY_HEALTH_GOOD, "Good"},
                            {BATTERY_HEALTH_FAILED, "Failed"},
                            {BATTERY_HEALTH_DEAD, "Dead"},
                            {BATTERY_HEALTH_OVERVOLTAGE, "Overvoltage"},
                            {BATTERY_HEALTH_OVERHEATED, "Overheated"},
                            {BATTERY_HEALTH_UNKNOWN, "Unknown"},
                    });
    populateListBox(mUi->bat_statusBox,
                    {
                            {BATTERY_STATUS_UNKNOWN, "Unknown"},
                            {BATTERY_STATUS_CHARGING, "Charging"},
                            {BATTERY_STATUS_DISCHARGING, "Discharging"},
                            {BATTERY_STATUS_NOT_CHARGING, "Not charging"},
                            {BATTERY_STATUS_FULL, "Full"},
                    });

    if (getConsoleAgents()->settings->hw()->hw_battery) {
        mState = loadBatteryState(false);
        mUi->bat_noBat_mask->hide();
        mUi->bat_noBat_message->hide();
    } else {
        mUi->bat_noBat_mask->raise();
        mUi->bat_noBat_message->raise();
    }
}

BatteryPage::~BatteryPage() = default;

void BatteryPage::setControllerForTest(
        std::unique_ptr<BatteryController> controller) {
    mController = std::move(controller);
    mInitialized = true;
}

void BatteryPage::showEvent(QShowEvent* event) {
    DD("Visibility changed");
    QWidget::showEvent(event);
    if (getConsoleAgents()->settings->hw()->hw_battery) {
        DD("Battery present!");
        setBatteryAgent(getConsoleAgents()->battery);
        mState = loadBatteryState(true);
        updateUiFromState();
    }
}

void BatteryPage::setBatteryAgent(const QAndroidBatteryAgent* agent) {
    DD("Setting battery agent to %p", agent);
    if (!getConsoleAgents()->settings->hw()->hw_battery || mInitialized) {
        DD("No battery agent or already initialized");
        return;
    }

    initializeController(agent);
    if (mController) {
        mController->setBattery(mState);
    }
    mInitialized = true;
}

void BatteryPage::initializeController(const QAndroidBatteryAgent* agent) {
    // Don't overwrite a controller that has been injected for testing.
    if (mController) {
        return;
    }
    if (getConsoleAgents()->settings->android_cmdLineOptions()->grpc_ui) {
        mController = std::make_unique<GrpcBatteryController>();
    } else {
        mController = std::make_unique<LegacyBatteryController>(agent);
    }
}

void BatteryPage::updateUiFromState() {
    int chargerIdx = mUi->bat_chargerBox->findData(mState.charger);
    if (chargerIdx < 0)
        chargerIdx = 0;

    int healthIdx = mUi->bat_healthBox->findData(mState.health);
    if (healthIdx < 0)
        healthIdx = 0;

    int statusIdx = mUi->bat_statusBox->findData(mState.status);
    if (statusIdx < 0)
        statusIdx = 0;

    mUi->bat_levelSlider->setValue(mState.chargeLevel);
    mUi->bat_chargeLevelText->setText(QString::number(mState.chargeLevel) +
                                      "%");
    mUi->bat_chargerBox->setCurrentIndex(chargerIdx);
    mUi->bat_healthBox->setCurrentIndex(healthIdx);
    mUi->bat_statusBox->setCurrentIndex(statusIdx);
}

void BatteryPage::populateListBox(
        QComboBox* list,
        std::initializer_list<std::pair<int, const char*>> associations) {
    list->clear();
    for (const auto& a : associations) {
        list->addItem(tr(a.second), a.first);
    }
}

void BatteryPage::saveAndSendState() {
    DD("Saving and sending state");
    if (mController) {
        DD("Sending state to controller");
        saveBatteryState(mState);
        mController->setBattery(mState);
    }
}

void BatteryPage::on_bat_chargerBox_activated(int index) {
    BatteryCharger bCharger = static_cast<BatteryCharger>(
            mUi->bat_chargerBox->itemData(index).toInt());

    if (bCharger >= 0 && bCharger < BATTERY_CHARGER_NUM_ENTRIES) {
        mState.charger = bCharger;
        mDropDownTracker->increment(translate_idx(bCharger));
        saveAndSendState();
    }
}

void BatteryPage::on_bat_levelSlider_valueChanged(int value) {
    mUi->bat_chargeLevelText->setText(QString::number(value) + "%");
    mDropDownTracker->increment("LEVEL_SLIDER");
    mState.chargeLevel = value;
    saveAndSendState();
}

void BatteryPage::on_bat_healthBox_activated(int index) {
    BatteryHealth bHealth = static_cast<BatteryHealth>(
            mUi->bat_healthBox->itemData(index).toInt());

    if (bHealth >= 0 && bHealth < BATTERY_HEALTH_NUM_ENTRIES) {
        mState.health = bHealth;
        mDropDownTracker->increment(translate_idx(bHealth));
        saveAndSendState();
    }
}

void BatteryPage::on_bat_statusBox_activated(int index) {
    BatteryStatus bStatus = static_cast<BatteryStatus>(
            mUi->bat_statusBox->itemData(index).toInt());

    if (bStatus >= 0 && bStatus < BATTERY_STATUS_NUM_ENTRIES) {
        mState.status = bStatus;
        mDropDownTracker->increment(translate_idx(bStatus));
        saveAndSendState();
    }
}

// QSettings implementation
static void saveChargeLevel(int chargeLevel) {
    int level = chargeLevel;
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        avdSpecificSettings.setValue(Ui::Settings::PER_AVD_BATTERY_CHARGE_LEVEL,
                                     level);
    } else {
        QSettings settings;
        settings.setValue(Ui::Settings::BATTERY_CHARGE_LEVEL, level);
    }
}

static void saveCharger(BatteryCharger charger) {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        avdSpecificSettings.setValue(
                Ui::Settings::PER_AVD_BATTERY_CHARGER_TYPE3, charger);
    } else {
        QSettings settings;
        settings.setValue(Ui::Settings::BATTERY_CHARGER_TYPE2, charger);
    }
}

static void saveHealth(BatteryHealth health) {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        avdSpecificSettings.setValue(Ui::Settings::PER_AVD_BATTERY_HEALTH,
                                     health);
    } else {
        QSettings settings;
        settings.setValue(Ui::Settings::BATTERY_HEALTH, health);
    }
}

static void saveStatus(BatteryStatus status) {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        avdSpecificSettings.setValue(Ui::Settings::PER_AVD_BATTERY_STATUS,
                                     status);
    } else {
        QSettings settings;
        settings.setValue(Ui::Settings::BATTERY_STATUS, status);
    }
}

static int getSavedChargeLevel(bool allow_zero) {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    int value;

    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        value = std::max<int>(
                avdSpecificSettings
                        .value(Ui::Settings::PER_AVD_BATTERY_CHARGE_LEVEL, 100)
                        .toInt(),
                0);
    } else {
        QSettings settings;
        value = std::max<int>(
                settings.value(Ui::Settings::BATTERY_CHARGE_LEVEL, 100).toInt(),
                0);
    }
    if (value == 0 && !allow_zero) {
        value = 1;
        // We always re-read the saved value, so we must save the adjusted value
        saveChargeLevel(value);
    }
    return value;
}

static BatteryCharger getSavedCharger() {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);

        int noMiscPipe = avdInfo_getApiLevel(
                                 getConsoleAgents()->settings->avdInfo()) < 26;

        BatteryCharger defaultCharger = BATTERY_CHARGER_NONE;

        if (noMiscPipe) {
            defaultCharger = BATTERY_CHARGER_AC;
        }

        return (BatteryCharger)avdSpecificSettings
                .value(Ui::Settings::PER_AVD_BATTERY_CHARGER_TYPE3,
                       defaultCharger)
                .toInt();
    } else {
        QSettings settings;
        return (BatteryCharger)settings
                .value(Ui::Settings::BATTERY_CHARGER_TYPE2,
                       BATTERY_CHARGER_NONE)
                .toInt();
    }
}

static BatteryHealth getSavedHealth() {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        return (BatteryHealth)avdSpecificSettings
                .value(Ui::Settings::PER_AVD_BATTERY_HEALTH,
                       BATTERY_HEALTH_GOOD)
                .toInt();
    } else {
        QSettings settings;
        return (BatteryHealth)settings
                .value(Ui::Settings::BATTERY_HEALTH, BATTERY_HEALTH_GOOD)
                .toInt();
    }
}

static BatteryStatus getSavedStatus() {
    BatteryCharger batteryCharger = getSavedCharger();
    BatteryStatus defaultBatteryStatus = (batteryCharger == BATTERY_CHARGER_AC)
                                                 ? BATTERY_STATUS_CHARGING
                                                 : BATTERY_STATUS_NOT_CHARGING;

    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        QString avdSettingsFile =
                avdPath + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
        QSettings avdSpecificSettings(avdSettingsFile, QSettings::IniFormat);
        return (BatteryStatus)avdSpecificSettings
                .value(Ui::Settings::PER_AVD_BATTERY_STATUS,
                       defaultBatteryStatus)
                .toInt();
    } else {
        QSettings settings;
        return (BatteryStatus)settings
                .value(Ui::Settings::BATTERY_STATUS, defaultBatteryStatus)
                .toInt();
    }
}
