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
#include "android/base/system/System.h"
#include "android/camera/camera-virtualscene-utils.h"
#include "android/cmdline-option.h"
#include "android/console.h"
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
#include <unordered_map>

using namespace android::base;

#define E(...) derror(__VA_ARGS__)
#define W(...) dwarning(__VA_ARGS__)
#define D(...) VERBOSE_PRINT(virtualscene, __VA_ARGS__)
#define D_ACTIVE VERBOSE_CHECK(virtualscene)

static constexpr const char* kPosterFile = "Toren1BD.posters";

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

/*******************************************************************************
 *                     ScenesManager API.
 ******************************************************************************/

StaticLock ScenesManager::mLock;
std::vector<std::shared_ptr<Scene>> ScenesManager::mScenes;

std::shared_ptr<Scene> ScenesManager::createScene(std::string_view sceneName,
                                                  const SceneConfig& config) {
    if (config.mSceneMode == SceneConfig::Mode::Unknown) {
        E("%s: invalid config", __func__);
        return nullptr;
    }

    // Create a new scene
    D("Initializing a scene with mode:%s, filename:%s",
      SceneConfig::modeToString(config.mSceneMode), config.mFilename.c_str());

    // Only initialize a renderer for the scene if GL is required
    bool rendererRequired =
            SceneConfig::modeRequiresRenderer(config.mSceneMode);

    std::unique_ptr<Renderer> renderer = nullptr;
    if (rendererRequired) {
        renderer = Renderer::create();
        if (!renderer) {
            E("VirtualSceneManager renderer failed to construct");
            return nullptr;
        }
    }

    // Make the renderer context current for graphics operations
    auto context = renderer ? renderer->makeCurrent() : nullptr;
    if (context && !context->isValid()) {
        E("%s: Cannot use EGL context", __FUNCTION__);
        return nullptr;
    }

    std::shared_ptr<Scene> scene = Scene::create(std::move(renderer), config);
    if (!scene) {
        E("VirtualSceneManager scene failed to load");
        return nullptr;
    }

    // Return existing one if the same scene was already created
    AutoLock lock(mLock);
    mScenes.push_back(scene);

    return scene;
}

