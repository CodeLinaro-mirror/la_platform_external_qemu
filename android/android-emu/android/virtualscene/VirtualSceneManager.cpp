/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "android/virtualscene/VirtualSceneManager.h"

#include "aemu/base/EventNotificationSupport.h"
#include "aemu/base/files/PathUtils.h"
#include "android/avd/info.h"
#include "android/base/system/System.h"
#include "android/camera/camera-metrics.h"
#include "android/camera/camera-virtualscene-utils.h"
#include "android/cmdline-option.h"
#include "android/console.h"
#include "android/skin/winsys.h"
#include "android/utils/debug.h"
#include "host-common/FeatureControl.h"
#include "host-common/hw-config-helper.h"
#include "host-common/hw-config.h"
#include "host-common/opengles.h"

#include <deque>
#include <fstream>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "android/physics/GlmHelpers.h"

using namespace android::base;

// TODO(virtualscene): disable debug logs after testing throughly
#define DEBUG_LOGS 1

#define E(...) derror(__VA_ARGS__)
#define W(...) dwarning(__VA_ARGS__)

#if DEBUG_LOGS
#define D(...) dprint(__VA_ARGS__)
#else
#define D(...) (void)0
#endif

static constexpr const char* kPosterFile = "Toren1BD.posters";

// Update at 30 fps by default
static constexpr int kUpdatePerSecond = 30;

// TODO(virtualscene-library): use different namespace for VirtualSceneManager
namespace android {
namespace virtualscene {

/**
 * @brief Parses a .posters file and returns a list of PosterInfo structures.
 *
 * @param filename The name of the posters file to parse.
 * @return A vector of PosterInfo containing the parsed poster data.
 */
std::vector<PosterInfo> parsePostersFile(const char* filename) {
    const std::string filePath = android::base::PathUtils::join(
            android::base::System::get()->getLauncherDirectory(), "resources",
            filename);

    std::ifstream in(
            android::base::PathUtils::asUnicodePath(filePath.data()).c_str());
    if (!in) {
        dwarning("%s: Could not find file '%s'", __FUNCTION__, filename);
        return {};
    }

    std::vector<PosterInfo> results;
    PosterInfo poster;

    std::string str;

    for (in >> str; !in.eof(); in >> str) {
        if (str.empty()) {
            continue;
        }

        if (str == "poster") {
            // New poster entry, specified with a string name.
            if (!poster.name.empty()) {
                // Store existing poster.
                dprint("%s: Loaded poster %s at (%f, %f, %f)", __FUNCTION__,
                       poster.name.c_str(), poster.position.x,
                       poster.position.y, poster.position.z);
                results.push_back(poster);
            }

            poster = PosterInfo();
            in >> poster.name;
            if (!in) {
                dwarning("%s: Invalid name.", __FUNCTION__);
                return {};
            }

        } else if (str == "position") {
            // Poster center position.
            // Specified with three floating point numbers, separated by
            // whitespace.

            in >> poster.position.x >> poster.position.y >> poster.position.z;
            if (!in) {
                dwarning("%s: Invalid position.", __FUNCTION__);
                return {};
            }
        } else if (str == "rotation") {
            // Poster rotation.
            // Specified with three floating point numbers, separated by
            // whitespace.  This represents euler angle rotation in degrees, and
            // it is applied in XYZ order.

            glm::vec3 eulerRotation;
            in >> eulerRotation.x >> eulerRotation.y >> eulerRotation.z;
            if (!in) {
                dwarning("%s: Invalid rotation.", __FUNCTION__);
                return {};
            }

            poster.rotation = fromEulerAnglesXYZ(glm::radians(eulerRotation));
        } else if (str == "size") {
            // Poster center position.
            // Specified with two floating point numbers, separated by
            // whitespace.

            in >> poster.size.x >> poster.size.y;
            if (!in) {
                dwarning("%s: Invalid size.", __FUNCTION__);
                return {};
            }
        } else if (str == "default") {
            // Poster default filename.
            // Specified with a string parameter.

            in >> poster.defaultFilename;
            if (!in) {
                dwarning("%s: Invalid default filename.", __FUNCTION__);
                return {};
            }
        } else {
            dwarning("%s: Invalid input %s", __FUNCTION__, str.c_str());
            return {};
        }
    }

    if (poster.name.empty()) {
        derror("%s: Posters file did not contain any entries.", __FUNCTION__);
        return {};
    }

    // Store last poster.
    dprint("%s: Loaded poster %s at (%f, %f, %f)", __FUNCTION__,
           poster.name.c_str(), poster.position.x, poster.position.y,
           poster.position.z);
    results.push_back(poster);

    return results;
}

// Stores settings for the virtual scene.
//
// Access to the instance of this class, sSettings should be guarded by
// VirtualSceneManager::mLock.
class Settings {
public:
    // Defines the setting for a single poster.
    struct PosterSetting {
        std::string mFilename;
        float mScale = 1.0f;
    };

