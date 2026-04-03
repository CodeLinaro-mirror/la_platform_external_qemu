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

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "android/avd/info.h"  // to resolve avd path for resources
#include "android/base/system/System.h"
#include "android/camera/camera-metrics.h"
#include "android/console.h"
#include "android/loadpng.h"
#include "android/raw_image_sources/image_file/raw_image_file_source.h"
#include "android/raw_image_sources/raw_image_source.h"
#include "android/raw_image_sources/video_file/raw_video_file_source.h"
#include "android/utils/debug.h"
#include "android/virtualscene/MeshSceneObject.h"
#include "android/virtualscene/Renderer.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>

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
static constexpr const char* kDefaultSceneObj = "Toren1BD.obj";
static constexpr const char* kDefaultImageFile = "default.png";
static constexpr const char* kDefaultVideoFile = "default.mp4";

// A blank background
static constexpr const char* kDefaultColor = "#000000";

// Function to find fullpath from a filename for the scene
// Scene filenames can be provided as fullpaths, AVD local,
// or from 'in 'resources' folder.
std::string resolveSceneFilename(const std::string& sceneFilename) {
    // Check if it's a fullpath
    fs::path inputPath(sceneFilename);
    if (fs::exists(inputPath)) {
        return inputPath.string();
    }

    // If it's not a usable full path, try AVD local
    const char* avdBasePath =
            getConsoleAgents() && getConsoleAgents()->settings
                    ? avdInfo_getContentPath(
                              getConsoleAgents()->settings->avdInfo())
                    : nullptr;
    if (avdBasePath) {
        fs::path avdPath = fs::path(avdBasePath) / inputPath;
        if (fs::exists(avdPath)) {
            return avdPath.string();
        }
    }

    // If not in AVD folder, check 'resources' folder
    fs::path resourcesBasePath =
            fs::path(System::get()->getLauncherDirectory()) / "resources";
    fs::path resourcePath = resourcesBasePath / inputPath;
    if (fs::exists(resourcePath)) {
        return resourcePath.string();
    }

    dwarning("Could not resolve environment scene filename '%s'",
             sceneFilename.c_str());
    return sceneFilename;
}

// static_cast the value in a unique_ptr.
// After this call, the unique_ptr that the value is cast from will be removed.
template <typename To, typename From>
std::unique_ptr<To> static_unique_cast(std::unique_ptr<From>& from) {
    return std::unique_ptr<To>(static_cast<To*>(from.release()));
}

