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

#include <thread>
#include "aemu/base/Debug.h"
#include "aemu/base/ProcessControl.h"
#include "aemu/base/files/PathUtils.h"
#include "aemu/base/process/Command.h"
#include "android/avd/info.h"
#include "android/base/system/System.h"
#include "android/cmdline-definitions.h"
#include "android/console.h"
#include "android/crashreport/crash-initializer.h"
#include "android/emulation/control/EmulatorAdvertisement.h"
#include "android/emulation/control/ScreenCapturer.h"
#include "android/emulation/control/interceptor/LoggingInterceptor.h"
#include "android/emulation/control/utils/EmulatorControlClient.h"
#include "android/emulation/control/utils/EmulatorGrcpClient.h"
#include "android/emulator-window.h"
#include "android/files/TemporaryFile.h"
#include "android/main-common-ui.h"
#include "android/main-common.h"
#include "android/opengl/gpuinfo.h"
#include "android/process_setup.h"
#include "android/qt/qt_path.h"
#include "android/skin/qt/QtLogger.h"
#include "android/skin/qt/SharedMemoryRenderer.h"
#include "android/skin/qt/emulator-qt-window.h"
#include "android/skin/qt/init-qt.h"
#include "android/utils/debug.h"
#include "android/utils/string.h"                           // for str_reset
#include "android/utils/system.h"
#include "fishtank_agents.h"
#include "grpc/FishtankGrpcServer.h"

#include <algorithm>
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_join.h"
#include "host-common/FeatureControl.h"
#include "host-common/feature_control.h"

using android::base::pj;
using android::base::System;
using android::emulation::control::EmulatorAdvertisement;

namespace fc = android::featurecontrol;

extern void injectFishtankConsoleAgents();

// HACK ATTACK
extern "C" void emulator_window_setup(EmulatorWindow* emulator);
extern void skin_winsys_setup_library_paths();
extern void android_set_external_renderer_active(bool active);
extern void myMessageOutput(QtMsgType type,
                            const QMessageLogContext& context,
                            const QString& msg);
extern "C" void emulator_window_refresh(EmulatorWindow* emulator);

using android::control::interceptor::StdOutLoggingInterceptorFactory;

AndroidOptions sOpts[1];