    Settings() { mPosterLocations = parsePostersFile(kPosterFile); }

    void parseCmdlineParameter(std::string_view param) {
        auto it = std::find(param.begin(), param.end(), '=');
        if (it == param.end()) {
            E("%s: Invalid command line parameter '%s', should be "
              "<name>=<filename>",
              __FUNCTION__, std::string(param).c_str());
            return;
        }

        std::string name(param.begin(), it++);
        std::string_view filename(&*it, param.end() - it);

        std::string absFilename;
        if (!PathUtils::isAbsolute(filename.data())) {
            absFilename = PathUtils::join(System::get()->getCurrentDirectory(),
                                          filename.data());
        } else {
            absFilename = filename;
        }

        if (!System::get()->pathExists(absFilename)) {
            E("%s: Path '%s' does not exist.", __FUNCTION__,
              absFilename.c_str());
            return;
        }

        D("%s: Found poster %s at %s", __FUNCTION__, name.c_str(),
          absFilename.c_str());

        mPosterSettings[name].mFilename = absFilename;
    }

    // Set the poster if it is not already defined.
    void setInitialPoster(const char* posterName, const char* filename) {
        if (mPosterSettings.find(posterName) == mPosterSettings.end()) {
            setPoster(posterName, filename);
        }
    }

    // Set the poster filename.
    void setPoster(const char* posterName, const char* filename) {
        mPosterSettings[posterName].mFilename =
                filename ? filename : std::string();
    }

    // Set the poster scale.
    void setPosterScale(const char* posterName, float scale) {
        mPosterSettings[posterName].mScale = scale;
    }

    // Enable/Disable TV animation.
    void setAnimationState(bool state) { mAnimationState = state; }

    bool getAnimationState() const { return mAnimationState; }

    const std::vector<PosterInfo> getPosterLocations() const {
        return mPosterLocations;
    }