namespace android {
namespace virtualscene {

SceneConfig::SceneConfig(Mode mode, std::string_view argument) {
    mSceneMode = mode;
    mArgument = argument;
}

SceneConfig::Mode SceneConfig::modeFromString(std::string_view sceneModeStr) {
    if (sceneModeStr == "virtualscene") {
        return SceneConfig::Mode::Mesh3D;
    } else if (sceneModeStr == "mesh3d") {
        return SceneConfig::Mode::Mesh3D;
    } else if (sceneModeStr == "videoplayback") {
        return SceneConfig::Mode::VideoPlayback;
    } else if (sceneModeStr == "videofile") {
        return SceneConfig::Mode::VideoFile;
    } else if (sceneModeStr == "imagefile") {
        return SceneConfig::Mode::ImageFile;
    } else if (sceneModeStr == "color") {
        return SceneConfig::Mode::Color;
    } else {
        dwarning("Unknown scene mode requested: %s", sceneModeStr);
        return SceneConfig::Mode::Unknown;
    }
}

const char* SceneConfig::modeToString(SceneConfig::Mode mode) {
    if (mode == SceneConfig::Mode::Mesh3D) {
        return "mesh3d";
    } else if (mode == SceneConfig::Mode::VideoPlayback) {
        return "videoplayback";
    } else if (mode == SceneConfig::Mode::VideoFile) {
        return "videofile";
    } else if (mode == SceneConfig::Mode::ImageFile) {
        return "imagefile";
    } else if (mode == SceneConfig::Mode::Color) {
        return "color";
    } else {
        return "unknown";
    }
}

const char* SceneConfig::defaultArgumentForMode(SceneConfig::Mode mode) {
    if (mode == SceneConfig::Mode::Mesh3D) {
        return kDefaultSceneObj;
    } else if (mode == SceneConfig::Mode::VideoPlayback ||
               mode == SceneConfig::Mode::VideoFile) {
        return kDefaultVideoFile;
    } else if (mode == SceneConfig::Mode::ImageFile) {
        return kDefaultImageFile;
    } else if (mode == SceneConfig::Mode::Color) {
        return kDefaultColor;
    } else {
        derror("%s: Invalid mode %d", __func__, (int)mode);
        return "invalid_filename";
    }
}

bool SceneConfig::modeRequiresRenderer(SceneConfig::Mode mode) {
    // Currently, only Mesh3D requires a renderer
    return (mode == SceneConfig::Mode::Mesh3D);
}

bool SceneConfig::modeSupportViewRotations(SceneConfig::Mode mode) {
    // Currently, only Mesh3D supports view rotations
    return (mode == SceneConfig::Mode::Mesh3D);
}

bool SceneConfig::modeSupportAnimations(SceneConfig::Mode mode) {
    // Output of the ImageFile and Color modes won't be affected by the
    // animations
    return (mode != SceneConfig::Mode::ImageFile &&
            mode != SceneConfig::Mode::Color);
}

Scene::Scene(std::unique_ptr<Renderer> renderer, const SceneConfig& config)
    : mRenderer(std::move(renderer)), mConfig(config) {
    D("%s: creating Scene", __func__);
}

Scene::~Scene() {
    D("%s: destroying Scene", __func__);
    if (mSceneObjects.size() || mRawImageSource || mOverlayObject) {
        // releaseResources should have been called!
        E("%s: Scene resources are not released!", __func__);
    }
    mRenderer.reset();
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
    dprint("Initializing scene with '%s' mode, argument:%s", sceneModeStr,
           mConfig.mArgument.c_str());

    // Use scene mode name for metrics
    // TODO(virtualscene): decide and add new metrics without PII
    CameraMetrics::instance().setVirtualSceneName(sceneModeStr);

    const auto sceneMode = getSceneMode();

    // Find the file, in case it's given as a local path
    std::string sceneFilename;
    if (sceneMode != SceneConfig::Mode::Color) {
        sceneFilename = resolveSceneFilename(mConfig.mArgument);
    }

    bool needsRawImageSource = false;
    switch (sceneMode) {
        case SceneConfig::Mode::Unknown: {
            derror("%s: Unknown scene mode!", __func__);
        } break;
        case SceneConfig::Mode::Mesh3D: {
            if (mRenderer == nullptr) {
                derror("%s: No renderer for scene: %s", __func__,
                       sceneFilename.c_str());
                return false;
            }
            std::unique_ptr<MeshSceneObject> sceneObject =
                    MeshSceneObject::load(*mRenderer, sceneFilename.c_str());
            if (!sceneObject) {
                derror("%s: Could not load scene object: %s", __func__,
                       sceneFilename.c_str());
                return false;
            }

            mSceneObjects.push_back(
                    std::move(static_unique_cast<SceneObject>(sceneObject)));

            // TODO (virtualScene) The virtual scene by default renders the
            // image rotated 90 degrees
            mBaseRotation = 90;
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
        case SceneConfig::Mode::VideoFile: {
            needsRawImageSource = true;
            mRawImageSource = RawVideofileSource::Create(sceneFilename);
        } break;
        case SceneConfig::Mode::ImageFile: {
            needsRawImageSource = true;
            mRawImageSource = RawImageFileSource::Create(sceneFilename);
        } break;
        case SceneConfig::Mode::Color: {
            needsRawImageSource = true;
            unsigned int r, g, b;
            if (sscanf(mConfig.mArgument.c_str(), "#%02x%02x%02x", &r, &g,
                       &b) == 3) {
                mRawImageSource = std::make_unique<SolidColorImageSource>(
                        Color{(uint8_t)r, (uint8_t)g, (uint8_t)b});
            } else {
                derror("%s: Could not parse color: %s", __func__,
                       mConfig.mArgument.c_str());
            }
        } break;
        default:
            dwarning("%s: Unhandled scene mode %d", __func__, (int)sceneMode);
    }

    if (needsRawImageSource) {
        if (!mRawImageSource) {
            derror("%s: Could not load background source: '%s', falling back to default",
                   __func__, mConfig.mArgument.c_str());
            mRawImageSource =
                    std::make_unique<SolidColorImageSource>(kErrorColor);
            mConfig.mSceneMode = SceneConfig::Mode::ImageFile;
        }
        mBaseRotation = mRawImageSource->GetBaseRotation();

        // Set up an initial black image
        mOverlayObject = std::make_unique<SceneOverlayObject>();
        mOverlayObject->mHeight = 1;
        mOverlayObject->mWidth = 1;
        mOverlayObject->mDataRGBA = {0x00, 0x00, 0x00, 0xFF};
    }

    mStartTimeUs = System::get()->getUnixTimeUs();
    mFrameTimeUs = 0;

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

    mRawImageSource.reset();
    mOverlayObject.reset();

    mObjectsVersion++;

    return true;
}

void Scene::update(bool updateTime) {
    // TODO(virtualscene-video): this should play the video in video mode
    for (auto& poster : mPosters) {
        poster.second.sceneObject->update();
    }

    if (updateTime) {
        // TODO(virtualscene): use ThreadLooper::nowNs(ClockType::kVirtual) ?
        mFrameTimeUs = System::get()->getUnixTimeUs() - mStartTimeUs;
        if (mRawImageSource) {
            int64_t animationLength = mRawImageSource->GetAnimationLengthUs();
            if (animationLength > 0 && mFrameTimeUs > animationLength) {
                mFrameTimeUs %= mRawImageSource->GetAnimationLengthUs();
            }
        }
    } else {
        // While paused, move our start time so we resume with the same value;
        mStartTimeUs = System::get()->getUnixTimeUs() - mFrameTimeUs;
    }

    if (mRawImageSource) {
        auto res = mRawImageSource->UpdateImage(
                mFrameTimeUs, mRawImageSourceToken,
                [&](const RawImageBufferView* buffer) {
                    if (buffer->pixel_format != V4L2_PIX_FMT_RGB32) {
                        return absl::InvalidArgumentError(absl::StrFormat(
                                "Unsupported pixel format from image source: %d",
                                buffer->pixel_format));
                    }

                    if (mOverlayObject->mDataRGBA.size() <
                        buffer->buffer_size) {
                        mOverlayObject->mDataRGBA.resize(buffer->buffer_size);
                        mOverlayObject->mWidth = buffer->width;
                        mOverlayObject->mHeight = buffer->height;
                    }

                    std::memcpy(mOverlayObject->mDataRGBA.data(),
                                buffer->buffer, buffer->buffer_size);
                    return absl::OkStatus();
                });
        if (res.ok()) {
            if (res->has_value()) {
                mRawImageSourceToken = res.value();
                mObjectsVersion++;
            }
        } else {
            E("Failed to Update Image Source: %s", res.status().message());
        }
    }
}

uint64_t Scene::getVersionHashForView(
        const RendererView* /*lockedView*/) const {
    const uint64_t sceneHash = reinterpret_cast<uint64_t>(this);
    // TODO(virtualscene-perf): check if the objects inside the view frustum
    // includes any changes/animations
    return (mObjectsVersion ^ sceneHash);
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
    if (mConfig.mSceneMode != SceneConfig::Mode::Mesh3D) {
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

// TODO(virtualscene-perf): implement load/unload user resources functions
// to reduce memory usage of the scene when there are no users of it
void Scene::loadUserResources() {
    dprint("%s", __FUNCTION__);
    if (mRawImageSource) {
        // TODO(virtualscene) Determine if we need these input values.
        // Currently they are just hints that we may use to better initialize
        // the webcam to a sensible resolution.
        mRawImageSource->Start(0, 0, 0);
    }
}

void Scene::unloadUserResources() {
    dprint("%s", __FUNCTION__);
    if (mRawImageSource) {
        mRawImageSource->Stop();
    }
}

}  // namespace virtualscene
}  // namespace android
