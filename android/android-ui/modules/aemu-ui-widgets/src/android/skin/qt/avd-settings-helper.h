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

#include <QString>
#include <QVariant>

// Get the per-avd settings file, if it exists
QString getAvdSettingsFile();

// Get the saved setting, or the |defaultValue| if not found.
// Prefer using the AVD setting via |avdSpecificKey| if the AVD setting file
// exists. If no AVD, use the global settings via |globalKey|.
QVariant getSavedSetting(const char* avdSpecificKey,
                         const char* globalKey,
                         const QVariant& defaultValue);

// Save the setting value.
// Prefer using the AVD setting via |avdSpecificKey| if the AVD setting file
// exists. If no AVD, use the global settings via |globalKey|.
void saveSetting(const char* avdSpecificKey,
                 const char* globalKey,
                 const QVariant& value);