    const std::unordered_map<std::string, PosterSetting>& getPosterSettings()
            const {
        return mPosterSettings;
    }

private:
    std::vector<PosterInfo> mPosterLocations;
    std::unordered_map<std::string, PosterSetting> mPosterSettings;
    bool mAnimationState = true;
};

static LazyInstance<Settings> sSettings = LAZY_INSTANCE_INIT;

// Structured data loaded from environment.ini file
struct EnvironmentConfig {
    static const float defaultBackgroundBlur = 5.0f;
    static const int defaultFps = 30;
    SceneConfig::Mode sceneMode = SceneConfig::Mode::Unknown;
    std::string sceneArgument;
    bool backgroundEnabled = true;
    float backgroundBlur = defaultBackgroundBlur;
    int fps;
};

static EnvironmentConfig getEnvironmentConfig(const AvdInfo* avdInfo,
                                              bool warnMissing,
                                              bool showBackground) {
    EnvironmentConfig ret;

    // Environment is required, set it up
    bool modeSet = false;
    CIniFile* environmentIni = avdInfo_getEnvironmentIni(avdInfo);
    if (!environmentIni) {
        // Not having an environment file is unexpected if it's not in
        // 'virtualscene' mode, defaults will be used
        if (warnMissing) {
            dwarning("%s: No environment config is provided", __func__);
        } else {
            dinfo("%s: No environment config is provided", __func__);
        }
    } else {
        // Ensure forward compatibility by checking a version string, if it's
        // newer the user should be using a newer version of the emulator to be
        // able to correctly load the environment config.
        int fileVersion = iniFile_getInteger(environmentIni, "version", 1);
        if (fileVersion != 1) {
            derror("%s: Invalid environment.ini version '%d' for this emulator version, "
                   "using defaults for the environment scene configuration.",
                   __func__, fileVersion);
        } else {
            std::string mode =
                    iniFile_getString(environmentIni, "scene.mode", "");
            if (!mode.empty()) {
                int separator = mode.find(':');
                int argpos;
                if (separator == std::string::npos) {
                    argpos = mode.length();
                } else {
                    argpos = separator + 1;
                }
                ret.sceneMode =
                        SceneConfig::modeFromString(mode.substr(0, separator));
                if (ret.sceneMode == SceneConfig::Mode::Unknown) {
                    dwarning("%s: Invalid mode set.", __func__);
                } else {
                    ret.sceneArgument = mode.substr(argpos);
                    modeSet = true;
                }
            } else {
                // Handle legacy image file specification
                std::string backgroundImageFilename = iniFile_getString(
                        environmentIni, "background.image.filename", "");
                if (!backgroundImageFilename.empty()) {
                    ret.sceneMode = SceneConfig::Mode::ImageFile;
                    ret.sceneArgument = backgroundImageFilename;
                    modeSet = true;
                }
            }

            // Update background view parameters from config, if given
            ret.backgroundBlur = static_cast<float>(iniFile_getDouble(
                    environmentIni, "background.blurAmount",
                    EnvironmentConfig::defaultBackgroundBlur));

            ret.backgroundEnabled =
                    iniFile_getBoolean(environmentIni, "background.enabled",
                                       "true") != 0;
        }
    }

    if (!modeSet) {
        if (showBackground) {
            dinfo("%s: Using default blank background for the environment.",
                  __func__);
            ret.sceneMode = SceneConfig::Mode::Color;
        } else {
            dinfo("%s: Using default virtual scene mode for the environment.",
                  __func__);
            ret.sceneMode = SceneConfig::Mode::Mesh3D;
        }
    }
    if (ret.sceneArgument.empty()) {
        dinfo("%s: Using default configuration for mode %s for the environment.",
              __func__, SceneConfig::modeToString(ret.sceneMode));
        ret.sceneArgument = SceneConfig::defaultArgumentForMode(ret.sceneMode);
    }

    return ret;
}

/*******************************************************************************
 *                     VirtualSceneManager API.
 ******************************************************************************/

StaticLock VirtualSceneManager::mLock;
VerSceneHandle VirtualSceneManager::mEnvironmentScene = nullptr;
std::deque<std::string> VirtualSceneManager::mPosterFilenameUpdates;
std::optional<std::thread> VirtualSceneManager::mBackgroundUpdateThread;
std::function<void()> VirtualSceneManager::mUpdateCallback;
std::atomic<int> VirtualSceneManager::mNumUsers = 0;
std::atomic<bool> VirtualSceneManager::mKeepUpdating = false;
bool VirtualSceneManager::mShowBackground = false;

class AnimationStatePublisher
    : public android::base::EventNotificationSupport<bool> {
public:
    void notifyListeners(bool enabled) { fireEvent(enabled); }
};

static AnimationStatePublisher sAnimationStateEventSupport;

void* VirtualSceneManager::getAnimationStateEventListener() {
    return &sAnimationStateEventSupport;
}

void VirtualSceneManager::parseCmdline() {
    AutoLock lock(mLock);
    if (sSettings.hasInstance()) {
        E("VirtualSceneManager settings already loaded");
        return;
    }

    if (!getConsoleAgents()->settings->has_cmdLineOptions()) {
        return;
    }

    if (!androidHwConfig_hasVirtualSceneOrEnvironmentCamera(
                getConsoleAgents()->settings->hw()) &&
        getConsoleAgents()
                ->settings->android_cmdLineOptions()
                ->virtualscene_poster) {
        W("[VirtualScene] Poster parameter ignored, virtual scene is not "
          "enabled.");
        return;
    }

    const ParamList* feature = getConsoleAgents()
                                       ->settings->android_cmdLineOptions()
                                       ->virtualscene_poster;
    while (feature) {
        sSettings->parseCmdlineParameter(feature->param);
        feature = feature->next;
    }
}

bool VirtualSceneManager::initialize(bool initBackgroundService,
                                     bool transparentDisplay) {
    AutoLock lock(mLock);
    if (mEnvironmentScene) {
        E("VirtualSceneManager already initialized");
        return false;
    }

    if (!getConsoleAgents() || !getConsoleAgents()->settings ||
        !getConsoleAgents()->settings->avdInfo() ||
        !getConsoleAgents()->settings->hw()) {
        derror("%s: invalid state!", __func__);
        return false;
    }

    const AvdInfo* avdInfo = getConsoleAgents()->settings->avdInfo();
    const AndroidHwConfig* hwCfg = getConsoleAgents()->settings->hw();
    const bool warnIfMissing = !strcmp(hwCfg->hw_camera_back, "virtualscene");

    mShowBackground = transparentDisplay;
    EnvironmentConfig envConfig =
            getEnvironmentConfig(avdInfo, warnIfMissing, mShowBackground);
    SceneConfig sceneConfig(envConfig.sceneMode, envConfig.sceneArgument);

    D("Initializing VirtualSceneManager with mode:%s, argument:%s",
      SceneConfig::modeToString(sceneConfig.mSceneMode),
      sceneConfig.mArgument.c_str());

    // Use scene mode name for metrics
    const char* sceneModeStr =
            SceneConfig::modeToString(sceneConfig.mSceneMode);
    camera::CameraMetrics::instance().setVirtualSceneName(sceneModeStr);

    mEnvironmentScene = ver_create_scene(sceneConfig);

    if (mEnvironmentScene == VER_INVALID_HANDLE) {
        E("VirtualSceneManager scene could not be initialized");
        return false;
    }

    mKeepUpdating = false;

    lock.unlock();

    if (initBackgroundService) {
        int displayWidth, displayHeight;
        androidHwConfig_getScreenDimensions(hwCfg, &displayWidth,
                                            &displayHeight);
        dinfo("%s: Setting up screen background view at %dx%d", __func__,
              displayWidth, displayHeight);

        if (!BackgroundUpdateService::start(displayWidth, displayHeight,
                                            envConfig.backgroundEnabled,
                                            envConfig.backgroundBlur)) {
            derror("%s: Cannot initialize background update service", __func__);
            return false;
        }
    }

    return true;
}

void VirtualSceneManager::uninitialize() {
    D("Uninitializing VirtualSceneManager");

    // TODO(virtualscene): stop background update service before calling this
    // function
    BackgroundUpdateService::stop();

    // First, stop the update thread, should be done outside of
    // the lock so that the tasks can finish properly.
    stopSceneUpdateThread();

    AutoLock lock(mLock);
    ver_destroy_scene(mEnvironmentScene);
    mEnvironmentScene = VER_INVALID_HANDLE;
    mPosterFilenameUpdates.clear();
}

void VirtualSceneManager::update() {
    if (!mLock.tryLock()) {
        // Scene is in use, skip this update..
        return;
    }
    if (mEnvironmentScene == VER_INVALID_HANDLE) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
    }

