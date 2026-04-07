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
#include "android/base/files/IniFile.h"
#include "android/base/system/System.h"

#ifdef _WIN32
#include <windows.h>
#endif

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
#include "OpenGLESDispatch/OpenGLDispatchLoader.h"
#include "android/process_setup.h"
#include "android/qt/qt_path.h"
#include "android/skin/qt/QtLogger.h"
#include "android/skin/qt/SharedMemoryRenderer.h"
#include "android/skin/qt/emulator-qt-window.h"
#include "android/skin/qt/init-qt.h"
#include "android/utils/debug.h"
#include "android/utils/string.h"  // for str_reset
#include "android/utils/system.h"
#include "fishtank_agents.h"
#include "grpc/FishtankGrpcServer.h"

#include <algorithm>
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_join.h"
#include "host-common/FeatureControl.h"

using android::base::pj;
using android::base::System;
using android::emulation::control::EmulatorAdvertisement;

namespace fc = android::featurecontrol;

extern void injectFishtankConsoleAgents();
extern "C" void injectFishtankOpenglesFuncs();

// HACK ATTACK
extern "C" void emulator_window_setup(EmulatorWindow* emulator);
extern void skin_winsys_setup_library_paths();
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
std::shared_ptr<android::emulation::control::SensorClient> gSensorClient;
std::shared_ptr<android::emulation::control::SimpleScreenRecordingClient>
        gRecordingClient;

std::shared_ptr<android::emulation::control::EmulatorControlClient>
getGlobalControlClient() {
    return gControlClient;
}

std::shared_ptr<android::emulation::control::SensorClient>
getGlobalSensorClient() {
    return gSensorClient;
}

std::shared_ptr<android::emulation::control::SimpleScreenRecordingClient>
getGlobalRecordingClient() {
    return gRecordingClient;
}

/**
 * @brief Resolves the emulator discovery file path based on command-line
 * arguments.
 *
 * This function searches for the -fishtank flag in the original argv. If
 * found, it attempts to discover the emulator (by serial, "default", or
 * direct file path).
 *
 * @param argc Original argument count.
 * @param argv Original argument vector.
 * @return std::string The path to the discovery file found, or an empty
 * string.
 */
static std::string discoverEmulatorFile(int argc, char* argv[]) {
    auto system = System::get();

    // Peek at -fishtank argument early
    std::string discovery_request;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-fishtank") == 0 && i + 1 < argc) {
            discovery_request = argv[i + 1];
            break;
        }
    }

    if (discovery_request.empty()) {
        return "";
    }

    int discovery_timeout = 15;
    std::string timeoutEnv =
            system->envGet("ANDROID_FISHTANK_DISCOVERY_TIMEOUT_SECS");
    if (!timeoutEnv.empty()) {
        absl::SimpleAtoi(timeoutEnv, &discovery_timeout);
    }

    int qemu_serial = 0;
    if (absl::SimpleAtoi(discovery_request, &qemu_serial)) {
        EmulatorAdvertisement adv({});
        auto start = absl::Now();
        auto deadline = start + absl::Seconds(discovery_timeout);
        std::string possibleDiscoveryFile = adv.discoverEmulatorWithProperties(
                {{"port.serial", discovery_request}});
        while (possibleDiscoveryFile.empty() && absl::Now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            possibleDiscoveryFile = adv.discoverEmulatorWithProperties(
                    {{"port.serial", discovery_request}});
        }
        return possibleDiscoveryFile;
    } else if ("default" == discovery_request) {
        EmulatorAdvertisement adv({});
        auto emulators = adv.discoverRunningEmulators();
        std::vector<std::string> discovery_files;
        std::copy_if(emulators.begin(), emulators.end(),
                     std::back_inserter(discovery_files),
                     [](auto str) { return absl::EndsWith(str, ".ini"); });
        if (!discovery_files.empty()) {
            return discovery_files[0];
        }
    }

    if (system->pathIsFile(discovery_request)) {
        return discovery_request;
    }

    return "";
}

/**
 * @brief Sets up environment variables from the discovery file.
 *
 * This function extracts launcher and AVD directories from the discovery .ini
 * and sets the corresponding environment variables.
 *
 * @param discovery_file Path to the discovery .ini file.
 */
