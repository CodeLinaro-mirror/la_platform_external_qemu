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

#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

/*
 * Defines the Virtual Scene, used by the Virtual Scene Camera
 */

#include "aemu/base/memory/LazyInstance.h"
#include "aemu/base/synchronization/Lock.h"
#include "android/emulation/control/virtual_scene_agent.h"
#include "android/utils/compiler.h"
#include "android/virtualscene/Renderer.h"

namespace android {
namespace virtualscene {

// Forward declarations.
struct SceneConfig;
class SceneCamera;

// ScenesManager is responsible for creating, rendering and destroying
// scenes. Clients can create and render scenes through this interface
// without any interaction with the 'environment' scene, like posters..etc.
class ScenesManager {
public:
    // Parse command line options for the virtual scene.
    static std::shared_ptr<Scene> createScene(std::string_view sceneName,
                                              const SceneConfig& config);

    // Render the virtual scene to the given view.
    static bool renderView(Scene* scene,
                           RendererView* view,
                           std::function<void()> finishCallback,
                           uint64_t* outFrameTime);

    static bool removeScene(Scene* scene);

    static bool removeAll();

private:
    static android::base::StaticLock mLock;
    static std::vector<std::shared_ptr<Scene>> mScenes;
};

// VirtualSceneManager is responsible for 'virtualscene', which is a special
// type of shared scene and can support changing posters.
class VirtualSceneManager {
public:
    // Parse command line options for the virtual scene.
    static void parseCmdline();

    // Initialize virtual scene rendering.
    // Returns true if initialization succeeded.
    static bool initialize(const SceneConfig& config);

    // Uninitialize virtual scene rendering, may be called on any thread, but
    // the same EGL context that was active when initialize() was called must be
    // active. This function modifies the GL state, callers must be resilient to
    // that.
    static void uninitialize();

    // Update scene animations and poster changes
    static void update();

    // Check if the view's cache requires update due to view/scene changes
    static bool viewCacheRequiresUpdate(const RendererView* view);

    // Render the virtual scene to the given view.
    static bool renderView(RendererView* view,
                           std::function<void()> finishCallback,
                           uint64_t* outFrameTime);

    // Set the initial poster of the scene, loaded from persisted settings.
    // Command line flags take precedence, so if the -virtualscene-poster flag
    // has been specified for the posterName this call will not replace it.
    //
    // |posterName| - Name of the poster position, such as "wall" or "table".
    // |filename| - Path to an image file, either PNG or JPEG, or nullptr
    //              to set to default.
    static void setInitialPoster(const char* posterName, const char* filename);

    // Load a poster into the scene from a file.  This may be called on any
    // thread.  Changes to the scene will happen the next time that render()
    // is called.
    //
    // |posterName| - Name of the poster position, such as "wall" or "table".
    // |filename| - Path to an image file, either PNG or JPEG.
    //
    // Returns true on success.
    static bool loadPoster(const char* posterName, const char* filename);

    // Enumerate posters in the scene and their current values.  Synchronously
    // calls the callback for each poster in the scene.
    //
    // |context| - Context object, passed into callback.
    // |callback| - Callback to be invoked for each poster.
    static void enumeratePosters(void* context,
                                 EnumeratePostersCallback callback);

    // Set the scale of a poster.
    //
    // |posterName| - Name of the poster position, such as "wall" or "table".
    // |scale| - Poster scale, a value between 0 and 1. The value will be
    //          clamped between the poster's minimum size and 1.
    static void setPosterScale(const char* posterName, float scale);

    // Enable/Disable the TV animation.
    //
    // |state| - State of the TV animation. True to enable TV animation, false
    // to disable.
    static void setAnimationState(bool state);

    // Get the TV animation state.
    //
    // Returns true for enabled, false for disabled.
    static bool getAnimationState();

    static void setSceneControlsParameters(bool show);

    static std::shared_ptr<Scene> addSceneUser();
    static void removeSceneUser();

    static void setUpdateCallback(std::function<void()> callback);

private:
    static android::base::StaticLock mLock;
    static std::shared_ptr<Scene> mEnvironmentScene;
    static std::deque<std::string> mPosterFilenameUpdates;
    static std::optional<std::thread> mBackgroundUpdateThread;
    static std::function<void()> mUpdateCallback;
    static int mNumUsers;
    static std::atomic<bool> mKeepUpdating;

    static void updateSceneWorker();
    static void startSceneUpdateThread();
    static void stopSceneUpdateThread();
};

// TODO(virtualscene): move into a regular service
class BackgroundUpdateService {
public:
    static bool start(int displayWidth,
                      int displayHeight,
                      float backgroundBlur);
    static void stop();

private:
    static std::unique_ptr<SceneCamera> mSceneCamera;
    static std::unique_ptr<RendererView> mBackgroundView;
    static bool mStarted;
};

}  // namespace virtualscene
}  // namespace android