    // Settings::AnimationState is mainly used for TV animation in default
    // virtualscene and animation is controlled by renderTime in shaders. Always
    // update the scene and timer in other modes.
    bool updateTime = true;
    if (SceneConfig::modeSupportsAnimations(
                ver_scene_get_mode(mEnvironmentScene))) {
        // Use virtualscene settings for animation control
        updateTime = sSettings->getAnimationState();
    } else {
        // Static scene, no need to update which may invalidate view caches
        updateTime = false;
    }

    ver_scene_update(mEnvironmentScene, updateTime);

    mLock.unlock();

    // Perform any requested updates, should be called outside of the lock as
    // any lock requiring operations should hold its own lock
    if (mUpdateCallback) {
        mUpdateCallback();
    }
}

bool VirtualSceneManager::viewCacheRequiresUpdate(
        const VerRenderViewHandle view) {
    if (!view) {
        E("%s: invalid parameters", __FUNCTION__);
        return false;
    }

    uint64_t sceneHash, frameTime;
    {
        AutoLock lock(mLock);
        sceneHash =
                ver_scene_get_version_hash_for_view(mEnvironmentScene, view);
        frameTime = ver_scene_get_frame_time_us(mEnvironmentScene);
    }

    return !ver_render_view_cache_is_valid_for(view, sceneHash, frameTime);
}

bool VirtualSceneManager::renderView(VerRenderViewHandle view,
                                     VerRenderFinishCallback finishCallback,
                                     uint64_t* outFrameTime) {
    AutoLock lock(mLock);
    if (mEnvironmentScene == VER_INVALID_HANDLE) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return false;
    }

