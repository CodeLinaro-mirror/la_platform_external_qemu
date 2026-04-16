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

#include "aemu/base/files/PathUtils.h"
#include "android/avd/info.h"
#include "android/base/system/System.h"
#include "android/camera/camera-virtualscene-utils.h"
#include "android/cmdline-option.h"
#include "android/console.h"
#include "android/raw_image_sources/raw_image_source.h"
#include "android/skin/winsys.h"
#include "android/utils/debug.h"
#include "android/virtualscene/PosterInfo.h"
#include "android/virtualscene/Renderer.h"
#include "android/virtualscene/Scene.h"
#include "host-common/FeatureControl.h"
#include "host-common/hw-config-helper.h"
#include "host-common/hw-config.h"
#include "host-common/opengles.h"

#include <deque>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

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

namespace android {
namespace virtualscene {

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
        std::string mode = iniFile_getString(environmentIni, "scene.mode", "");
        if (!mode.empty()) {
            modeSet = true;
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
                // TODO(virtualscene) Do we want to use the default magenta
                // color here instead?
                dinfo("%s: Invalid mode set. Using default virtual scene mode for the environment.",
                      __func__);
                ret.sceneMode = SceneConfig::Mode::Mesh3D;
                ret.sceneArgument =
                        SceneConfig::defaultArgumentForMode(ret.sceneMode);
            }
            ret.sceneArgument = mode.substr(argpos);
        } else {
            // Handle legacy image file specification
            std::string backgroundImageFilename = iniFile_getString(
                    environmentIni, "background.image.filename", "");
            if (!backgroundImageFilename.empty()) {
                modeSet = true;
                ret.sceneMode = SceneConfig::Mode::ImageFile;
                ret.sceneArgument = backgroundImageFilename;
            }
        }

