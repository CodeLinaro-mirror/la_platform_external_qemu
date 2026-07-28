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

#include "Renderer.h"
#include "TextureUtils.h"
#include "VulkanDispatch.h"
#include "VulkanShaders.h"

#include "aemu/base/synchronization/Lock.h"
#include "aemu/base/threads/WorkerThread.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace android {
namespace ver {

class RendererVulkan : public Renderer {
    DISALLOW_COPY_AND_ASSIGN(RendererVulkan);

public:
    RendererVulkan(int width,
                   int height,
                   const std::filesystem::path& vulkanBasePath = {});
    ~RendererVulkan() override;

    static std::unique_ptr<RendererVulkan> create(
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
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkShaderModule vertShader = VK_NULL_HANDLE;
        VkShaderModule fragShader = VK_NULL_HANDLE;
        bool isScreenSpace = false;
    };

    struct MeshData {
        VertexInfo mVertexInfo;
        VkBuffer mVertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mVertexMemory = VK_NULL_HANDLE;
        VkBuffer mIndexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mIndexMemory = VK_NULL_HANDLE;
        size_t mIndexCount = 0;
    };

    enum class TextureState { Placeholder, Loaded };

    struct TextureData {
        size_t mRefCount = 0;
        TextureState mState = TextureState::Loaded;
        VkImage mImage = VK_NULL_HANDLE;
        VkDeviceMemory mImageMemory = VK_NULL_HANDLE;
        VkImageView mImageView = VK_NULL_HANDLE;
        VkDescriptorSet mDescriptorSet = VK_NULL_HANDLE;
        std::string mFilename;
        uint32_t mWidth = 0;
        uint32_t mHeight = 0;
    };


    enum class LoaderCommandType { Shutdown, LoadTexture };

    struct LoaderCommand {
        LoaderCommandType mType;
        int mHandle = -1;

        LoaderCommand(LoaderCommandType type, int handle)
            : mType(type), mHandle(handle) {}
    };

    android::base::WorkerProcessingResult onLoaderCommand(
            LoaderCommand&& command);

    void releaseMaterialInternal(Material material);
    bool isStandardMaterial(Material material);
    Texture tryGetCachedTexture(const char* filename);
    Texture createEmptyTexture(uint32_t width, uint32_t height);
    Texture createTextureInternal(TextureState state,
                                  const char* filename,
                                  const TextureUtils::Result& data);
    void onLoaderLoadTexture(Texture texture);
    bool replaceTextureInternal(Texture texture,
                                const TextureUtils::Result& data);

    void uploadTextureImage(TextureData& texData,
                            const TextureUtils::Result& data);

    uint32_t findMemoryType(uint32_t typeFilter,
                            VkMemoryPropertyFlags properties);

    bool createVulkanBuffer(VkDeviceSize size,
                            VkBufferUsageFlags usage,
                            VkMemoryPropertyFlags properties,
                            VkBuffer& buffer,
                            VkDeviceMemory& bufferMemory);

    bool createVulkanImage(uint32_t width,
                           uint32_t height,
                           VkFormat format,
                           VkImageUsageFlags usage,
                           VkMemoryPropertyFlags properties,
                           VkImage& image,
                           VkDeviceMemory& imageMemory,
                           VkImageView& imageView);

    void submitCommandBuffer(VkCommandBuffer cmdBuffer,
                             VkFence fence,
                             bool waitIdle = false);

    void executeSingleTimeCommands(
            VkCommandBuffer cmdBuffer,
            VkFence fence,
            const std::function<void(VkCommandBuffer)>& recordFunc,
            bool waitIdle = false);

    void transitionImageLayout(
            VkCommandBuffer cmdBuffer,
            VkImage image,
            VkImageLayout oldLayout,
            VkImageLayout newLayout,
            VkAccessFlags srcAccessMask,
            VkAccessFlags dstAccessMask,
            VkPipelineStageFlags srcStageMask,
            VkPipelineStageFlags dstStageMask,
            VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT);

    void drawRenderables(VkCommandBuffer cmd,
                         const std::vector<RenderableObject>& renderables,
                         float time);

    VkShaderModule createShaderModule(const uint32_t* spirvCode,
                                      size_t codeSize);

    bool createGraphicsPipeline(VkShaderModule vertShader,
                                VkShaderModule fragShader,
                                VkPipelineLayout pipelineLayout,
                                VkFormat colorFormat,
                                VkFormat depthFormat,
                                VkPipeline& pipeline);

    const int mRenderWidth;
    const int mRenderHeight;
    const std::filesystem::path mVulkanBasePath;

    VulkanDispatchTable mVk;
    VkInstance mInstance = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mMemoryProperties = {};
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mQueue = VK_NULL_HANDLE;
    uint32_t mQueueFamilyIndex = 0;

    VkCommandPool mCommandPool = VK_NULL_HANDLE;
    VkCommandBuffer mCommandBuffer = VK_NULL_HANDLE;
    VkFence mFence = VK_NULL_HANDLE;

    VkCommandPool mLoaderCommandPool = VK_NULL_HANDLE;
    VkCommandBuffer mLoaderCommandBuffer = VK_NULL_HANDLE;
    VkFence mLoaderFence = VK_NULL_HANDLE;
    android::base::Lock mQueueLock;

    VkSampler mSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool mDescriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout mMeshPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout mScreenPipelineLayout = VK_NULL_HANDLE;

    VkImage mOffscreenColorImage = VK_NULL_HANDLE;
    VkDeviceMemory mOffscreenColorMemory = VK_NULL_HANDLE;
    VkImageView mOffscreenColorView = VK_NULL_HANDLE;

    VkImage mOffscreenDepthImage = VK_NULL_HANDLE;
    VkDeviceMemory mOffscreenDepthMemory = VK_NULL_HANDLE;
    VkImageView mOffscreenDepthView = VK_NULL_HANDLE;

    VkBuffer mStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory mStagingMemory = VK_NULL_HANDLE;

    android::base::WorkerThread<LoaderCommand> mLoaderThread;

    android::base::Lock mResourceLock;
    int mNextResourceId = 0;
    std::unordered_map<int, MaterialData> mMaterials;
    std::unordered_map<int, MeshData> mMeshes;
    std::unordered_map<int, TextureData> mTextures;
    std::unordered_map<std::string, int> mTextureCache;
    Material mMaterialTextured;
    Texture mDefaultTexture;
    bool mInitialized = false;
};

}  // namespace ver
}  // namespace android