    return ver_render_view(
            mEnvironmentScene, view,
            [&]() {
                // Posters needs to be updated within the render context
                // TODO(virtualscene): load posters async and avoid overwriting
                // finishCallback
                const auto& posters = sSettings->getPosterSettings();
                while (!mPosterFilenameUpdates.empty()) {
                    const char* posterName =
                            mPosterFilenameUpdates.front().c_str();
                    const Settings::PosterSetting& setting =
                            posters.at(posterName);
                    ver_scene_load_poster(mEnvironmentScene, posterName,
                                          setting.mFilename.c_str(),
                                          setting.mScale);
                    mPosterFilenameUpdates.pop_front();
                }

                finishCallback();
            },
            outFrameTime);
}

void VirtualSceneManager::setInitialPoster(const char* posterName,
                                           const char* filename) {
    AutoLock lock(mLock);
    sSettings->setInitialPoster(posterName, filename);

    // If the scene is active, it will update the poster in the next render()
    // invocation.
    mPosterFilenameUpdates.push_back(posterName);
}

bool VirtualSceneManager::loadPoster(const char* posterName,
                                     const char* filename) {
    AutoLock lock(mLock);
    sSettings->setPoster(posterName, filename);

    // If the scene is active, it will update the poster in the next render()
    // invocation.
    mPosterFilenameUpdates.push_back(posterName);

    return true;
}

void VirtualSceneManager::enumeratePosters(void* context,
                                           EnumeratePostersCallback callback) {
    AutoLock lock(mLock);

    const auto& settings = sSettings->getPosterSettings();
    for (const auto& location : sSettings->getPosterLocations()) {
        float scale = 1.0f;
        const char* filename = nullptr;

        auto settingIt = settings.find(location.name);
        if (settingIt != settings.end()) {
            filename = settingIt->second.mFilename.empty()
                               ? nullptr
                               : settingIt->second.mFilename.c_str();
            scale = settingIt->second.mScale;
        }

        const float maxWidth = location.size.x;
        callback(context, location.name.c_str(), kPosterMinimumSizeMeters,
                 maxWidth, filename, scale);
    }
}

void VirtualSceneManager::setPosterScale(const char* posterName, float scale) {
    AutoLock lock(mLock);
    sSettings->setPosterScale(posterName, scale);

    // Updating the poster scale can be done on any thread, update it now.
    if (mEnvironmentScene) {
        ver_scene_update_poster_scale(mEnvironmentScene, posterName, scale);
    }
}

void VirtualSceneManager::setAnimationState(bool state) {
    bool changed = false;
    {
        AutoLock lock(mLock);
        if (sSettings->getAnimationState() != state) {
            sSettings->setAnimationState(state);
            changed = true;
        }
    }
    if (changed) {
        sAnimationStateEventSupport.notifyListeners(state);
    }
}

bool VirtualSceneManager::getAnimationState() {
    AutoLock lock(mLock);
    return sSettings->getAnimationState();
}

void VirtualSceneManager::setSceneControlsParameters(bool show) {
    AutoLock lock(mLock);
    if (mEnvironmentScene == VER_INVALID_HANDLE) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return;
    }

    // Only allow showing scene controls if it's a 3d scene
    if (!show || SceneConfig::modeSupportsSceneControls(
                         ver_scene_get_mode(mEnvironmentScene))) {
        D("%s: show=%s", __func__, (show ? "true" : "false"));
        skin_winsys_show_virtual_scene_controls(show);
    }
}