bool ScenesManager::renderView(Scene* scene,
                               RendererView* view,
                               float renderTime,
                               std::function<void()> finishCallback) {
    // TODO(virtualscene-perf): do not create different renderers for each scene
    if (!scene || !view) {
        E("%s: invalid parameters", __FUNCTION__);
        return false;
    }

    std::lock_guard lock(view->mLock);

    auto sceneHash = scene->getVersionHashForView(view);
    if (view->mCache.isValidFor(sceneHash, renderTime)) {
        // TODO(virtualscene-perf): check the hash at higher level to avoid
        // copies&conversions
        finishCallback();
        return true;
    }

    // View is not up to date,need to render again.

    // Update cache with the render results
    auto readbackSize = view->getWidthLocked() * view->getHeightLocked() * 4;
    view->mCache.mSceneHash = sceneHash;
    view->mCache.mRenderTime = renderTime;

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
    auto context = renderer ? renderer->makeCurrent() : nullptr;
    if (renderer && !context->isValid()) {
        E("%s: Cannot use EGL context", __FUNCTION__);
        return false;
    }

    view->preRenderLocked();

    switch (mode) {
        case SceneConfig::Mode::Mesh3dScene: {
            const auto renderables =
                    scene->getRenderableObjects(view->mViewProjection);
            if (!renderer || !renderer->render(view, renderables, renderTime)) {
                E("Scene rendering failed");
                return false;
            }
        } break;
        case SceneConfig::Mode::VideoPlayback: {
            // TODO(virtualscene-video): create video playback scene and render
            // a view Renders a procedural animation for now..
            const int dummyVideoWidth = view->getWidthLocked();
            const int dummyVideoHeight = view->getHeightLocked();
            const int stride = dummyVideoWidth * 4;
            std::vector<uint8_t>& fbData = view->getFramebufferLocked();
            if (fbData.size() < dummyVideoWidth * dummyVideoHeight * 4) {
                // preRenderLocked failed
                E("Scene rendering failed");
                return false;
            }
            for (int y = 0; y < dummyVideoHeight; y++) {
                for (int x = 0; x < dummyVideoWidth; x++) {
                    uint8_t& r = fbData[(y * dummyVideoWidth + x) * 4 + 0];
                    uint8_t& g = fbData[(y * dummyVideoWidth + x) * 4 + 1];
                    uint8_t& b = fbData[(y * dummyVideoWidth + x) * 4 + 2];
                    uint8_t& a = fbData[(y * dummyVideoWidth + x) * 4 + 3];

                    float u = (x / (float)dummyVideoWidth) * 10.0;
                    float v = (y / (float)dummyVideoHeight) * 10.0;
                    float local_u = u - floor(u);
                    float local_v = v - floor(v);
                    float dist = abs(local_u - 0.5) + abs(local_v - 0.5);
                    float threshold =
                            0.1 + 0.4 * (0.5 + 0.5 * sin(renderTime * 6.283));
                    float mask = (dist < threshold) ? 1.0 : 0.0;

                    r = (uint8_t)(mask * 100);
                    g = (uint8_t)(mask * 200);
                    b = (uint8_t)(mask * 255);
                    a = 255;
                }
            }
        } break;
        case SceneConfig::Mode::ImageFile: {
            const SceneOverlayObject* overlay = scene->getOverlayObject();
            if (!overlay || !overlay->isValid()) {
                E("Scene rendering failed");
                return false;
            }
            std::vector<uint8_t>& fbData = view->getFramebufferLocked();

            ImageScaler scaler(view->getWidthLocked(), view->getHeightLocked(),
                               fbData.data());
            if (!scaler.updateImage(overlay->mWidth, overlay->mHeight,
                                    overlay->mDataRGBA.data(),
                                    ImageScaler::ScaleMode::ScaleToFill)) {
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
    D("%s", __func__);

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
    D("%s", __func__);

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
int VirtualSceneManager::mNumUsers = 0;

void VirtualSceneManager::parseCmdline() {
    AutoLock lock(mLock);
    if (sSettings.hasInstance()) {
        E("VirtualSceneManager settings already loaded");
        return;
    }

    if (!getConsoleAgents()->settings->has_cmdLineOptions()) {
        return;
    }

    if (!androidHwConfig_hasVirtualSceneCamera(
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

bool VirtualSceneManager::initialize(const SceneConfig& config) {
    AutoLock lock(mLock);
    if (mEnvironmentScene) {
        E("VirtualSceneManager already initialized");
        return false;
    }

    D("Initializing VirtualSceneManager with mode:%s, filename:%s",
      SceneConfig::modeToString(config.mSceneMode), config.mFilename.c_str());

    std::shared_ptr<Scene> scene =
            ScenesManager::createScene("environment", config);

    if (!scene) {
        E("VirtualSceneManager scene could not be initialized");
        return false;
    }

    if (Renderer* renderer = scene->getRenderer()) {
        auto context = renderer->makeCurrent();

        //  Load the poster configuration in the scene.
        for (const auto& it : sSettings->getPosterLocations()) {
            if (!scene->createPosterLocation(it)) {
                E("VirtualSceneManager failed to create poster location");
                return false;
            }
        }

        for (const auto& it : sSettings->getPosterSettings()) {
            const char* posterName = it.first.c_str();
            const Settings::PosterSetting& setting = it.second;
            Scene::LoadBehavior loadBehavior = Scene::LoadBehavior::Default;
            scene->loadPoster(posterName, setting.mFilename.c_str(),
                              setting.mScale, loadBehavior);
        }
    }

    mEnvironmentScene = std::move(scene);

    return true;
}

void VirtualSceneManager::uninitialize() {
    D("Uninitializing VirtualSceneManager");
    AutoLock lock(mLock);
    if (mEnvironmentScene) {
        ScenesManager::removeScene(mEnvironmentScene.get());
        mEnvironmentScene.reset();
    }
    mPosterFilenameUpdates.clear();
}

void VirtualSceneManager::update() {
    AutoLock lock(mLock);
    if (!mEnvironmentScene) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return 0L;
    }

    return mEnvironmentScene->update();
}

bool VirtualSceneManager::renderView(RendererView* view,
                                     float renderTime,
                                     std::function<void()> finishCallback) {
    AutoLock lock(mLock);
    if (!mEnvironmentScene) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return false;
    }

    return ScenesManager::renderView(
            mEnvironmentScene.get(), view, renderTime, [&]() {
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
            });
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

    // Only allow showing scene controls if it's a mesh3d scene
    if (!show ||
        (mEnvironmentScene->getSceneMode() == SceneConfig::Mode::Mesh3dScene)) {
        dprint("%s: show=%s", __func__, (show ? "true" : "false"));
        skin_winsys_show_virtual_scene_controls(show);
    }
}

std::shared_ptr<Scene> VirtualSceneManager::addSceneUser() {
    AutoLock lock(mLock);
    if (!mEnvironmentScene) {
        E("%s:%d VirtualSceneManager not initialized", __func__, __LINE__);
        return nullptr;
    }
    if (mNumUsers == 0) {
        // Make sure the scene is ready to use
        mEnvironmentScene->loadUserResources();
    }
    mNumUsers++;

    return mEnvironmentScene;
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
    }
}

}  // namespace virtualscene
}  // namespace android
