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

#include "android/virtualscene/Scene.h"

#include "android/base/system/System.h"
#include "android/camera/camera-metrics.h"
#include "android/loadpng.h"
#include "android/utils/debug.h"
#include "android/virtualscene/MeshSceneObject.h"
#include "android/virtualscene/Renderer.h"

#include <cmath>
#include <filesystem>

using namespace android::base;
using android::camera::CameraMetrics;
namespace fs = std::filesystem;

#define E(...) derror(__VA_ARGS__)
#define W(...) dwarning(__VA_ARGS__)
#define D(...) VERBOSE_PRINT(virtualscene, __VA_ARGS__)
#define D_ACTIVE VERBOSE_CHECK(virtualscene)

// Default filenames for different scene modes, can be used
// when the file cannot be found or loaded, all relative to
// the emulator's 'resources' folder
// TODO(virtualscene): create and use proper default image&video
static constexpr const char* kDefaultSceneObj = "Toren1BD.obj";
static constexpr const char* kDefaultImageFile = "poster.png";
static constexpr const char* kDefaultVideoFile =
        "macroPreviews/Reset_position.mp4";

// static_cast the value in a unique_ptr.
// After this call, the unique_ptr that the value is cast from will be removed.
template <typename To, typename From>
std::unique_ptr<To> static_unique_cast(std::unique_ptr<From>& from) {
    return std::unique_ptr<To>(static_cast<To*>(from.release()));
}