bool VirtualSceneManager::addSceneUser() {
    AutoLock lock(mLock);
    if (mEnvironmentScene == VER_INVALID_HANDLE) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return false;
    }
    if (mNumUsers == 0) {
        // Make sure the scene is ready to use, this will also
        // crete the renderer and load renderer resources if needed
        ver_scene_load_user_resources(mEnvironmentScene, [&]() {
            //  Load the poster configuration in the scene.
            for (const auto& it : sSettings->getPosterLocations()) {
                if (!ver_scene_create_poster_location(mEnvironmentScene, it)) {
                    W("VirtualSceneManager failed to create poster location");
                }
            }

            for (const auto& it : sSettings->getPosterSettings()) {
                const char* posterName = it.first.c_str();
                const Settings::PosterSetting& setting = it.second;
                ver_scene_load_poster(mEnvironmentScene, posterName,
                                      setting.mFilename.c_str(),
                                      setting.mScale);
            }
        });

        startSceneUpdateThread();
    }
    mNumUsers++;

    return true;
}

void VirtualSceneManager::removeSceneUser() {
    AutoLock lock(mLock);
    if (mEnvironmentScene == VER_INVALID_HANDLE) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return;
    }
    mNumUsers--;
    if (mNumUsers == 0) {
        // Allow scene to unload resources when there are no users of it
        ver_scene_unload_user_resources(mEnvironmentScene);

        lock.unlock();
        stopSceneUpdateThread();
    }
}

void VirtualSceneManager::setUpdateCallback(std::function<void()> callback) {
    AutoLock lock(mLock);
    if (mUpdateCallback) {
        // Callback should be set only once as it'll be used outside of the lock
        E("%s:%d Background update callback is already set", __func__,
          __LINE__);
        return;
    }
    mUpdateCallback = callback;
}

SceneConfig::Mode VirtualSceneManager::getSceneMode() {
    AutoLock lock(mLock);
    if (mEnvironmentScene == VER_INVALID_HANDLE) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return SceneConfig::Mode::Unknown;
    }
    return ver_scene_get_mode(mEnvironmentScene);
}

bool VirtualSceneManager::reloadScene(const SceneConfig& config) {
    bool shouldNotifyAnimation = false;
    {
        AutoLock lock(mLock);

        // Only reload if the config has changed
        const SceneConfig* existingConfig =
                ver_scene_get_config(mEnvironmentScene);
        if (mEnvironmentScene && existingConfig && *existingConfig == config) {
            D("%s: no changes to the scene config.", __func__);
            return true;
        }

        D("%s: Reloading with mode:%s, argument:%s", __func__,
          SceneConfig::modeToString(config.mSceneMode),
          config.mArgument.c_str());

        // Create a new scene and check if there were any errors
        auto scene = ver_create_scene(config);
        if (!scene) {
            E("VirtualSceneManager scene failed to reload!");
            return false;
        }

        // When we set a scene that animates, toggle animation on.
        // Otherwise, a nonplaying video may cause confusion.
        // In the future we may want to revisit this based on UI
        // decisions.
        if (config.modeSupportsAnimations(config.mSceneMode)) {
            if (!sSettings->getAnimationState()) {
                sSettings->setAnimationState(true);
                shouldNotifyAnimation = true;
            }
        }

        // If we're currently running, we need to load resources
        if (mNumUsers > 0) {
            ver_scene_load_user_resources(scene, []() {});
            ver_scene_update(scene, false);
        }

        // TODO(virtualscene) Handle virtual scene controls. Those should move
        // out of the camera callback and be controlled here, since the camera
        // has no knowledge of what the scene is when it changes.

        // Replace the scene, not that this is safe because we don't expose the
        // scene to the outside users and all operations are done in-sync
        // through VirtualSceneManager interface
        ver_destroy_scene(mEnvironmentScene);
        mEnvironmentScene = scene;

        D("%s: finished", __func__);
    }
    if (shouldNotifyAnimation) {
        sAnimationStateEventSupport.notifyListeners(true);
    }

    return true;
}