void messagePump(int, char**) {
    auto window = emulator_window_get();
    while (!window->done) {
        // Pump events..
        emulator_window_refresh(window);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::shared_ptr<android::emulation::control::EmulatorControlClient>
        gControlClient;

std::shared_ptr<android::emulation::control::EmulatorControlClient>
getGlobalControlClient() {
    return gControlClient;
}

int main(int argc, char* argv[]) {
    base_configure_logs(kLogDefaultOptions);

    auto system = System::get();
    std::string launcherDir = system->envGet("ANDROID_EMULATOR_LAUNCHER_DIR");
    if (launcherDir.empty()) {
        LOG(WARNING) << "ANDROID_EMULATOR_LAUNCHER_DIR is not defined. You "
                        "must've started "
                     << "fishtank as a standalone application. Will try to "
                        "deduce the launcher "
                     << "directory from ANDROID_SDK_ROOT.";
        std::string sdkRoot = system->envGet("ANDROID_SDK_ROOT");
        if (!sdkRoot.empty()) {
            // Default to the qemu-now emulator launcher for now.
            launcherDir = pj(sdkRoot, "emulator");
            system->setEnvironmentVariable("ANDROID_EMULATOR_LAUNCHER_DIR",
                                           launcherDir);
            LOG(INFO) << "Inferred ANDROID_EMULATOR_LAUNCHER_DIR from "
                         "ANDROID_SDK_ROOT: "
                      << launcherDir;
        } else {
            // ANDROID_EMULATOR_LAUNCHER_DIR is critical to resolve things like
            // the advancedFeatures.ini and maps.key file. Without those things,
            // fishtank will likely misbehave, which is why we decide to make it
            // a fatal error.
            LOG(FATAL) << "Neither ANDROID_EMULATOR_LAUNCHER_DIR nor "
                          "ANDROID_SDK_ROOT is defined. Cannot start fishtank.";
        }
    } else {
        LOG(INFO) << "Using ANDROID_EMULATOR_LAUNCHER_DIR: " << launcherDir;
    }

    process_early_setup(argc, argv);
    // crashhandler_init(argc, argv);
    async_query_host_gpu_start();

    const char* executable = argv[0];
    // QtWebEngine requires the executable name in argv, so let's save it here,
    // as argv is modified below.
    char* qt_argv = argv[0];
    int qt_argc = 1;
    injectFishtankConsoleAgents();

    // ParameterList params(argc, argv);
    getConsoleAgents()->settings->inject_android_cmdLine(
            android::base::createEscapedLaunchString(argc, argv).c_str());

    AndroidOptions* opts = &sOpts[0];
    AndroidHwConfig* hw = getConsoleAgents()->settings->hw();
    AvdInfo* avd;
    int inAndroidBuild = 0;
    int exitStatus;
    if (!emulator_parseCommonCommandLineOptions(&argc, &argv, "arm64",
                                                true,  // is_qemu2
                                                opts, hw, &avd, &exitStatus)) {
        LOG(FATAL) << "Failed";
    }

    // uncomment when grpc engine ready.
    opts->grpc_ui = true;
    getConsoleAgents()->settings->inject_AvdInfo(avd);

    const UiEmuAgent uiEmuAgent = {
            getConsoleAgents()->automation,
            getConsoleAgents()->battery,
            getConsoleAgents()->cellular,
            getConsoleAgents()->clipboard,
            getConsoleAgents()->display,
            getConsoleAgents()->emu,
            getConsoleAgents()->finger,
            getConsoleAgents()->location,
            getConsoleAgents()->proxy,
            getConsoleAgents()->record,
            getConsoleAgents()->sensors,
            getConsoleAgents()->telephony,
            getConsoleAgents()->user_event,
            getConsoleAgents()->virtual_scene,
            getConsoleAgents()->car,
            getConsoleAgents()->multi_display,
            nullptr  // For now there's no uses of SettingsAgent, so we
                     //          // don't set it.
    };

    if (!emulator_parseUiCommandLineOptions(opts, avd, hw)) {
        LOG(FATAL) << "Bad news bears, unable to init ui";
    }

    android::emulation::control::EmulatorGrpcClient::Builder builder;

    if (VERBOSE_CHECK(grpc)) {
        builder.withInterceptor(new StdOutLoggingInterceptorFactory());
    }

    std::string discovery = opts->fishtank;

    // 3 Options:

    // -- "default": Pick the first running emulator
    // -- "<serial>": Find the emulator with the given serial port (e.g. 5554)
    // -- "file.ini": Discovery file to use.
    int qemu_serial = 0;
    if (absl::SimpleAtoi(discovery, &qemu_serial)) {
        EmulatorAdvertisement adv({});

        // We are willing to wait 5 seconds for the discovery file.
        auto start = absl::Now();
        auto deadline = start + absl::Seconds(5);
        std::string possibleDiscoveryFile =
                adv.discoverEmulatorWithProperties({{"port.serial", discovery}});
        while (possibleDiscoveryFile.empty() && absl::Now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            possibleDiscoveryFile = adv.discoverEmulatorWithProperties(
                    {{"port.serial", discovery}});
        }

        if (possibleDiscoveryFile.empty()) {
            LOG(FATAL) << "The discovery file for serial " << discovery
                       << " was not found after " << (absl::Now() - start);
        }

        discovery = possibleDiscoveryFile;
    } else if ("default" == discovery) {
        EmulatorAdvertisement adv({});
        auto emulators = adv.discoverRunningEmulators();
        std::vector<std::string> discovery_files;
        std::copy_if(emulators.begin(), emulators.end(),
                     std::back_inserter(discovery_files),
                     [](auto str) { return absl::EndsWith(str, ".ini"); });

        if (discovery_files.empty()) {
            LOG(FATAL) << "No running emulators were found";
        }

        LOG(INFO) << "Discovered: " << absl::StrJoin(discovery_files, ",");
        discovery = discovery_files[0];
    }

    auto status = builder.withDiscoveryFile(discovery).build();

    if (!status.ok()) {
        LOG(FATAL) << "Failed to discover emulator due to "
                   << status.status().ToString();
        return 1;
    }
    android::emulation::control::EmulatorGrpcClient::configureMe(
            std::move(status.value()));
    bool connect = android::emulation::control::EmulatorGrpcClient::me()
                           ->hasOpenChannel();
    if (!connect) {
        LOG(FATAL) << "Failed to connect to emulator";
    }

    gControlClient = std::make_shared<
            android::emulation::control::EmulatorControlClient>(
            android::emulation::control::EmulatorGrpcClient::me());
    initializeGrpcUserEventAgent(gControlClient.get());

    auto program_dir = System::get()->getProgramDirectory();

    // Fishtank UI does not use any Vulkan
    fc::setEnabledOverride(fc::Vulkan, false);
    // Fishtank UI only uses/ships with swiftshader GL library.
    // Map null/auto/empty gpu mode to 'swiftshader_indirect'
    if (!hw->hw_gpu_mode || !strcmp(hw->hw_gpu_mode, "") || !strcmp(hw->hw_gpu_mode, "auto")) {
        str_reset(&hw->hw_gpu_mode, "swiftshader_indirect");
    }

    android_set_external_renderer_active(true);

    // Needs to be called before compatibility checks to correctly control
    // if hw gpu is going to be used
    RendererConfig rendererConfig;
    if (!configureRenderer(WINSYS_GLESBACKEND_PREFERENCE_SWIFTSHADER_DEPRECATED, &rendererConfig)) {
        derror("Error: could not configure renderer!");
    }

    if (!strcmp(hw->hw_gpu_mode, "swiftshader_indirect") ||
        !strcmp(hw->hw_gpu_mode, "swiftshader")) {
        // Use the swiftshader libraries bundled with fishtank.
        // We add this search path right after configureRenderer, so we can override the search
        // path added by that call.
        auto swiftshader_dir = pj({program_dir, "lib64", "gles_swiftshader"});
        LOG(INFO) << "Adding swiftshader library search path: " << swiftshader_dir;
        add_library_search_dir(swiftshader_dir.c_str());
    }

    if (!startRenderer(&rendererConfig)) {
        derror("Could not start renderer");
    }

    // Set Environment variables that get picked up by qt_path.cpp, which in
    // turn, will set environment variables for Qt to determine the path to the
    // plugins, qtwebengine stuff, etc.

    auto qt_base_dir = pj({program_dir, "lib64", "qt"});
#ifdef _WIN32
    // For windows, we install all the Qt core dlls into the same directory with
    // fishtank.exe.
    auto qt_lib_path = program_dir;
    // QtWebEngineProcess.exe lives in qt/bin.
    auto qt_process_path = pj({qt_base_dir, "bin", "QtWebEngineProcess.exe"});
    // Prepend the program directory to the PATH so that child processes (like
    // QtWebEngineProcess) can find the Qt DLLs that are located in the same
    // directory as the main executable.
    // We only need to do this for Windows because on linux/mac, we use rpaths.
    std::string currentPath = system->envGet("PATH");
    system->setEnvironmentVariable("PATH", program_dir + ";" + currentPath);
    LOG(INFO) << "Prepended " << program_dir << " to PATH";
#else
    // For mac/linux, Qt core libraries installed at lib64/qt/lib.
    auto qt_lib_path = pj(qt_base_dir, "lib");
    // QtWebEngineProcess lives in qt/libexec.
    auto qt_process_path = pj({qt_base_dir, "libexec", "QtWebEngineProcess"});
#endif
    auto qt_plugin_path = pj(qt_base_dir, "plugins");
    auto qt_resources_path = pj(qt_base_dir, "resources");
    auto qt_locales_path =
            pj({qt_base_dir, "translations", "qtwebengine_locales"});

    System::get()->setEnvironmentVariable("ANDROID_QT_LIB_PATH", qt_lib_path);
    System::get()->setEnvironmentVariable("QTWEBENGINEPROCESS_PATH",
                                          qt_process_path);
    System::get()->setEnvironmentVariable("ANDROID_QT_QPA_PLATFORM_PLUGIN_PATH",
                                          qt_plugin_path);
    System::get()->setEnvironmentVariable("QTWEBENGINE_RESOURCES_PATH",
                                          qt_resources_path);
    System::get()->setEnvironmentVariable("QTWEBENGINE_LOCALES_PATH",
                                          qt_locales_path);

    LOG(INFO) << "Setting ANDROID_QT_LIB_PATH to: " << qt_lib_path;
    LOG(INFO) << "Setting QTWEBENGINEPROCESS_PATH to: " << qt_process_path;
    LOG(INFO) << "Setting ANDROID_QT_QPA_PLATFORM_PLUGIN_PATH to: "
              << qt_plugin_path;
    LOG(INFO) << "Setting QTWEBENGINE_RESOURCES_PATH to: " << qt_resources_path;
    LOG(INFO) << "Setting QTWEBENGINE_LOCALES_PATH to: " << qt_locales_path;

    skin_winsys_init_args(qt_argc, &qt_argv);
    if (!emulator_initUserInterface(opts, &uiEmuAgent)) {
        dwarning("%s: user interface init failed", __func__);
        return 1;
    }

    // Start gRPC service for UI controller
    android::fishtank::FishtankGrpcServer fishtankGrpc;
    int preferredPort = 0;
    if (opts->grpc) {
        if (!absl::SimpleAtoi(opts->grpc, &preferredPort)) {
            LOG(WARNING) << "Invalid gRPC port specified: " << opts->grpc
                         << ". Using default: " << 8554;
        }
    }

    absl::Status grpcStatus = fishtankGrpc.setup(preferredPort);
    if (grpcStatus.ok()) {
        auto registerStatus = fishtankGrpc.registerUiController(
                android::emulation::control::EmulatorGrpcClient::me().get());
        if (!registerStatus.ok()) {
            LOG(ERROR) << "Failed to register UI controller with emulator: "
                       << registerStatus;
        }
    } else {
        LOG(ERROR) << "Failed to start gRPC server: " << grpcStatus;
    }

    android::files::TemporaryFile pixels;
    EmulatorQtWindow* window = EmulatorQtWindow::getInstance();

    if (!opts->qt_hide_window) {
        LOG(INFO) << "Visible ui, initializng pixel streamer at: "
                  << pixels.path();
        window->initializeStreamer("file:///" + pixels.path());
    } else {
        LOG(INFO) << "No visible ui, not initializing pixel streamer";
    }

    LOG(INFO) << "Setting up window";
    emulator_window_setup(emulator_window_get());

    LOG(INFO) << "Spawn winsys loop";

    skin_winsys_spawn_thread(false, &messagePump, 0, nullptr);
    skin_winsys_report_entering_main_loop();
    skin_winsys_enter_main_loop(false);

    LOG(INFO) << "Completed main loop";
    stopRenderer();
    emulator_finiUserInterface();
    process_late_teardown();
    LOG(INFO) << "Bye bye!";
}
