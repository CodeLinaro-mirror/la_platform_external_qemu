/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "OpenGLESDispatch/EGLDispatch.h"
#include "OpenGLESDispatch/GLESv2Dispatch.h"
#include "RenderTarget.h"
#include "Renderer.h"
#include "TextureUtils.h"
#include "VulkanDispatch.h"

#include "aemu/base/synchronization/Lock.h"
#include "aemu/base/synchronization/MessageChannel.h"
#include "aemu/base/threads/WorkerThread.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace android {
namespace ver {

class ScopedEglContext;

class RendererGLES : public Renderer {
    DISALLOW_COPY_AND_ASSIGN(RendererGLES);

public:
    RendererGLES(int width,
                 int height,
                 const std::filesystem::path& vulkanBasePath);
    ~RendererGLES() override;

    static std::unique_ptr<RendererGLES> create(
            const std::filesystem::path& vulkanBasePath);

    bool initialize();

    // Renderer public API.
    float getAspectRatio() override;
    bool isTextureLoaded(Texture texture) override;
    void getTextureInfo(Texture texture,
                        uint32_t* outWidth,
                        uint32_t* outHeight) override;

    void releaseTexture(Texture texture) override;
    void releaseMaterial(Material material) override;
    void releaseMesh(Mesh mesh) override;

    Material createMaterialCheckerboard() override;
    Material createMaterialTextured() override;
    Material createMaterialScreenSpace(const char* frag) override;

    Mesh createMesh(const VertexPositionUV* vertices,
                    size_t verticesSize,
                    const uint32_t* indices,
                    size_t indicesSize) override;

    bool updateMesh(Mesh mesh,
                    const VertexPositionUV* vertices,
                    size_t verticesSize) override;

    Texture loadTexture(const char* filename) override;
    Texture loadTextureAsync(const char* filename) override;
    Texture createTextureRGBA(const uint8_t* rgba,
                              uint32_t width,
                              uint32_t height) override;
    Texture duplicateTexture(Texture texture) override;

    bool render(RendererView* view,
                const std::vector<RenderableObject>& renderables,
                float time) override;

    std::unique_ptr<RendererContext> makeCurrent() override;

private:
    struct MaterialData {
        GLuint program = 0;

        GLint positionLocation = -1;
        GLint uvLocation = -1;
        GLint texSamplerLocation = -1;
        GLint mvpLocation = -1;
        GLint resolutionLocation = -1;
        GLint timeLocation = -1;
    };

    struct MeshData {
        VertexInfo mVertexInfo;
        GLuint mVertexBuffer = 0;
        GLuint mIndexBuffer = 0;
        size_t mIndexCount = 0;
    };

    enum class TextureState { Placeholder, Loaded };

    struct TextureData {
        size_t mRefCount = 0;
        TextureState mState = TextureState::Loaded;
        GLuint mTextureId = 0;
        std::string mFilename;
        uint32_t mWidth = 0;
        uint32_t mHeight = 0;
    };

    struct RendererHeldResources {
        std::vector<Material> materials;
        std::vector<Mesh> meshes;
        std::vector<Texture> textures;
    };

    enum class LoaderCommandType { Shutdown, LoadTexture };

    struct LoaderCommand {
        LoaderCommandType mType;
        int mHandle = -1;

        LoaderCommand(LoaderCommandType type, int handle)
            : mType(type), mHandle(handle) {}
    };

    using RenderableParameterCallback =
            std::function<void(const MaterialData& material)>;

    void dispatchToRenderThread(std::function<void()>&& workItem);

    android::base::WorkerProcessingResult onLoaderCommand(
            LoaderCommand&& command);

    void releaseMaterialInternal(Material material);
    bool isStandardMaterial(Material material);
    bool isTextureSizeValid(uint32_t width, uint32_t height);
    Texture tryGetCachedTexture(const char* filename);
    Texture createEmptyTexture(uint32_t width, uint32_t height);
    Texture createTextureInternal(TextureState state,
                                  const char* filename,
                                  const TextureUtils::Result& data);
    void onLoaderLoadTexture(Texture texture);
    bool replaceTextureInternal(Texture texture,
                                const TextureUtils::Result& data);

    GLuint compileShader(GLenum type, const char* shaderSource);
    GLuint linkShaders(GLuint vertexId, GLuint fragmentId);
    GLint getAttribLocation(GLuint program,
                            const char* name,
                            bool optional = false);
    GLint getUniformLocation(GLuint program, const char* name);

    GLuint getTextureId(Texture texture) const;

    void processRenderable(const Renderable& renderable,
                           RenderableParameterCallback parameterCallback);

    const int mRenderWidth;
    const int mRenderHeight;
    const std::filesystem::path mVulkanBasePath;

    VulkanDispatchTable mVk;
    std::unique_ptr<RenderTarget> mRenderTargets[2];
    std::unique_ptr<RenderTarget> mScreenRenderTarget;
    Mesh mEffectsMesh;
    std::vector<Material> mEffectsChain;

    android::base::WorkerThread<LoaderCommand> mLoaderThread;
    android::base::MessageChannel<std::function<void()>, 10>
            mRenderThreadDispatcherQueue;

    android::base::Lock mResourceLock;
    int mNextResourceId = 0;
    std::unordered_map<int, MaterialData> mMaterials;
    std::unordered_map<int, MeshData> mMeshes;
    std::unordered_map<int, TextureData> mTextures;
    std::unordered_map<std::string, int> mTextureCache;

    RendererHeldResources mRendererResources;
    Material mMaterialTextured;

    struct EglState {
        ~EglState() { destroy(); }

        bool initialize(int frameWidth, int frameHeight);
        void destroy();
        std::unique_ptr<RendererContext> makeEglCurrent();

        const gfxstream::host::gl::EGLDispatch* mEglDispatch = nullptr;
        const gfxstream::host::gl::GLESv2Dispatch* mGles2 = nullptr;

        EGLDisplay mEglDisplay = EGL_NO_DISPLAY;
        EGLContext mEglContext = EGL_NO_CONTEXT;
        EGLSurface mEglSurface = EGL_NO_SURFACE;
        bool mEglInitialized = false;
    } mGL;
    const gfxstream::host::gl::GLESv2Dispatch* mGles2 = nullptr;
    GLint mMaxTextureSize = 0;
};

}  // namespace ver
}  // namespace android