static void setupEnvFromDiscovery(const std::string& discovery_file) {
    auto system = System::get();
    if (discovery_file.empty() || !system->pathIsFile(discovery_file)) {
        return;
    }

    android::base::IniFile discoveryFile(discovery_file);
    if (discoveryFile.read()) {
        std::string launcherDirFromDiscovery =
                discoveryFile.getString("launcher.dir", "");
        if (!launcherDirFromDiscovery.empty()) {
            system->envSet("ANDROID_EMULATOR_LAUNCHER_DIR",
                           launcherDirFromDiscovery);
        }

        std::string avdDir = discoveryFile.getString("avd.dir", "");
        if (!avdDir.empty()) {
            std::string avdHome;
            if (android::base::PathUtils::split(avdDir.c_str(), &avdHome,
                                                nullptr)) {
                system->envSet("ANDROID_AVD_HOME", avdHome);
            }
        }
    }
}

/**
 * @brief Extracts the AVD ID from the discovery file.
 *
 * This function extracts the AVD ID from the discovery .ini.
 *
 * @param discovery_file Path to the discovery .ini file.
 * @return std::string The AVD ID, or an empty string if not found.
 */
static std::string getAvdIdFromDiscovery(const std::string& discovery_file) {
    auto system = System::get();
    if (discovery_file.empty() || !system->pathIsFile(discovery_file)) {
        return "";
    }

    android::base::IniFile discoveryFile(discovery_file);
    if (discoveryFile.read()) {
        return discoveryFile.getString("avd.id", "");
    }
    return "";
}

/**
 * @brief Ensures the ANDROID_EMULATOR_LAUNCHER_DIR environment variable is set.
 *
 * If not already set, it attempts to infer the launcher directory from
 * ANDROID_SDK_ROOT. This directory is critical for resolving emulator
 * resources like advancedFeatures.ini.
 *
 * @param system Pointer to the System interface.
 */
static void setupLauncherDirectory(System* system) {
    std::string launcherDir = system->envGet("ANDROID_EMULATOR_LAUNCHER_DIR");
    if (launcherDir.empty()) {
        LOG(WARNING) << "ANDROID_EMULATOR_LAUNCHER_DIR is not defined. You "
                        "must've started "
                     << "fishtank as a standalone application. Will try to "
                        "deduce the launcher "
                     << "directory from ANDROID_SDK_ROOT.";
        std::string sdkRoot = system->envGet("ANDROID_SDK_ROOT");
        if (!sdkRoot.empty()) {
            launcherDir = pj(sdkRoot, "emulator");
            system->setEnvironmentVariable("ANDROID_EMULATOR_LAUNCHER_DIR",
                                           launcherDir);
            LOG(INFO) << "Inferred ANDROID_EMULATOR_LAUNCHER_DIR from "
                         "ANDROID_SDK_ROOT: "
                      << launcherDir;
        } else {
            LOG(FATAL) << "Neither ANDROID_EMULATOR_LAUNCHER_DIR nor "
                          "ANDROID_SDK_ROOT is defined. Cannot start fishtank.";
        }
    } else {
        LOG(INFO) << "Using ANDROID_EMULATOR_LAUNCHER_DIR: " << launcherDir;
    }
}

/**
 * @brief Configures and starts the external renderer.
 *
 * This function disables Vulkan (as it's not currently used by Fishtank),
 * sets SwiftShader as the default GPU mode if none is specified, and
 * initializes the external renderer.
 *
 * @param hw Pointer to the hardware configuration.
 * @param program_dir Path to the directory containing the main executable.
 */
static void setupRenderer(AndroidHwConfig* hw, const std::string& program_dir) {
    // Fishtank UI does not use any Vulkan
    fc::setEnabledOverride(fc::Vulkan, false);
    // Fishtank UI only uses/ships with swiftshader GL library.
    // Map null/auto/empty gpu mode to 'swiftshader_indirect'
    if (!hw->hw_gpu_mode || !strcmp(hw->hw_gpu_mode, "") ||
        !strcmp(hw->hw_gpu_mode, "auto")) {
        str_reset(&hw->hw_gpu_mode, "swiftshader_indirect");
    }

    // Override default OpenGLES behavior with fishtank-specific functions.
    injectFishtankOpenglesFuncs();

    bool isSwiftshader = false;
    if (!strcmp(hw->hw_gpu_mode, "swiftshader_indirect") ||
        !strcmp(hw->hw_gpu_mode, "swiftshader")) {
        isSwiftshader = true;
        auto swiftshader_dir = pj({program_dir, "lib64", "gles_swiftshader"});
        LOG(INFO) << "Using swiftshader from: " << swiftshader_dir;

        // SwiftShader Library Resolution Strategy:
        // - Windows: We use SetDllDirectoryA to guide the OS loader to the
        //   swiftshader subdirectory.
        // - macOS/Linux: We rely on RPATH-based resolution (set in
        //   CMakeLists.txt) to find the libraries relative to the executable
        //   path (@executable_path/lib64/gles_swiftshader or
        //   $ORIGIN/lib64/gles_swiftshader). This avoids relying on
        //   environment variables like LD_LIBRARY_PATH/DYLD_LIBRARY_PATH,
        //   which can be restricted by features like macOS SIP.
#ifdef _WIN32
        SetDllDirectoryA(swiftshader_dir.c_str());
#endif
    }

    auto egl = gfxstream::host::gl::LazyLoadedEGLDispatch::get();
    if (egl && egl->eglUseOsEglApi) {
        LOG(INFO) << "Configuring EGL-on-EGL: " << (isSwiftshader ? "on" : "off");
        egl->eglUseOsEglApi(isSwiftshader ? EGL_TRUE : EGL_FALSE, EGL_FALSE);
    }
}