        // Update blur amount from config, if given
        ret.backgroundBlur = static_cast<float>(
                iniFile_getDouble(environmentIni, "background.blurAmount",
                                  EnvironmentConfig::defaultBackgroundBlur));
    }

    if (!modeSet) {
        if (showBackground) {
            dinfo("%s: Using blank background for the environment.", __func__);
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
 *                     ScenesManager API.
 ******************************************************************************/

StaticLock ScenesManager::mLock;
std::vector<std::shared_ptr<Scene>> ScenesManager::mScenes;

std::shared_ptr<Scene> ScenesManager::createScene(const SceneConfig& config) {
    if (config.mSceneMode == SceneConfig::Mode::Unknown) {
        E("%s: invalid config", __func__);
        return nullptr;
    }

    // Create a new scene
    D("Initializing a scene with mode:%s, argument:%s",
      SceneConfig::modeToString(config.mSceneMode), config.mArgument.c_str());

    std::shared_ptr<Scene> scene = Scene::create(config);
    if (!scene) {
        E("VirtualSceneManager scene failed to load");
        return nullptr;
    }

    AutoLock lock(mLock);
    mScenes.push_back(scene);

    return scene;
}

bool ScenesManager::renderView(Scene* scene,
                               RendererView* view,
                               std::function<void()> finishCallback,
                               uint64_t* outFrameTime) {
    // TODO(virtualscene-perf): do not create different renderers for each scene
    if (!scene || !view) {
        E("%s: invalid parameters", __FUNCTION__);
        return false;
    }

    const uint64_t frameTime = scene->getFrameTimeUs();
    if (outFrameTime) {
        *outFrameTime = frameTime;
    }

    std::lock_guard lock(view->mLock);

    auto sceneHash = scene->getVersionHashForView(view);
    if (view->mCache.isValidFor(sceneHash, frameTime)) {
        // We still need to call finish callback to let caller use the existing
        // view cache. viewCacheRequiresUpdate should be used when a final
        // copy/conversion is not needed.
        finishCallback();
        return true;
    }

    // View is not up to date, render and update the cache
    auto readbackSize = view->getWidthLocked() * view->getHeightLocked() * 4;
    view->mCache.mSceneHash = sceneHash;
    view->mCache.mFrameTime = frameTime;

    SceneConfig::Mode mode = scene->getSceneMode();
    Renderer* renderer = nullptr;
    if (SceneConfig::modeRequiresRenderer(mode)) {
        renderer = scene->getRenderer();

        // This mode requires renderer
        if (!renderer) {
            E("%s: invalid scene renderer in mode %s", __FUNCTION__,
              SceneConfig::modeToString(mode));
            return false;
        }
    }

    // Make the renderer context current for graphics operations
    const float renderTime = frameTime / 1000000.0f;
    auto context = renderer ? renderer->makeCurrent() : nullptr;
    if (renderer && !context->isValid()) {
        E("%s: Cannot use EGL context", __FUNCTION__);
        return false;
    }

    view->preRenderLocked();

    switch (mode) {
        case SceneConfig::Mode::Mesh3D:
        case SceneConfig::Mode::Image360:
        {
            const auto renderables =
                    scene->getRenderableObjects(view->mViewProjection);
            if (!renderer || !renderer->render(view, renderables, renderTime)) {
                E("Scene rendering failed");
                return false;
            }
        } break;
        case SceneConfig::Mode::ImageFile:
        case SceneConfig::Mode::VideoFile:
        case SceneConfig::Mode::Color: {
            const SceneOverlayObject* overlay = scene->getOverlayObject();
            if (!overlay || !overlay->isValid()) {
                E("Scene rendering failed");
                return false;
            }
            std::vector<uint8_t>& fbData = view->getFramebufferLocked();

            ImageScaler scaler(view->getWidthLocked(), view->getHeightLocked(),
                               fbData.data());
            auto mode = ImageScaler::ScaleMode::AspectFitZoom;
            // AspectFitZoom requires a minimum size.
            // For a single color image, just use ScaleToFill
            if (overlay->mWidth == 1 && overlay->mHeight == 1) {
                mode = ImageScaler::ScaleMode::ScaleToFill;
            }
            if (!scaler.updateImage(overlay->mWidth, overlay->mHeight,
                                    overlay->mDataRGBA.data(), mode)) {
                E("%s: Failed to resize the framebuffer for the view",
                  __FUNCTION__);
                return false;
            }
        } break;
        default: {
            E("%s: Unknown scene mode: %d", __FUNCTION__,
              static_cast<int>(mode));
        }
    }

    view->postRenderLocked();

    // This needs to be called inside the lock
    finishCallback();

    return true;
}

bool ScenesManager::removeScene(Scene* scene) {
    D("ScenesManager::%s", __func__);

    AutoLock lock(mLock);
    if (!scene || !scene->releaseResources()) {
        return false;
    }

    // Remove from mScenes array
    auto it = std::find_if(
            mScenes.begin(), mScenes.end(),
            [&scene](const auto& iter) { return iter.get() == scene; });
    if (it != mScenes.end()) {
        // Warn if the caller is not the only reference
        if (it->use_count() > 2) {
            D("Removing scene with references");
        }
        mScenes.erase(it);
    } else {
        E("%s: could not find scene", __FUNCTION__);
    }

    return true;
}

bool ScenesManager::removeAll() {
    D("ScenesManager::%s", __func__);

    // First make sure VirtualSceneManager is uninitialized, should
    // be done outside the lock
    VirtualSceneManager::uninitialize();

    // Release all scenes
    AutoLock lock(mLock);
    for (auto& it : mScenes) {
        // Warn if there are other users of the scene
        if (it.use_count() > 1) {
            D("Removing scene with references");
        }
        it->releaseResources();
    }

    mScenes.clear();

    return true;
}

/*******************************************************************************
 *                     VirtualSceneManager API.
 ******************************************************************************/

StaticLock VirtualSceneManager::mLock;
std::shared_ptr<Scene> VirtualSceneManager::mEnvironmentScene;
std::deque<std::string> VirtualSceneManager::mPosterFilenameUpdates;
std::optional<std::thread> VirtualSceneManager::mBackgroundUpdateThread;
std::function<void()> VirtualSceneManager::mUpdateCallback;
int VirtualSceneManager::mNumUsers = 0;
std::atomic<bool> VirtualSceneManager::mKeepUpdating = false;
bool VirtualSceneManager::mShowBackground = false;

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

    EnvironmentConfig envConfig =
            getEnvironmentConfig(avdInfo, warnIfMissing, mShowBackground);
    SceneConfig sceneConfig(envConfig.sceneMode, envConfig.sceneArgument);

    D("Initializing VirtualSceneManager with mode:%s, argument:%s",
      SceneConfig::modeToString(sceneConfig.mSceneMode),
      sceneConfig.mArgument.c_str());

    std::shared_ptr<Scene> scene = createEnvironmentScene(sceneConfig);

    if (!scene) {
        E("VirtualSceneManager scene could not be initialized");
        return false;
    }

    mEnvironmentScene = std::move(scene);
    mKeepUpdating = false;

    lock.unlock();

    mShowBackground = transparentDisplay;
    if (initBackgroundService) {
        int displayWidth, displayHeight;
        androidHwConfig_getScreenDimensions(hwCfg, &displayWidth,
                                            &displayHeight);
        dinfo("%s: Setting up screen background view at %dx%d", __func__,
              displayWidth, displayHeight);

        if (!BackgroundUpdateService::start(displayWidth, displayHeight,
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
    if (mEnvironmentScene) {
        ScenesManager::removeScene(mEnvironmentScene.get());
        mEnvironmentScene.reset();
    }
    mPosterFilenameUpdates.clear();
}

void VirtualSceneManager::update() {
    if (!mLock.tryLock()) {
        // Scene is in use, skip this update..
        return;
    }
    if (!mEnvironmentScene) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
    }

    // Settings::AnimationState is mainly used for TV animation in default
    // virtualscene and animation is controlled by renderTime in shaders. Always
    // update the scene and timer in other modes.
    bool updateTime = true;
    if (SceneConfig::modeSupportsAnimations(mEnvironmentScene->getSceneMode())) {
        // Use virtualscene settings for animation control
        updateTime = sSettings->getAnimationState();
    } else {
        // Static scene, no need to update which may invalidate view caches
        updateTime = false;
    }
    mEnvironmentScene->update(updateTime);

    mLock.unlock();

    // Perform any requested updates, should be called outside of the lock as
    // any lock requiring operations should hold its own lock
    if (mUpdateCallback) {
        mUpdateCallback();
    }
}

bool VirtualSceneManager::viewCacheRequiresUpdate(const RendererView* view) {
    if (!view) {
        E("%s: invalid parameters", __FUNCTION__);
        return false;
    }

    uint64_t sceneHash, frameTime;
    {
        AutoLock lock(mLock);
        sceneHash = mEnvironmentScene->getVersionHashForView(view);
        frameTime = mEnvironmentScene->getFrameTimeUs();
    }

    std::lock_guard lock(view->mLock);
    return !(view->mCache.isValidFor(sceneHash, frameTime));
}

bool VirtualSceneManager::renderView(RendererView* view,
                                     std::function<void()> finishCallback,
                                     uint64_t* outFrameTime) {
    AutoLock lock(mLock);
    if (!mEnvironmentScene) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return false;
    }

    return ScenesManager::renderView(
            mEnvironmentScene.get(), view,
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
                    Scene::LoadBehavior loadBehavior =
                            Scene::LoadBehavior::Default;
                    mEnvironmentScene->loadPoster(posterName,
                                                  setting.mFilename.c_str(),
                                                  setting.mScale, loadBehavior);
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
        mEnvironmentScene->updatePosterScale(posterName, scale);
    }
}

void VirtualSceneManager::setAnimationState(bool state) {
    AutoLock lock(mLock);
    sSettings->setAnimationState(state);
}

bool VirtualSceneManager::getAnimationState() {
    AutoLock lock(mLock);
    return sSettings->getAnimationState();
}

void VirtualSceneManager::setSceneControlsParameters(bool show) {
    AutoLock lock(mLock);
    if (!mEnvironmentScene) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return;
    }

    // Only allow showing scene controls if it's a 3d scene
    if (!show || SceneConfig::modeSupportsSceneControls(
                         mEnvironmentScene->getSceneMode())) {
        D("%s: show=%s", __func__, (show ? "true" : "false"));
        skin_winsys_show_virtual_scene_controls(show);
    }
}

bool VirtualSceneManager::addSceneUser() {
    AutoLock lock(mLock);
    if (!mEnvironmentScene) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return false;
    }
    if (mNumUsers == 0) {
        const bool sceneHasRenderer = mEnvironmentScene->getRenderer();

        // Make sure the scene is ready to use, this will also
        // crete the renderer and load renderer resources if needed
        mEnvironmentScene->loadUserResources();

        // Poster location objects must be loaded after the renderer is initialized
        if (!sceneHasRenderer) {
            // Add renderer related resources if it's the first time
            // the resources are being loaded
            if (Renderer* renderer = mEnvironmentScene->getRenderer()) {
                auto context = renderer->makeCurrent();

                //  Load the poster configuration in the scene.
                for (const auto& it : sSettings->getPosterLocations()) {
                    if (!mEnvironmentScene->createPosterLocation(it)) {
                        E("VirtualSceneManager failed to create poster location");
                        return false;
                    }
                }

                for (const auto& it : sSettings->getPosterSettings()) {
                    const char* posterName = it.first.c_str();
                    const Settings::PosterSetting& setting = it.second;
                    Scene::LoadBehavior loadBehavior = Scene::LoadBehavior::Default;
                    mEnvironmentScene->loadPoster(posterName, setting.mFilename.c_str(),
                                    setting.mScale, loadBehavior);
                }
            }
        }

        startSceneUpdateThread();
    }
    mNumUsers++;

    return true;
}