bool VirtualSceneManager::reloadEnvironment(const char* environmentData) {
    if (!getConsoleAgents() || !getConsoleAgents()->settings ||
        !getConsoleAgents()->settings->avdInfo()) {
        derror("%s: invalid state!", __func__);
        return false;
    }

    const AvdInfo* avdInfo = getConsoleAgents()->settings->avdInfo();

    // Reload ini file
    if (!avdInfo_reloadEnvironmentIni(avdInfo, environmentData)) {
        derror("%s: failed to reload environment config!", __func__);
        return false;
    }

    EnvironmentConfig envConfig =
            getEnvironmentConfig(avdInfo, true, mShowBackground);

    // Reload virtual scene
    SceneConfig sceneConfig(envConfig.sceneMode, envConfig.sceneArgument);
    if (!reloadScene(sceneConfig)) {
        derror("%s: Cannot reload virtual scene for the environment", __func__);
        return false;
    }

    // Save environment.ini on success
    if (environmentData) {
        avdInfo_saveEnvironmentIni(avdInfo);
    }

    // Update background service parameters
    BackgroundUpdateService::updateBackgroundEnabled(
            envConfig.backgroundEnabled);
    BackgroundUpdateService::updateBlurAmount(envConfig.backgroundBlur);

    dinfo("%s: Reloaded environment with scene argument: %s", __func__,
          envConfig.sceneArgument.c_str());

    return true;
}

void VirtualSceneManager::updateSceneWorker() {
    const auto interval = std::chrono::microseconds(1000000 / kUpdatePerSecond);
    auto nextUpdateTime = std::chrono::steady_clock::now();
    while (mKeepUpdating) {
        nextUpdateTime += interval;

        update();

        // Sleep until the next update
        auto now = std::chrono::steady_clock::now();
        if (now >= nextUpdateTime) {
            // update took longer than the interval, skip missed frames
            nextUpdateTime = now;
            // We must still yield, or we risk starving out other threads
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_until(nextUpdateTime);
        }
    }
}

void VirtualSceneManager::startSceneUpdateThread() {
    D("%s: Starting update thread", __func__);
    if (mBackgroundUpdateThread) {
        E("%s:%d Background Update Thread is already initialized", __func__,
          __LINE__);
        return;
    }
    mKeepUpdating = true;
    mBackgroundUpdateThread = std::thread(updateSceneWorker);
}

void VirtualSceneManager::stopSceneUpdateThread() {
    if (!mKeepUpdating) {
        return;
    }
    D("%s: Stopping update thread", __func__);
    mKeepUpdating = false;
    std::thread threadToJoin;
    {
        // Only lock to reset the member variable
        AutoLock lock(mLock);
        if (mBackgroundUpdateThread.has_value()) {
            threadToJoin = std::move(*mBackgroundUpdateThread);
            mBackgroundUpdateThread.reset();
        }
    }
    if (threadToJoin.joinable()) {
        threadToJoin.join();
    }
    D("%s: Stopped update thread", __func__);
}

int VirtualSceneManager::getSceneBaseRotationLocked() {
    if (mEnvironmentScene == VER_INVALID_HANDLE) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return 0;
    } else {
        return ver_scene_get_scene_rotation(mEnvironmentScene);
    }
}

int VirtualSceneManager::getSceneBaseRotation() {
    AutoLock lock(mLock);
    return getSceneBaseRotationLocked();
}

/*******************************************************************************
 *                     BackgroundUpdateService API.
 ******************************************************************************/
std::unique_ptr<SceneCamera> BackgroundUpdateService::mSceneCamera;
VerRenderViewHandle BackgroundUpdateService::mBackgroundView =
        VER_INVALID_HANDLE;
std::vector<uint8_t> BackgroundUpdateService::mReadbackDataCopy;
bool BackgroundUpdateService::mStarted = false;
bool BackgroundUpdateService::mBackgroundEnabled = true;
bool BackgroundUpdateService::mScreenBackgroundSet = false;

