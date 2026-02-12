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
#include "android/cmdline-option.h"
#include "host-common/FeatureControl.h"
#include "host-common/hw-config.h"
#include "host-common/hw-config-helper.h"
#include "host-common/opengles.h"
#include "android/console.h"
#include "android/skin/winsys.h"
#include "android/utils/debug.h"
#include "android/virtualscene/PosterInfo.h"
#include "android/virtualscene/Renderer.h"
#include "android/virtualscene/Scene.h"
#include "android/virtualscene/Scene.h"
#include "android/camera/camera-virtualscene-utils.h"

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

StaticLock VirtualSceneManager::mLock;
VirtualSceneManagerImpl* VirtualSceneManager::mImpl = nullptr;

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

// All functions in this class must be called under the
// VirtualSceneManager::mLock lock.
class VirtualSceneManagerImpl {
private:
    VirtualSceneManagerImpl(std::unique_ptr<Renderer>&& renderer,
                            std::unique_ptr<Scene>&& scene);

public:
    ~VirtualSceneManagerImpl();

    static std::unique_ptr<VirtualSceneManagerImpl> create();

    void update();

    bool renderView(RendererView* view,
                    float renderTime,
                    std::function<void()> finishCallback);

    // Queue an update to a poster filename that should be executed on the
    // render thread.
    void queuePosterUpdate(const char* posterName);

    // Update the poster scale.
    void updatePosterScale(const char* posterName, float scale);

    // Load a PosterSetting in the scene.
    void loadPosterInternal(const char* posterName,
                            const Settings::PosterSetting& setting,
                            Scene::LoadBehavior loadBehavior);

private:
    std::unique_ptr<Renderer> mRenderer;
    std::unique_ptr<Scene> mScene;

    std::deque<std::string> mPosterFilenameUpdates;
};

VirtualSceneManagerImpl::VirtualSceneManagerImpl(
        std::unique_ptr<Renderer>&& renderer,
        std::unique_ptr<Scene>&& scene)
    : mRenderer(std::move(renderer)), mScene(std::move(scene)) {
    // Load the poster configuration in the scene.
    for (const auto& it : sSettings->getPosterSettings()) {
        loadPosterInternal(it.first.c_str(), it.second,
                           Scene::LoadBehavior::Synchronous);
    }
}

VirtualSceneManagerImpl::~VirtualSceneManagerImpl() {
    mScene->releaseResources();
}

std::unique_ptr<VirtualSceneManagerImpl> VirtualSceneManagerImpl::create() {
    std::unique_ptr<Renderer> renderer = Renderer::create();
    if (!renderer) {
        E("VirtualSceneManager renderer failed to construct");
        return nullptr;
    }

    // Make the renderer context current for graphics operations
    auto context = renderer->makeCurrent();

    std::unique_ptr<Scene> scene = Scene::create(*renderer.get());
    if (!scene) {
        E("VirtualSceneManager scene failed to load");
        return nullptr;
    }

    for (const auto& it : sSettings->getPosterLocations()) {
        if (!scene->createPosterLocation(it)) {
            E("VirtualSceneManager failed to create poster location");
            return nullptr;
        }
    }

    return std::unique_ptr<VirtualSceneManagerImpl>(
            new VirtualSceneManagerImpl(std::move(renderer), std::move(scene)));
}

void VirtualSceneManagerImpl::update() {
    mScene->update();
}

bool VirtualSceneManagerImpl::renderView(RendererView* view,
                                         float renderTime,
                                         std::function<void()> finishCallback) {
    std::lock_guard lock(view->mLock);

    auto sceneHash = mScene->getVersionHashForView(view);
    if (!view->mCache.isValidFor(sceneHash)) {
        // View is not up to date,need to render again.

        // Update cache with the render results
        auto readbackSize = view->getWidthLocked() * view->getHeightLocked() * 4;
        view->mCache.mSceneHash = sceneHash;
        view->mCache.mFramebufferRGBA8.resize(readbackSize);

        // Make the renderer context current for graphics operations
        auto context = mRenderer->makeCurrent();
        if (!context->isValid()) {
            derror("%s: Cannot use EGL context", __FUNCTION__);
            return false;
        }

        // Apply any pending updates to the scene.  This must be done on the
        // OpenGL thread.
        const auto& posters = sSettings->getPosterSettings();
        while (!mPosterFilenameUpdates.empty()) {
            const std::string& posterName = mPosterFilenameUpdates.front();
            loadPosterInternal(posterName.c_str(), posters.at(posterName),
                               Scene::LoadBehavior::Default);
            mPosterFilenameUpdates.pop_front();
        }

        const auto renderables =
                mScene->getRenderableObjects(view->mViewProjection);
        if (!mRenderer->render(view, renderables, renderTime)) {
            derror("Scene rendering failed");
            return false;
        }
    }

    // TODO(virtualscene): check the hash at higher level to avoid copies&conversions
    finishCallback();

    return true;
}