void VirtualSceneManager::removeSceneUser() {
    AutoLock lock(mLock);
    if (!mEnvironmentScene) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return nullptr;
    }
    mNumUsers--;
    if (mNumUsers == 0) {
        // Allow scene to unload resources when there are no users of it
        mEnvironmentScene->unloadUserResources();

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
    if (!mEnvironmentScene) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return SceneConfig::Mode::Unknown;
    }
    return mEnvironmentScene->getSceneMode();
}

bool VirtualSceneManager::reloadScene(const SceneConfig& config) {
    AutoLock lock(mLock);

    // Only reload if the config has changed
    if (mEnvironmentScene && mEnvironmentScene->getSceneConfig() == config) {
        D("%s: no changes to the scene config.", __func__);
        return true;
    }

    D("%s: Reloading with mode:%s, argument:%s", __func__,
      SceneConfig::modeToString(config.mSceneMode), config.mArgument.c_str());

    // Create a new scene and check if there were any errors
    auto scene = createEnvironmentScene(config);
    if (!scene) {
        E("VirtualSceneManager scene failed to reload!");
        return false;
    }

    if (mEnvironmentScene) {
        ScenesManager::removeScene(mEnvironmentScene.get());
        mEnvironmentScene.reset();
    }

    // If we're currently running, we need to load resources
    if (mNumUsers > 0) {
        scene->loadUserResources();
        scene->update(false);
    }

    // TODO(virtualscene) Handle virtual scene controls. Those should move
    // out of the camera callback and be controlled here, since the camera
    // has no knowledge of what the scene is when it changes.

    // Replace the scene, not that this is safe because we don't expose the
    // scene to the outside users and all operations are done in-sync through
    // VirtualSceneManager interface
    mEnvironmentScene = scene;

    D("%s: finished", __func__);

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

    // Update background view, if exists
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

std::shared_ptr<Scene> VirtualSceneManager::createEnvironmentScene(
        const SceneConfig& config) {
    std::shared_ptr<Scene> scene = ScenesManager::createScene(config);

    if (!scene) {
        E("VirtualSceneManager scene could not be initialized");
        return nullptr;
    }

    return scene;
}

/*******************************************************************************
 *                     ScenesManager API.
 ******************************************************************************/
std::unique_ptr<SceneCamera> BackgroundUpdateService::mSceneCamera;
std::unique_ptr<RendererView> BackgroundUpdateService::mBackgroundView;
std::vector<uint8_t> BackgroundUpdateService::mReadbackDataCopy;

bool BackgroundUpdateService::mStarted = false;

bool BackgroundUpdateService::start(int displayWidth,
                                    int displayHeight,
                                    float backgroundBlur) {
    const float aspectRatio = static_cast<float>(displayWidth) / displayHeight;
    mSceneCamera = std::make_unique<SceneCamera>();
    mSceneCamera->setAspectRatio(aspectRatio);

    // TODO(virtualscene): do not call renderView if it's a static
    // image, adjust fps based on environment.ini
    mBackgroundView = std::make_unique<RendererView>();
    mBackgroundView->updateTarget(RendererView::Format::RGBA8, displayWidth,
                                  displayHeight);
    mBackgroundView->setBlurFactor(backgroundBlur);
    mReadbackDataCopy.resize(displayWidth * displayHeight * 4);

    // Set update callback, to update the background image after each
    // scene update
    VirtualSceneManager::setUpdateCallback([displayWidth, displayHeight]() {
        const bool supportsPosition = (VirtualSceneManager::getSceneMode() ==
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
        glm::mat4 viewProjection = mSceneCamera->getProjection() * cameraView;
        mBackgroundView->updateViewProjection(viewProjection);

        if (VirtualSceneManager::viewCacheRequiresUpdate(
                    mBackgroundView.get())) {
            if (VirtualSceneManager::renderView(
                        mBackgroundView.get(),
                        []() {
                            mReadbackDataCopy =
                                    mBackgroundView->getFramebufferLocked();
                        },
                        nullptr)) {
                // Update the background image for the display composition
                // TODO(virtualscene-perf): Avoid copy of the data by making
                // android_setOpenglesScreenBackground call lighter weight and
                // callable inside the lock
                android_setOpenglesScreenBackground(displayWidth, displayHeight,
                                                    mReadbackDataCopy.data());
            }
        }
    });

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
    mBackgroundView.reset();
    mSceneCamera.reset();
    mStarted = false;
}

void BackgroundUpdateService::updateBlurAmount(float blurAmount) {
    if (mBackgroundView) {
        mBackgroundView->setBlurFactor(blurAmount);
    }
}

int VirtualSceneManager::getSceneBaseRotationLocked() {
    if (!mEnvironmentScene) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return 0;
    } else {
        return mEnvironmentScene->getSceneRotation();
    }
}

int VirtualSceneManager::getSceneBaseRotation() {
    AutoLock lock(mLock);
    return getSceneBaseRotationLocked();
}

}  // namespace virtualscene
}  // namespace android
