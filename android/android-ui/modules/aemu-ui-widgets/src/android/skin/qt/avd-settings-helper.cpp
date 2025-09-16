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

#include "avd-settings-helper.h"

#include "android/avd/util.h"
#include "android/console.h"
#include "android/skin/qt/qt-settings.h"
#include "host-common/hw-config.h"

#include <QSettings>

QString getAvdSettingsFile() {
    const char* avdPath = path_getAvdContentPath(
            getConsoleAgents()->settings->hw()->avd_name);
    if (avdPath) {
        return QString(avdPath) + QString(Ui::Settings::PER_AVD_SETTINGS_NAME);
    } else {
        return {};
    }
}

QVariant getSavedSetting(const char* avdSpecificKey,
                         const char* globalKey,
                         const QVariant& defaultValue) {
    const QString avdSettingsFile = getAvdSettingsFile();
    if (avdSettingsFile.isEmpty()) {
        // Use the global settings if no AVD.
        QSettings settings;
        return settings.value(globalKey, defaultValue);
    } else {
        // Use the AVD specific key
        QSettings settings(avdSettingsFile, QSettings::IniFormat);
        return settings.value(avdSpecificKey, defaultValue);
    }
}

void saveSetting(const char* avdSpecificKey,
                 const char* globalKey,
                 const QVariant& value) {
    const QString avdSettingsFile = getAvdSettingsFile();
    if (avdSettingsFile.isEmpty()) {
        // Use the global settings if no AVD.
        QSettings settings;
        settings.setValue(globalKey, value);
    } else {
        // Use the AVD specific key
        QSettings settings(avdSettingsFile, QSettings::IniFormat);
        settings.setValue(avdSpecificKey, value);
    }
}