/**
 * @brief Configures environment variables required for the Qt runtime.
 *
 * Sets up paths for Qt libraries, plugins, resources, and locales, ensuring
 * that the Fishtank UI and QtWebEngine can find their dependencies.
 *
 * @param system Pointer to the System interface.
 * @param program_dir Path to the directory containing the main executable.
 */
static void setupQtEnvironment(System* system, const std::string& program_dir) {
    auto qt_base_dir = pj({program_dir, "lib64", "qt"});
#ifdef _WIN32
    auto qt_lib_path = program_dir;
    auto qt_process_path = pj({qt_base_dir, "bin", "QtWebEngineProcess.exe"});
    std::string currentPath = system->envGet("PATH");
    system->setEnvironmentVariable("PATH", program_dir + ";" + currentPath);
    LOG(INFO) << "Prepended " << program_dir << " to PATH";
#else
    auto qt_lib_path = pj(qt_base_dir, "lib");
    auto qt_process_path = pj({qt_base_dir, "libexec", "QtWebEngineProcess"});
#endif
    auto qt_plugin_path = pj(qt_base_dir, "plugins");
    auto qt_resources_path = pj(qt_base_dir, "resources");
    auto qt_locales_path =
            pj({qt_base_dir, "translations", "qtwebengine_locales"});

    system->setEnvironmentVariable("ANDROID_QT_LIB_PATH", qt_lib_path);
    system->setEnvironmentVariable("QTWEBENGINEPROCESS_PATH", qt_process_path);
    system->setEnvironmentVariable("ANDROID_QT_QPA_PLATFORM_PLUGIN_PATH",
                                   qt_plugin_path);
    system->setEnvironmentVariable("QTWEBENGINE_RESOURCES_PATH",
                                   qt_resources_path);
    system->setEnvironmentVariable("QTWEBENGINE_LOCALES_PATH", qt_locales_path);

    LOG(INFO) << "Setting ANDROID_QT_LIB_PATH to: " << qt_lib_path;
    LOG(INFO) << "Setting QTWEBENGINEPROCESS_PATH to: " << qt_process_path;
    LOG(INFO) << "Setting ANDROID_QT_QPA_PLATFORM_PLUGIN_PATH to: "
              << qt_plugin_path;
    LOG(INFO) << "Setting QTWEBENGINE_RESOURCES_PATH to: " << qt_resources_path;
    LOG(INFO) << "Setting QTWEBENGINE_LOCALES_PATH to: " << qt_locales_path;
}

/**
 * @brief Creates a UiEmuAgent populated with the available console agents.
 *
 * @return UiEmuAgent The populated agent structure.
 */
static UiEmuAgent createUiEmuAgent() {
    return {
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
            nullptr  // For now there's no uses of SettingsAgent
    };
}

/**
 * @brief Sets up and starts the Fishtank gRPC server.
 *
 * This server allows the Fishtank UI to communicate with the emulator backend
 * via gRPC.
 *
 * @param opts Pointer to the parsed AndroidOptions.
 */
static void startFishtankGrpc(AndroidOptions* opts) {
    static android::fishtank::FishtankGrpcServer fishtankGrpc;
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
}

