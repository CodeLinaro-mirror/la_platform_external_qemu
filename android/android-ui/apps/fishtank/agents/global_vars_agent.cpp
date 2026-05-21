// Copyright (C) 2025 The Android Open Source Project
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
#include "fishtank_agents.h"

#include "android/avd/info.h"
#include "android/cmdline-definitions.h"
#include "android/emulation/control/globals_agent.h"
#include "aemu/base/logging/Log.h"

#include <string>

// --- Global variables needed by the global vars agent ---
AndroidOptions emptyOptions{};
AndroidOptions* sAndroid_cmdLineOptions = &emptyOptions;
AvdInfo* sAndroid_avdInfo = nullptr;
AndroidHwConfig s_hwConfig = {};
AvdInfoParams sAndroid_avdInfoParams = {};
std::string sCmdlLine;
LanguageSettings s_languageSettings = {};
AUserConfig* s_userConfig = nullptr;
bool sKeyCodeForwarding = false;
bool sEnforceKeyCodeForwarding = false;
int s_guest_data_partition_mounted = 0;
bool s_guest_boot_completed = 0;
bool s_arm_snapshot_save_completed = 0;
bool s_host_emulator_is_headless = 0;
bool s_android_qemu_mode = true;
bool s_min_config_qemu_mode = false;
int s_android_snapshot_update_timer = 0;

// A mock implementation of the global vars agent.
// This is basically copied from MockAndroidAgentFactory.cpp
const QAndroidGlobalVarsAgent sFishtankQAndroidGlobalVarsAgent = {
        .avdParams = []() { return &sAndroid_avdInfoParams; },
        .avdInfo = []() { return sAndroid_avdInfo; },
        .hw = []() { return &s_hwConfig; },
        .guest_data_partition_mounted =
                []() { return s_guest_data_partition_mounted; },
        .guest_boot_completed = []() { return s_guest_boot_completed; },
        .arm_snapshot_save_completed =
                []() { return s_arm_snapshot_save_completed; },
        .host_emulator_is_headless =
                []() { return s_host_emulator_is_headless; },
        .android_qemu_mode = []() { return s_android_qemu_mode; },
        .min_config_qemu_mode = []() { return s_min_config_qemu_mode; },
        .is_fuchsia = []() { return s_min_config_qemu_mode; },
        .android_snapshot_update_timer =
                []() { return s_android_snapshot_update_timer; },
        .language = []() { return &s_languageSettings; },
        .use_keycode_forwarding =
                []() {
                    return sEnforceKeyCodeForwarding || sKeyCodeForwarding;
                },
        .userConfig = []() { return s_userConfig; },
        .android_cmdLineOptions = []() { return sAndroid_cmdLineOptions; },
        .inject_cmdLineOptions =
                [](AndroidOptions* opts) { sAndroid_cmdLineOptions = opts; },
        .has_cmdLineOptions =
                []() {
                    return sFishtankQAndroidGlobalVarsAgent
                                   .android_cmdLineOptions() != nullptr;
                },
        .android_cmdLine = []() { return (const char*)sCmdlLine.c_str(); },
        .inject_android_cmdLine =
                [](const char* cmdline) { sCmdlLine = cmdline; },
        .inject_language =
                [](char* language, char* country, char* locale) {
                    s_languageSettings.language = language;
                    s_languageSettings.country = country;
                    s_languageSettings.locale = locale;
                    s_languageSettings.changing_language_country_locale =
                            language || country || locale;
                },
        .inject_userConfig = [](AUserConfig* config) { s_userConfig = config; },
        .set_keycode_forwarding =
                [](bool enabled) { sKeyCodeForwarding = enabled; },
        .set_enforce_keycode_forwarding =
                [](bool enabled) { sEnforceKeyCodeForwarding = enabled; },
        .inject_AvdInfo = [](AvdInfo* avd) { sAndroid_avdInfo = avd; },
        .set_guest_data_partition_mounted =
                [](int mounted) { s_guest_data_partition_mounted = mounted; },
        .set_guest_boot_completed =
                [](bool completed) { s_guest_boot_completed = completed; },
        .set_arm_snapshot_save_completed =
                [](bool completed) {
                    s_arm_snapshot_save_completed = completed;
                },
        .set_host_emulator_is_headless =
                [](bool headless) { s_host_emulator_is_headless = headless; },
        .set_android_qemu_mode = [](bool mode) { s_android_qemu_mode = mode; },
        .set_min_config_qemu_mode =
                [](bool mode) { s_min_config_qemu_mode = mode; },
        .set_is_fuchsia =
                [](bool fuchsia) { s_min_config_qemu_mode = fuchsia; },
        .set_android_snapshot_update_timer =
                [](int timer) { s_android_snapshot_update_timer = timer; },
        .android_base_port = []() -> int { LOG(FATAL) << "QAndroidGlobalVarsAgent.android_base_port not implemented"; },
        .set_android_base_port = [](int port) { LOG(FATAL) << "QAndroidGlobalVarsAgent.set_android_base_port not implemented"; },
        .android_adb_port = []() -> int { LOG(FATAL) << "QAndroidGlobalVarsAgent.android_adb_port not implemented"; },
        .set_android_adb_port = [](int port) { LOG(FATAL) << "QAndroidGlobalVarsAgent.set_android_adb_port not implemented"; },
        .android_serial_number_port = []() -> int { LOG(FATAL) << "QAndroidGlobalVarsAgent.android_serial_number_port not implemented"; },
        .set_android_serial_number_port = [](int port) { LOG(FATAL) << "QAndroidGlobalVarsAgent.set_android_serial_number_port not implemented"; },
        .has_network_option = []() -> bool { LOG(FATAL) << "QAndroidGlobalVarsAgent.has_network_option not implemented"; }};