namespace android {
namespace virtualscene {

SceneConfig::SceneConfig(Mode mode, std::string_view filename) {
    mSceneMode = mode;
    mFilename = filename;
}

SceneConfig::Mode SceneConfig::modeFromString(std::string_view sceneModeStr) {
    if (sceneModeStr == "virtualscene") {
        return SceneConfig::Mode::Mesh3dScene;
    } else if (sceneModeStr == "videoplayback") {
        return SceneConfig::Mode::VideoPlayback;
    } else if (sceneModeStr == "imagefile") {
        return SceneConfig::Mode::ImageFile;
    } else {
        dwarning("Unknown scene mode requested: %s", sceneModeStr);
        return SceneConfig::Mode::Unknown;
    }
}

const char* SceneConfig::modeToString(SceneConfig::Mode mode) {
    if (mode == SceneConfig::Mode::Mesh3dScene) {
        return "mesh3dscene";
    } else if (mode == SceneConfig::Mode::VideoPlayback) {
        return "videoplayback";
    } else if (mode == SceneConfig::Mode::ImageFile) {
        return "imagefile";
    } else {
        return "unknown";
    }
}

const char* SceneConfig::defaultFilenameForMode(SceneConfig::Mode mode) {
    if (mode == SceneConfig::Mode::Mesh3dScene) {
        return kDefaultSceneObj;
    } else if (mode == SceneConfig::Mode::VideoPlayback) {
        return kDefaultVideoFile;
    } else if (mode == SceneConfig::Mode::ImageFile) {
        return kDefaultImageFile;
    } else {
        derror("%s: Invalid mode %d", __func__, (int)mode);
        return "invalid_filename";
    }
}


Scene::Scene(std::unique_ptr<Renderer> renderer, const SceneConfig& config)
    : mRenderer(std::move(renderer)), mConfig(config) {
    D("%s: creating Scene", __func__);
}

Scene::~Scene() {
    D("%s: destroying Scene", __func__);
    if (mSceneObjects.size() || mOverlayObject) {
        // releaseResources should have been called!
        E("%s: Scene resources are not released!", __func__);
    }
}

std::unique_ptr<Scene> Scene::create(std::unique_ptr<Renderer> renderer,
                                     const SceneConfig& config) {
    std::unique_ptr<Scene> scene;
    scene.reset(new Scene(std::move(renderer), config));
    if (!scene || !scene->initialize()) {
        return nullptr;
    }

    return scene;
}

bool Scene::initialize() {
    const char* sceneModeStr = SceneConfig::modeToString(mConfig.mSceneMode);
    dprint("Initializing scene with '%s' mode, file:%s", sceneModeStr,
           mConfig.mFilename.c_str());

    // Use scene mode name for metrics
    // TODO(virtualscene): decide and add new metrics without PII
    CameraMetrics::instance().setVirtualSceneName(sceneModeStr);

    // Find the file, in case it's given as a local path
    const std::string sceneFilename = mConfig.mFilename;
    const auto sceneMode = getSceneMode();

    switch (sceneMode) {
        case SceneConfig::Mode::Unknown: {
            derror("%s: Unknown scene mode!", __func__);
        } break;
        case SceneConfig::Mode::Mesh3dScene: {
            if (mRenderer == nullptr) {
                derror("%s: No renderer for scene: %s", __func__,
                       sceneFilename.c_str());
                return false;
            }
            std::unique_ptr<MeshSceneObject> sceneObject =
                    MeshSceneObject::load(*mRenderer,
                                          sceneFilename.c_str());
            if (!sceneObject) {
                derror("%s: Could not load scene object: %s", __func__,
                       sceneFilename.c_str());
                return false;
            }

            mSceneObjects.push_back(
                    std::move(static_unique_cast<SceneObject>(sceneObject)));
        } break;
        case SceneConfig::Mode::VideoPlayback: {
            // TODO(virtualscene-video): actually load sceneFilename video,
            // and change render()
            if (!fs::exists(sceneFilename)) {
                derror("%s: Could not load video file: %s", __func__,
                       sceneFilename.c_str());
                return false;
            }
        } break;
        case SceneConfig::Mode::ImageFile: {
            uint32_t width = 0;
            uint32_t height = 0;
            void* backgroundImageData =
                    loadpng(sceneFilename.c_str(), &width, &height);
            if (!backgroundImageData) {
                derror("%s: Could not load background image: %s", __func__,
                       mConfig.mFilename.c_str());
                return false;
            }
            mOverlayObject = std::make_unique<SceneOverlayObject>();
            mOverlayObject->mWidth = width;
            mOverlayObject->mHeight = height;
            mOverlayObject->mDataRGBA.resize(width * height * 4);
            memcpy(mOverlayObject->mDataRGBA.data(), backgroundImageData,
                   mOverlayObject->mDataRGBA.size());
            free(backgroundImageData);
        } break;
        default:
            dwarning("%s: Unhandled scene mode %d", __func__, (int)sceneMode);
    }

    mObjectsVersion++;

    return true;
}

bool Scene::releaseResources() {
    auto context = mRenderer ? mRenderer->makeCurrent() : nullptr;
    if (mRenderer) {
        if (!context->isValid()) {
            E("%s: Cannot use EGL context", __FUNCTION__);
            return false;
        }

        for (auto& poster : mPosters) {
            mRenderer->releaseTexture(poster.second.texture);
            mRenderer->releaseTexture(poster.second.defaultTexture);

            if (poster.second.sceneObject) {
                poster.second.sceneObject.reset();
            }
        }
    }

    mSceneObjects.clear();

    mOverlayObject.reset();

    mObjectsVersion++;

    return true;
}

void Scene::update() {
    // TODO(virtualscene-video): this should play the video in video mode
    for (auto& poster : mPosters) {
        poster.second.sceneObject->update();
    }
}

uint64_t Scene::getVersionHashForView(
        const RendererView* /*lockedView*/) const {
    // TODO(virtualscene-perf): check if the objects inside the view frustum includes
    // any changes/animations
    return mObjectsVersion;
}

std::vector<RenderableObject> Scene::getRenderableObjects(
        const glm::mat4& viewProjection) const {
    std::vector<RenderableObject> renderables;

    for (auto& sceneObject : mSceneObjects) {
        getRenderableObjectsFromSceneObject(viewProjection, sceneObject.get(),
                                            renderables);
    }

    for (auto& poster : mPosters) {
        if (poster.second.sceneObject) {
            getRenderableObjectsFromSceneObject(viewProjection,
                                                poster.second.sceneObject.get(),
                                                renderables);
        }
    }

    return std::move(renderables);
}

bool Scene::createPosterLocation(const PosterInfo& info) {
    if (mConfig.mSceneMode != SceneConfig::Mode::Mesh3dScene) {
        // Scene mode doesn't support poster locations, not an error
        return true;
    }
    if (!mRenderer) {
        return false;
    }
    PosterStorage storage;
    storage.sceneObject =
            PosterSceneObject::create(*mRenderer, info.position, info.rotation,
                                      kPosterMinimumSizeMeters, info.size);
    if (!storage.sceneObject) {
        W("%s: Failed to create poster scene object %s.", __FUNCTION__,
          info.name.c_str());
        return false;
    }

    if (!info.defaultFilename.empty()) {
        storage.defaultTexture =
                mRenderer->loadTextureAsync(info.defaultFilename.c_str());
        storage.sceneObject->setTexture(storage.defaultTexture);
    }

    mPosters.insert(std::make_pair(info.name, std::move(storage)));
    return true;
}

bool Scene::loadPoster(const char* posterName,
                       const char* filename,
                       float scale,
                       LoadBehavior loadBehavior) {
    auto it = mPosters.find(posterName);
    if (it == mPosters.end()) {
        W("%s: Could not find poster with name '%s'", __FUNCTION__, posterName);
        return false;
    }

    PosterStorage& poster = it->second;

    mRenderer->releaseTexture(poster.texture);
    poster.texture = Texture();

    if (filename && strlen(filename) > 0) {
        poster.texture = loadBehavior == LoadBehavior::Synchronous
                                 ? mRenderer->loadTexture(filename)
                                 : mRenderer->loadTextureAsync(filename);
    } else {
        // Always render empty posters at 100% scale.
        scale = 1.0f;
    }

    poster.sceneObject->setScale(scale);
    poster.sceneObject->setTexture(
            poster.texture.isValid() ? poster.texture : poster.defaultTexture);

    mObjectsVersion++;

    return true;
}

void Scene::updatePosterScale(const char* posterName, float scale) {
    auto it = mPosters.find(posterName);
    if (it == mPosters.end()) {
        W("%s: Could not find poster with name '%s'", __FUNCTION__, posterName);
        return;
    }

    PosterStorage& poster = it->second;
    poster.sceneObject->setScale(scale);

    mObjectsVersion++;
}

void Scene::getRenderableObjectsFromSceneObject(
        const glm::mat4& viewProjection,
        const SceneObject* sceneObject,
        std::vector<RenderableObject>& outRenderableObjects) {
    if (sceneObject->isVisible()) {
        const glm::mat4 mvp = viewProjection * sceneObject->getTransform();

        for (const Renderable& renderable : sceneObject->getRenderables()) {
            outRenderableObjects.push_back({mvp, renderable});
        }
    }
}

}  // namespace virtualscene
}  // namespace android