int main(int argc, char* argv[]) {
    base_configure_logs(kLogDefaultOptions);
    injectFishtankConsoleAgents();

    auto system = System::get();
    AndroidOptions* opts = &sOpts[0];
    AndroidHwConfig* hw = getConsoleAgents()->settings->hw();

    // Construct the modified argument list. We start with a copy of the
    // original argv to preserve command-line options.
    std::vector<char*> modified_argv_storage;
    for (int i = 0; i < argc; ++i) {
        modified_argv_storage.push_back(argv[i]);
    }

    std::string discovery_file = discoverEmulatorFile(argc, argv);
    setupEnvFromDiscovery(discovery_file);

    // Extract AVD ID from discovery file if not already provided on cmdline.
    std::string avdId = getAvdIdFromDiscovery(discovery_file);
    if (!avdId.empty()) {
        bool has_avd_arg = false;
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "-avd") == 0 || argv[i][0] == '@') {
                has_avd_arg = true;
                break;
            }
        }
        if (!has_avd_arg) {
            LOG(INFO) << "Setting AVD ID from discovery: " << avdId;
            modified_argv_storage.push_back((char*)"-avd");
            modified_argv_storage.push_back((char*)avdId.c_str());
        }
    }

    // Ensure -grpc-ui is present so it survives the options reset in
    // emulator_parseCommonCommandLineOptions, allowing us to skip the
    // QEMU version check in main-common.c.
    modified_argv_storage.push_back((char*)"-grpc-ui");

    int modified_argc = (int)modified_argv_storage.size();
    char** modified_argv = modified_argv_storage.data();

    setupLauncherDirectory(system);

    process_early_setup(modified_argc, modified_argv);
    // crashhandler_init(modified_argc, modified_argv);
    async_query_host_gpu_start();

    // QtWebEngine requires the executable name in argv, so let's save it here,
    // as argv is modified below.
    char* qt_argv = modified_argv[0];
    int qt_argc = 1;

    getConsoleAgents()->settings->inject_android_cmdLine(
            android::base::createEscapedLaunchString(modified_argc,
                                                     modified_argv)
                    .c_str());

    AvdInfo* avd = nullptr;
    int exitStatus;
    if (!emulator_parseCommonCommandLineOptions(&modified_argc, &modified_argv,
                                                "arm64", true,  // is_qemu2
                                                opts, hw, &avd, &exitStatus)) {
        LOG(FATAL) << "Failed to parse common command line options.";
    }

    if (!discovery_file.empty()) {
        str_reset(&opts->fishtank, discovery_file.c_str());
    }

    getConsoleAgents()->settings->inject_AvdInfo(avd);

    const UiEmuAgent uiEmuAgent = createUiEmuAgent();

    if (!emulator_parseUiCommandLineOptions(opts, avd, hw)) {
        LOG(FATAL) << "Bad news bears, unable to init ui";
    }

    android::emulation::control::EmulatorGrpcClient::Builder builder;
    if (VERBOSE_CHECK(grpc)) {
        builder.withInterceptor(new StdOutLoggingInterceptorFactory());
    }

    auto status = builder.withDiscoveryFile(opts->fishtank).build();
    if (!status.ok()) {
        LOG(FATAL) << "Failed to discover emulator due to "
                   << status.status().ToString();
        return 1;
    }
    android::emulation::control::EmulatorGrpcClient::configureMe(
            std::move(status.value()));
    if (!android::emulation::control::EmulatorGrpcClient::me()
                 ->hasOpenChannel()) {
        LOG(FATAL) << "Failed to connect to emulator";
    }

    gControlClient = std::make_shared<
            android::emulation::control::EmulatorControlClient>(
            android::emulation::control::EmulatorGrpcClient::me());
    gSensorClient = std::make_shared<android::emulation::control::SensorClient>(
            android::emulation::control::EmulatorGrpcClient::me());
    gRecordingClient = std::make_shared<
            android::emulation::control::SimpleScreenRecordingClient>(
            android::emulation::control::EmulatorGrpcClient::me());
    initializeGrpcUserEventAgent(gControlClient.get());

    auto program_dir = system->getProgramDirectory();
    setupRenderer(hw, program_dir);
    setupQtEnvironment(system, program_dir);

    skin_winsys_init_args(qt_argc, &qt_argv);
    if (!emulator_initUserInterface(opts, &uiEmuAgent)) {
        dwarning("%s: user interface init failed", __func__);
        return 1;
    }

    startFishtankGrpc(opts);

    android::files::TemporaryFile pixels;
    EmulatorQtWindow* window = EmulatorQtWindow::getInstance();
    if (!opts->qt_hide_window) {
        if (opts->grpc_ui) {
            LOG(INFO) << "Visible ui, initializing pixel streamer via gRPC";
            window->initializeStreamer("", StreamTransport::Standard);
        } else {
            LOG(INFO) << "Visible ui, initializing pixel streamer at: "
                      << pixels.path();
            window->initializeStreamer("file:///" + pixels.path(),
                                       StreamTransport::MMAP);
        }
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
    emulator_finiUserInterface();
    process_late_teardown();
    LOG(INFO) << "Bye bye!";
}