void VirtualSceneManagerImpl::queuePosterUpdate(const char* posterName) {
    mPosterFilenameUpdates.push_back(posterName);
}

void VirtualSceneManagerImpl::updatePosterScale(const char* posterName,
                                                float scale) {
    mScene->updatePosterScale(posterName, scale);
}

void VirtualSceneManagerImpl::loadPosterInternal(
        const char* posterName,
        const Settings::PosterSetting& setting,
        Scene::LoadBehavior loadBehavior) {
    if (setting.mFilename.empty()) {
        // Always render empty posters at 100% scale.
        mScene->loadPoster(posterName, nullptr, 1.0f, loadBehavior);
    } else {
        mScene->loadPoster(posterName, setting.mFilename.c_str(),
                           setting.mScale, loadBehavior);
    }
}

/*******************************************************************************
 *                     VirtualSceneManager API.
 ******************************************************************************/

void VirtualSceneManager::parseCmdline() {
    AutoLock lock(mLock);
    if (sSettings.hasInstance()) {
        E("VirtualSceneManager settings already loaded");
        return;
    }

    if (!getConsoleAgents()->settings->has_cmdLineOptions()) {
        return;
    }

    if (!androidHwConfig_hasVirtualSceneCamera(getConsoleAgents()->settings->hw()) &&
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

bool VirtualSceneManager::initialize() {
    AutoLock lock(mLock);
    if (mImpl) {
        E("VirtualSceneManager already initialized");
        return false;
    }

    D("Initializing VirtualSceneManager");
    mImpl = VirtualSceneManagerImpl::create().release();
    return mImpl != nullptr;
}

bool VirtualSceneManager::isInitialized() {
    AutoLock lock(mLock);
    return (mImpl != nullptr);
}

void VirtualSceneManager::uninitialize() {
    // TODO(virtualscene): correctly call uninitialize at exit
    AutoLock lock(mLock);
    if (!mImpl) {
        E("VirtualSceneManager not initialized");
        return;
    }

    // To make sure it's released the same way it was created, attach to a
    // unique_ptr and let that release it.
    D("Uninitializing VirtualSceneManager");
    std::unique_ptr<VirtualSceneManagerImpl> stateReleaser(mImpl);
    mImpl = nullptr;
}

void VirtualSceneManager::update() {
    AutoLock lock(mLock);
    if (!mImpl) {
        E("VirtualSceneManager not initialized");
        return 0L;
    }

    return mImpl->update();
}

bool VirtualSceneManager::renderView(RendererView* view,
                                     float renderTime,
                                     std::function<void()> finishCallback) {
    AutoLock lock(mLock);
    if (!mImpl) {
        E("VirtualSceneManager not initialized");
        return false;
    }

    return mImpl->renderView(view, renderTime, finishCallback);
}

void VirtualSceneManager::setInitialPoster(const char* posterName,
                                           const char* filename) {
    AutoLock lock(mLock);
    sSettings->setInitialPoster(posterName, filename);

    // If the scene is active, it will update the poster in the next render()
    // invocation.
    if (mImpl) {
        mImpl->queuePosterUpdate(posterName);
    }
}

bool VirtualSceneManager::loadPoster(const char* posterName,
                                     const char* filename) {
    AutoLock lock(mLock);
    sSettings->setPoster(posterName, filename);

    // If the scene is active, it will update the poster in the next render()
    // invocation.
    if (mImpl) {
        mImpl->queuePosterUpdate(posterName);
    }

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
    if (mImpl) {
        mImpl->updatePosterScale(posterName, scale);
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

void VirtualSceneManager::showSceneControls(bool show) {
    // TODO(virtualscene): reconsider how to enable/disable controls
    dprint("%s: %s", __func__, (show ? "true" : "false"));
    skin_winsys_show_virtual_scene_controls(show);
}

}  // namespace virtualscene
}  // namespace android