bool BackgroundUpdateService::start(int displayWidth,
                                    int displayHeight,
                                    bool backgroundEnabled,
                                    float backgroundBlur) {
    const float aspectRatio = static_cast<float>(displayWidth) / displayHeight;
    mSceneCamera = std::make_unique<SceneCamera>();
    mSceneCamera->setAspectRatio(aspectRatio);

    // TODO(virtualscene): do not call renderView if it's a static
    // image, adjust fps based on environment.ini
    mBackgroundView = ver_create_render_view();
    ver_render_view_set_dimensions(mBackgroundView, displayWidth,
                                   displayHeight);
    ver_render_view_set_blur_factor(mBackgroundView, backgroundBlur);
    mReadbackDataCopy.resize(displayWidth * displayHeight * 4);

    mBackgroundEnabled = backgroundEnabled;

    // Set update callback, to update the background image after each
    // scene update
    VirtualSceneManager::setUpdateCallback([displayWidth, displayHeight]() {
        if (mBackgroundEnabled) {
            const bool supportsPosition =
                    (VirtualSceneManager::getSceneMode() ==
                     SceneConfig::Mode::Mesh3D);
            mSceneCamera->update(supportsPosition);

            // TODO(virtualscene) Handle rotation properly for all scenes.
            // SceneCamera uses 90 degrees rotated views by default for
            // the camera rendering, rotate it back to correct for background
            float angle = VirtualSceneManager::getSceneBaseRotation();
            glm::mat4 rollRotation =
                    glm::rotate(glm::mat4(1.0f), glm::radians(angle),
                                glm::vec3(0.0f, 0.0f, 1.0f));
            glm::mat4 cameraView = rollRotation * mSceneCamera->getView();
            glm::mat4 viewProjection =
                    mSceneCamera->getProjection() * cameraView;
            ver_render_view_set_view_projection(mBackgroundView,
                                                &viewProjection[0][0]);

            if (!mScreenBackgroundSet ||
                VirtualSceneManager::viewCacheRequiresUpdate(mBackgroundView)) {
                if (VirtualSceneManager::renderView(
                            mBackgroundView,
                            []() {
                                const uint8_t* viewFbDataPtr = nullptr;
                                uint64_t viewFbDataSize = 0;
                                ver_render_view_get_framebuffer(
                                        mBackgroundView, &viewFbDataPtr,
                                        &viewFbDataSize);
                                if (!viewFbDataPtr || viewFbDataSize == 0) {
                                    LOG(ERROR)
                                            << "Could not get framebuffer data for the background view.";
                                    return;
                                }

                                mReadbackDataCopy.resize(viewFbDataSize);
                                memcpy(mReadbackDataCopy.data(), viewFbDataPtr,
                                       viewFbDataSize);
                            },
                            nullptr)) {
                    // Update the background image for the display composition
                    // TODO(virtualscene-perf): Avoid copy of the data by making
                    // android_setOpenglesScreenBackground call lighter weight
                    // and callable inside the lock
                    android_setOpenglesScreenBackground(
                            displayWidth, displayHeight,
                            mReadbackDataCopy.data());
                    mScreenBackgroundSet = true;
                }
            }
        } else if (mScreenBackgroundSet) {
            // Reset the screen background if an image has been set before
            android_setOpenglesScreenBackground(0, 0, nullptr);
            mScreenBackgroundSet = false;
        }
    });

    // A user should be added even when the background rendering is not enabled,
    // this will ensure scene and animations are updated correctly for the
    // camera and other users of the environment scene
    VirtualSceneManager::addSceneUser();
    mStarted = true;

    return true;
}

void BackgroundUpdateService::stop() {
    if (!mStarted) {
        // Service is not active
        return;
    }

    VirtualSceneManager::removeSceneUser();
    if (mBackgroundView != VER_INVALID_HANDLE) {
        ver_destroy_render_view(mBackgroundView);
        mBackgroundView = VER_INVALID_HANDLE;
    }
    mSceneCamera.reset();
    mStarted = false;
}

void BackgroundUpdateService::updateBackgroundEnabled(bool renderEnabled) {
    mBackgroundEnabled = renderEnabled;
}

void BackgroundUpdateService::updateBlurAmount(float blurAmount) {
    if (mBackgroundView != VER_INVALID_HANDLE) {
        ver_render_view_set_blur_factor(mBackgroundView, blurAmount);
    }
}

}  // namespace virtualscene
}  // namespace android
