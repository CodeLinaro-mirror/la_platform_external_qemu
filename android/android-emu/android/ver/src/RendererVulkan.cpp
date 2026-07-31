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

#include "RendererVulkan.h"
#include "ScenesManager.h"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_set>

#include <libyuv.h>

#include "aemu/base/files/PathUtils.h"
#include "android/base/system/System.h"

using namespace android::base;

#define E(...) derror(__VA_ARGS__)
#define W(...) dwarning(__VA_ARGS__)
#define D(...) dprint(__VA_ARGS__)

static constexpr int kRendererDefaultFramebufferWidth = 1024;
static constexpr int kRendererDefaultFramebufferHeight = 1024;

static uint64_t durationUsToMs(uint64_t startUs, uint64_t endUs) {
    assert(startUs < endUs);
    const uint64_t durationUs = (endUs - startUs);
    return durationUs / 1000;
}

namespace android {
namespace ver {

class VulkanRendererContext : public RendererContext {
public:
    VulkanRendererContext(bool valid) : mValid(valid) {}
    bool isValid() const override { return mValid; }

private:
    bool mValid;
};

std::unique_ptr<RendererVulkan> RendererVulkan::create(
        const std::filesystem::path& vulkanBasePath) {
    dprint("virtualscene: Creating Vulkan renderer.");
    std::unique_ptr<RendererVulkan> renderer(new RendererVulkan(
            kRendererDefaultFramebufferWidth, kRendererDefaultFramebufferHeight,
            vulkanBasePath));
    if (!renderer->initialize()) {
        derror("virtualscene: could not create Vulkan renderer.");
        return nullptr;
    }
    return renderer;
}

RendererVulkan::RendererVulkan(int width,
                               int height,
                               const std::filesystem::path& vulkanBasePath)
    : mRenderWidth(width),
      mRenderHeight(height),
      mVulkanBasePath(vulkanBasePath),
      mLoaderThread([this](LoaderCommand&& command) {
          return onLoaderCommand(std::move(command));
      }) {}

bool RendererVulkan::initialize() {
    if (!mVk.initDriver(mVulkanBasePath)) {
        derror("VER: Cannot initialize Vulkan dispatcher.");
        return false;
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VER";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "VirtualEnvironmentRenderer";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instanceInfo = {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;

    std::vector<const char*> extNames;
    VkResult res = VK_SUCCESS;

#ifdef __APPLE__
    // Add portability enumeration extension if available.
    uint32_t instanceExtCount = 0;
    res = mVk.vkEnumerateInstanceExtensionProperties(
            nullptr, &instanceExtCount, nullptr);
    if (res != VK_SUCCESS || instanceExtCount == 0) {
        derror("VER: Cannot enumerate Vulkan instance extensions.");
        return false;
    }
    std::vector<VkExtensionProperties> instanceExts(instanceExtCount);
    res = mVk.vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtCount,
                                                     instanceExts.data());
    if (res != VK_SUCCESS) {
        derror("VER: Cannot enumerate Vulkan instance extensions.");
        return false;
    }
    for (const auto& ext : instanceExts) {
        if (strcmp(ext.extensionName,
                   VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            instanceInfo.flags |=
                    VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            extNames.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            break;
        }
    }
#endif

    instanceInfo.enabledExtensionCount = (uint32_t)extNames.size();
    instanceInfo.ppEnabledExtensionNames = extNames.data();

    res = mVk.vkCreateInstance(&instanceInfo, nullptr, &mInstance);
    if (res != VK_SUCCESS || !mInstance) {
        derror("vkCreateInstance failed: %d", res);
        return false;
    }

    if (!mVk.initInstance(mInstance)) {
        derror("initInstance failed.");
        return false;
    }

    uint32_t deviceCount = 0;
    mVk.vkEnumeratePhysicalDevices(mInstance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        derror("No physical Vulkan devices found.");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    mVk.vkEnumeratePhysicalDevices(mInstance, &deviceCount, devices.data());
    mPhysicalDevice = devices[0];
    mVk.vkGetPhysicalDeviceMemoryProperties(mPhysicalDevice,
                                            &mMemoryProperties);

    VkPhysicalDeviceProperties deviceProperties = {};
    mVk.vkGetPhysicalDeviceProperties(mPhysicalDevice, &deviceProperties);
    dinfo("VER: Using Vulkan device: %s, driver version: %u",
          deviceProperties.deviceName, deviceProperties.driverVersion);

    uint32_t queueFamilyCount = 0;
    mVk.vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice,
                                                 &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    mVk.vkGetPhysicalDeviceQueueFamilyProperties(
            mPhysicalDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            mQueueFamilyIndex = i;
            break;
        }
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = mQueueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceVulkan13Features features13 = {};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;

    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features13;

    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &features2;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;

    res = mVk.vkCreateDevice(mPhysicalDevice, &deviceInfo, nullptr, &mDevice);
    if (res != VK_SUCCESS || !mDevice) {
        derror("vkCreateDevice failed: %d", res);
        return false;
    }

    if (!mVk.initDevice(mDevice)) {
        derror("initDevice failed.");
        return false;
    }

    mVk.vkGetDeviceQueue(mDevice, mQueueFamilyIndex, 0, &mQueue);

    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = mQueueFamilyIndex;

    if (mVk.vkCreateCommandPool(mDevice, &poolInfo, nullptr, &mCommandPool) !=
        VK_SUCCESS) {
        derror("vkCreateCommandPool failed.");
        return false;
    }

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = mCommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (mVk.vkAllocateCommandBuffers(mDevice, &allocInfo, &mCommandBuffer) !=
        VK_SUCCESS) {
        derror("vkAllocateCommandBuffers failed.");
        return false;
    }

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    if (mVk.vkCreateFence(mDevice, &fenceInfo, nullptr, &mFence) !=
        VK_SUCCESS) {
        derror("vkCreateFence failed.");
        return false;
    }

    if (mVk.vkCreateCommandPool(mDevice, &poolInfo, nullptr,
                                &mLoaderCommandPool) != VK_SUCCESS) {
        derror("vkCreateCommandPool loader failed.");
        return false;
    }

    VkCommandBufferAllocateInfo loaderAllocInfo = {};
    loaderAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    loaderAllocInfo.commandPool = mLoaderCommandPool;
    loaderAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    loaderAllocInfo.commandBufferCount = 1;

    if (mVk.vkAllocateCommandBuffers(mDevice, &loaderAllocInfo,
                                     &mLoaderCommandBuffer) != VK_SUCCESS) {
        derror("vkAllocateCommandBuffers loader failed.");
        return false;
    }

    if (mVk.vkCreateFence(mDevice, &fenceInfo, nullptr, &mLoaderFence) !=
        VK_SUCCESS) {
        derror("vkCreateFence loader failed.");
        return false;
    }

    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (mVk.vkCreateSampler(mDevice, &samplerInfo, nullptr, &mSampler) !=
        VK_SUCCESS) {
        derror("vkCreateSampler failed.");
        return false;
    }

    VkDescriptorSetLayoutBinding layoutBinding = {};
    layoutBinding.binding = 0;
    layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    layoutBinding.descriptorCount = 1;
    layoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &layoutBinding;

    if (mVk.vkCreateDescriptorSetLayout(mDevice, &layoutInfo, nullptr,
                                        &mDescriptorSetLayout) != VK_SUCCESS) {
        derror("vkCreateDescriptorSetLayout failed.");
        return false;
    }

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 100;

    VkDescriptorPoolCreateInfo descPoolInfo = {};
    descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descPoolInfo.maxSets = 100;
    descPoolInfo.poolSizeCount = 1;
    descPoolInfo.pPoolSizes = &poolSize;

    if (mVk.vkCreateDescriptorPool(mDevice, &descPoolInfo, nullptr,
                                   &mDescriptorPool) != VK_SUCCESS) {
        derror("vkCreateDescriptorPool failed.");
        return false;
    }

    VkPushConstantRange meshPushConstant = {};
    meshPushConstant.stageFlags =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    meshPushConstant.offset = 0;
    meshPushConstant.size = 128;

    VkPipelineLayoutCreateInfo meshPipelineLayoutInfo = {};
    meshPipelineLayoutInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    meshPipelineLayoutInfo.setLayoutCount = 1;
    meshPipelineLayoutInfo.pSetLayouts = &mDescriptorSetLayout;
    meshPipelineLayoutInfo.pushConstantRangeCount = 1;
    meshPipelineLayoutInfo.pPushConstantRanges = &meshPushConstant;

    if (mVk.vkCreatePipelineLayout(mDevice, &meshPipelineLayoutInfo, nullptr,
                                   &mMeshPipelineLayout) != VK_SUCCESS) {
        derror("vkCreatePipelineLayout mesh failed.");
        return false;
    }

    VkPushConstantRange screenPushConstant = {};
    screenPushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    screenPushConstant.offset = 0;
    screenPushConstant.size = 16;

    VkPipelineLayoutCreateInfo screenPipelineLayoutInfo = {};
    screenPipelineLayoutInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    screenPipelineLayoutInfo.setLayoutCount = 1;
    screenPipelineLayoutInfo.pSetLayouts = &mDescriptorSetLayout;
    screenPipelineLayoutInfo.pushConstantRangeCount = 1;
    screenPipelineLayoutInfo.pPushConstantRanges = &screenPushConstant;

    if (mVk.vkCreatePipelineLayout(mDevice, &screenPipelineLayoutInfo, nullptr,
                                   &mScreenPipelineLayout) != VK_SUCCESS) {
        derror("vkCreatePipelineLayout screen failed.");
        return false;
    }

    if (!createVulkanImage(
                mRenderWidth, mRenderHeight, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mOffscreenColorImage,
                mOffscreenColorMemory, mOffscreenColorView)) {
        derror("createVulkanImage offscreen color failed.");
        return false;
    }

    if (!createVulkanImage(mRenderWidth, mRenderHeight, VK_FORMAT_D32_SFLOAT,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           mOffscreenDepthImage, mOffscreenDepthMemory,
                           mOffscreenDepthView)) {
        derror("createVulkanImage offscreen depth failed.");
        return false;
    }

    const VkDeviceSize stagingBufferSize = mRenderWidth * mRenderHeight * 4;
    if (!createVulkanBuffer(stagingBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            mStagingBuffer, mStagingMemory)) {
        derror("createVulkanBuffer staging failed.");
        return false;
    }

    mDefaultTexture = createEmptyTexture(1, 1);
    if (!mDefaultTexture.isValid()) {
        derror("Failed creating default texture.");
        return false;
    }

    mMaterialTextured = createMaterialTextured();
    if (!mMaterialTextured.isValid()) {
        derror("Failed creating material textured.");
        return false;
    }

    mLoaderThread.start();
    mInitialized = true;
    return true;
}

RendererVulkan::~RendererVulkan() {
    mLoaderThread.enqueue(LoaderCommand(LoaderCommandType::Shutdown, -1));
    mLoaderThread.join();

    mMaterialTextured = Material();
    mDefaultTexture = Texture();

    if (mDevice) {
        mVk.vkDeviceWaitIdle(mDevice);

        std::unordered_set<VkPipelineLayout> destroyedLayouts;
        if (mMeshPipelineLayout) {
            mVk.vkDestroyPipelineLayout(mDevice, mMeshPipelineLayout, nullptr);
            destroyedLayouts.insert(mMeshPipelineLayout);
            mMeshPipelineLayout = VK_NULL_HANDLE;
        }
        if (mScreenPipelineLayout) {
            mVk.vkDestroyPipelineLayout(mDevice, mScreenPipelineLayout,
                                        nullptr);
            destroyedLayouts.insert(mScreenPipelineLayout);
            mScreenPipelineLayout = VK_NULL_HANDLE;
        }

        for (auto& pair : mMaterials) {
            if (pair.second.pipeline)
                mVk.vkDestroyPipeline(mDevice, pair.second.pipeline, nullptr);
            if (pair.second.pipelineLayout &&
                destroyedLayouts.insert(pair.second.pipelineLayout).second) {
                mVk.vkDestroyPipelineLayout(mDevice, pair.second.pipelineLayout,
                                            nullptr);
            }
            if (pair.second.vertShader)
                mVk.vkDestroyShaderModule(mDevice, pair.second.vertShader,
                                          nullptr);
            if (pair.second.fragShader)
                mVk.vkDestroyShaderModule(mDevice, pair.second.fragShader,
                                          nullptr);
        }
        mMaterials.clear();

        for (auto& pair : mMeshes) {
            if (pair.second.mVertexBuffer)
                mVk.vkDestroyBuffer(mDevice, pair.second.mVertexBuffer,
                                    nullptr);
            if (pair.second.mVertexMemory)
                mVk.vkFreeMemory(mDevice, pair.second.mVertexMemory, nullptr);
            if (pair.second.mIndexBuffer)
                mVk.vkDestroyBuffer(mDevice, pair.second.mIndexBuffer, nullptr);
            if (pair.second.mIndexMemory)
                mVk.vkFreeMemory(mDevice, pair.second.mIndexMemory, nullptr);
        }
        mMeshes.clear();

        for (auto& pair : mTextures) {
            if (pair.second.mImageView)
                mVk.vkDestroyImageView(mDevice, pair.second.mImageView,
                                       nullptr);
            if (pair.second.mImage)
                mVk.vkDestroyImage(mDevice, pair.second.mImage, nullptr);
            if (pair.second.mImageMemory)
                mVk.vkFreeMemory(mDevice, pair.second.mImageMemory, nullptr);
        }
        mTextures.clear();
        mTextureCache.clear();

        if (mOffscreenColorView) {
            mVk.vkDestroyImageView(mDevice, mOffscreenColorView, nullptr);
            mOffscreenColorView = VK_NULL_HANDLE;
        }
        if (mOffscreenColorImage) {
            mVk.vkDestroyImage(mDevice, mOffscreenColorImage, nullptr);
            mOffscreenColorImage = VK_NULL_HANDLE;
        }
        if (mOffscreenColorMemory) {
            mVk.vkFreeMemory(mDevice, mOffscreenColorMemory, nullptr);
            mOffscreenColorMemory = VK_NULL_HANDLE;
        }

        if (mOffscreenDepthView) {
            mVk.vkDestroyImageView(mDevice, mOffscreenDepthView, nullptr);
            mOffscreenDepthView = VK_NULL_HANDLE;
        }
        if (mOffscreenDepthImage) {
            mVk.vkDestroyImage(mDevice, mOffscreenDepthImage, nullptr);
            mOffscreenDepthImage = VK_NULL_HANDLE;
        }
        if (mOffscreenDepthMemory) {
            mVk.vkFreeMemory(mDevice, mOffscreenDepthMemory, nullptr);
            mOffscreenDepthMemory = VK_NULL_HANDLE;
        }

        if (mStagingBuffer) {
            mVk.vkDestroyBuffer(mDevice, mStagingBuffer, nullptr);
            mStagingBuffer = VK_NULL_HANDLE;
        }
        if (mStagingMemory) {
            mVk.vkFreeMemory(mDevice, mStagingMemory, nullptr);
            mStagingMemory = VK_NULL_HANDLE;
        }

        if (mSampler) {
            mVk.vkDestroySampler(mDevice, mSampler, nullptr);
            mSampler = VK_NULL_HANDLE;
        }
        if (mDescriptorPool) {
            mVk.vkDestroyDescriptorPool(mDevice, mDescriptorPool, nullptr);
            mDescriptorPool = VK_NULL_HANDLE;
        }
        if (mDescriptorSetLayout) {
            mVk.vkDestroyDescriptorSetLayout(mDevice, mDescriptorSetLayout,
                                             nullptr);
            mDescriptorSetLayout = VK_NULL_HANDLE;
        }

        if (mFence) {
            mVk.vkDestroyFence(mDevice, mFence, nullptr);
            mFence = VK_NULL_HANDLE;
        }
        if (mCommandPool) {
            mVk.vkDestroyCommandPool(mDevice, mCommandPool, nullptr);
            mCommandPool = VK_NULL_HANDLE;
        }
        if (mLoaderFence) {
            mVk.vkDestroyFence(mDevice, mLoaderFence, nullptr);
            mLoaderFence = VK_NULL_HANDLE;
        }
        if (mLoaderCommandPool) {
            mVk.vkDestroyCommandPool(mDevice, mLoaderCommandPool, nullptr);
            mLoaderCommandPool = VK_NULL_HANDLE;
        }

        mVk.vkDestroyDevice(mDevice, nullptr);
        mDevice = VK_NULL_HANDLE;
    }

    if (mInstance) {
        mVk.vkDestroyInstance(mInstance, nullptr);
        mInstance = VK_NULL_HANDLE;
    }
}

uint32_t RendererVulkan::findMemoryType(uint32_t typeFilter,
                                        VkMemoryPropertyFlags properties) {
    for (uint32_t i = 0; i < mMemoryProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (mMemoryProperties.memoryTypes[i].propertyFlags & properties) ==
                    properties) {
            return i;
        }
    }
    return 0;
}

bool RendererVulkan::createVulkanBuffer(VkDeviceSize size,
                                        VkBufferUsageFlags usage,
                                        VkMemoryPropertyFlags properties,
                                        VkBuffer& buffer,
                                        VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (mVk.vkCreateBuffer(mDevice, &bufferInfo, nullptr, &buffer) !=
        VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    mVk.vkGetBufferMemoryRequirements(mDevice, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
            findMemoryType(memRequirements.memoryTypeBits, properties);

    if (mVk.vkAllocateMemory(mDevice, &allocInfo, nullptr, &bufferMemory) !=
        VK_SUCCESS) {
        mVk.vkDestroyBuffer(mDevice, buffer, nullptr);
        return false;
    }

    mVk.vkBindBufferMemory(mDevice, buffer, bufferMemory, 0);
    return true;
}

bool RendererVulkan::createVulkanImage(uint32_t width,
                                       uint32_t height,
                                       VkFormat format,
                                       VkImageUsageFlags usage,
                                       VkMemoryPropertyFlags properties,
                                       VkImage& image,
                                       VkDeviceMemory& imageMemory,
                                       VkImageView& imageView) {
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (mVk.vkCreateImage(mDevice, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    mVk.vkGetImageMemoryRequirements(mDevice, image, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex =
            findMemoryType(memRequirements.memoryTypeBits, properties);

    if (mVk.vkAllocateMemory(mDevice, &allocInfo, nullptr, &imageMemory) !=
        VK_SUCCESS) {
        mVk.vkDestroyImage(mDevice, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    mVk.vkBindImageMemory(mDevice, image, imageMemory, 0);

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask =
            (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM)
                    ? VK_IMAGE_ASPECT_DEPTH_BIT
                    : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (mVk.vkCreateImageView(mDevice, &viewInfo, nullptr, &imageView) !=
        VK_SUCCESS) {
        mVk.vkFreeMemory(mDevice, imageMemory, nullptr);
        imageMemory = VK_NULL_HANDLE;
        mVk.vkDestroyImage(mDevice, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

void RendererVulkan::submitCommandBuffer(VkCommandBuffer cmdBuffer,
                                         VkFence fence,
                                         bool waitIdle) {
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;

    AutoLock queueLock(mQueueLock);
    mVk.vkResetFences(mDevice, 1, &fence);
    mVk.vkQueueSubmit(mQueue, 1, &submitInfo, fence);
    mVk.vkWaitForFences(mDevice, 1, &fence, VK_TRUE, UINT64_MAX);
    if (waitIdle) {
        mVk.vkQueueWaitIdle(mQueue);
    }
}

void RendererVulkan::executeSingleTimeCommands(
        VkCommandBuffer cmdBuffer,
        VkFence fence,
        const std::function<void(VkCommandBuffer)>& recordFunc,
        bool waitIdle) {
    mVk.vkResetCommandBuffer(cmdBuffer, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    mVk.vkBeginCommandBuffer(cmdBuffer, &beginInfo);

    recordFunc(cmdBuffer);

    mVk.vkEndCommandBuffer(cmdBuffer);
    submitCommandBuffer(cmdBuffer, fence, waitIdle);
}

void RendererVulkan::transitionImageLayout(VkCommandBuffer cmdBuffer,
                                           VkImage image,
                                           VkImageLayout oldLayout,
                                           VkImageLayout newLayout,
                                           VkAccessFlags srcAccessMask,
                                           VkAccessFlags dstAccessMask,
                                           VkPipelineStageFlags srcStageMask,
                                           VkPipelineStageFlags dstStageMask,
                                           VkImageAspectFlags aspectMask) {
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccessMask;
    barrier.dstAccessMask = dstAccessMask;

    mVk.vkCmdPipelineBarrier(cmdBuffer, srcStageMask, dstStageMask, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
}

VkShaderModule RendererVulkan::createShaderModule(const uint32_t* spirvCode,
                                                  size_t codeSize) {
    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = codeSize;
    createInfo.pCode = spirvCode;

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (mVk.vkCreateShaderModule(mDevice, &createInfo, nullptr,
                                 &shaderModule) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return shaderModule;
}

bool RendererVulkan::createGraphicsPipeline(VkShaderModule vertShader,
                                            VkShaderModule fragShader,
                                            VkPipelineLayout pipelineLayout,
                                            VkFormat colorFormat,
                                            VkFormat depthFormat,
                                            VkPipeline& pipeline) {
    VkPipelineShaderStageCreateInfo vertStageInfo = {};
    vertStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStageInfo.module = vertShader;
    vertStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragStageInfo = {};
    fragStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStageInfo.module = fragShader;
    fragStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertStageInfo,
                                                      fragStageInfo};

    VkVertexInputBindingDescription bindingDescription = {};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(VertexPositionUV);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributeDescriptions[2] = {};
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(VertexPositionUV, pos);

    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(VertexPositionUV, uv);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
    vertexInputInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = 2;
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType =
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType =
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType =
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable =
            (depthFormat != VK_FORMAT_UNDEFINED) ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable =
            (depthFormat != VK_FORMAT_UNDEFINED) ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
    colorBlendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending = {};
    colorBlending.sType =
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                      VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo renderingCreateInfo = {};
    renderingCreateInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCreateInfo.colorAttachmentCount = 1;
    renderingCreateInfo.pColorAttachmentFormats = &colorFormat;
    renderingCreateInfo.depthAttachmentFormat = depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingCreateInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;

    return mVk.vkCreateGraphicsPipelines(mDevice, VK_NULL_HANDLE, 1,
                                         &pipelineInfo, nullptr,
                                         &pipeline) == VK_SUCCESS;
}

float RendererVulkan::getAspectRatio() {
    return static_cast<float>(mRenderWidth) / mRenderHeight;
}

bool RendererVulkan::isTextureLoaded(Texture texture) {
    AutoLock lock(mResourceLock);
    auto it = mTextures.find(texture.id);
    if (it == mTextures.end())
        return false;
    return it->second.mState == TextureState::Loaded;
}

void RendererVulkan::getTextureInfo(Texture texture,
                                    uint32_t* outWidth,
                                    uint32_t* outHeight) {
    AutoLock lock(mResourceLock);
    auto it = mTextures.find(texture.id);
    if (it != mTextures.end()) {
        *outWidth = it->second.mWidth;
        *outHeight = it->second.mHeight;
    } else {
        *outWidth = 0;
        *outHeight = 0;
    }
}

void RendererVulkan::releaseTexture(Texture texture) {
    if (!texture.isValid()) {
        return;
    }

    AutoLock lock(mResourceLock);
    if (texture.id == mDefaultTexture.id) {
        return;
    }

    auto it = mTextures.find(texture.id);
    if (it != mTextures.end()) {
        it->second.mRefCount--;
        if (it->second.mRefCount == 0) {
            if (!it->second.mFilename.empty()) {
                mTextureCache.erase(it->second.mFilename);
            }
            if (it->second.mDescriptorSet)
                mVk.vkFreeDescriptorSets(mDevice, mDescriptorPool, 1,
                                         &it->second.mDescriptorSet);
            if (it->second.mImageView)
                mVk.vkDestroyImageView(mDevice, it->second.mImageView, nullptr);
            if (it->second.mImage)
                mVk.vkDestroyImage(mDevice, it->second.mImage, nullptr);
            if (it->second.mImageMemory)
                mVk.vkFreeMemory(mDevice, it->second.mImageMemory, nullptr);
            mTextures.erase(it);
        }
    }
}

void RendererVulkan::releaseMaterial(Material material) {
    if (!material.isValid()) {
        return;
    }

    AutoLock lock(mResourceLock);
    if (!isStandardMaterial(material)) {
        releaseMaterialInternal(material);
    }
}

void RendererVulkan::releaseMaterialInternal(Material material) {
    auto it = mMaterials.find(material.id);
    if (it != mMaterials.end()) {
        if (it->second.pipeline)
            mVk.vkDestroyPipeline(mDevice, it->second.pipeline, nullptr);
        if (it->second.vertShader)
            mVk.vkDestroyShaderModule(mDevice, it->second.vertShader, nullptr);
        if (it->second.fragShader)
            mVk.vkDestroyShaderModule(mDevice, it->second.fragShader, nullptr);
        mMaterials.erase(it);
    }
}

void RendererVulkan::releaseMesh(Mesh mesh) {
    if (!mesh.isValid()) {
        return;
    }

    AutoLock lock(mResourceLock);
    auto it = mMeshes.find(mesh.id);
    if (it != mMeshes.end()) {
        if (it->second.mVertexBuffer)
            mVk.vkDestroyBuffer(mDevice, it->second.mVertexBuffer, nullptr);
        if (it->second.mVertexMemory)
            mVk.vkFreeMemory(mDevice, it->second.mVertexMemory, nullptr);
        if (it->second.mIndexBuffer)
            mVk.vkDestroyBuffer(mDevice, it->second.mIndexBuffer, nullptr);
        if (it->second.mIndexMemory)
            mVk.vkFreeMemory(mDevice, it->second.mIndexMemory, nullptr);
        mMeshes.erase(it);
    }
}

Material RendererVulkan::createMaterialTextured() {
    // Return cached instance if it exists.
    if (mMaterialTextured.isValid()) {
        return mMaterialTextured;
    }

    AutoLock lock(mResourceLock);
    int id = mNextResourceId++;

    MaterialData data;
    data.pipelineLayout = mMeshPipelineLayout;
    data.vertShader =
            createShaderModule(kTexturedVertSpirv, sizeof(kTexturedVertSpirv));
    data.fragShader =
            createShaderModule(kTexturedFragSpirv, sizeof(kTexturedFragSpirv));

    if (!createGraphicsPipeline(data.vertShader, data.fragShader,
                                data.pipelineLayout, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_FORMAT_D32_SFLOAT, data.pipeline)) {
        if (data.vertShader)
            mVk.vkDestroyShaderModule(mDevice, data.vertShader, nullptr);
        if (data.fragShader)
            mVk.vkDestroyShaderModule(mDevice, data.fragShader, nullptr);
        return Material{};
    }

    mMaterials[id] = data;
    return Material{id};
}

Material RendererVulkan::createMaterialCheckerboard() {
    AutoLock lock(mResourceLock);
    int id = mNextResourceId++;

    MaterialData data;
    data.pipelineLayout = mMeshPipelineLayout;
    data.vertShader =
            createShaderModule(kTexturedVertSpirv, sizeof(kTexturedVertSpirv));
    data.fragShader = createShaderModule(kCheckerboardFragSpirv,
                                         sizeof(kCheckerboardFragSpirv));

    if (!createGraphicsPipeline(data.vertShader, data.fragShader,
                                data.pipelineLayout, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_FORMAT_D32_SFLOAT, data.pipeline)) {
        if (data.vertShader)
            mVk.vkDestroyShaderModule(mDevice, data.vertShader, nullptr);
        if (data.fragShader)
            mVk.vkDestroyShaderModule(mDevice, data.fragShader, nullptr);
        return Material{};
    }

    mMaterials[id] = data;
    return Material{id};
}

Material RendererVulkan::createMaterialScreenSpace(const char* frag) {
    AutoLock lock(mResourceLock);
    int id = mNextResourceId++;

    MaterialData data;
    data.isScreenSpace = true;
    data.pipelineLayout = mScreenPipelineLayout;
    data.vertShader = createShaderModule(kScreenSpaceVertSpirv,
                                         sizeof(kScreenSpaceVertSpirv));

    if (frag && std::string(frag).find("blit") != std::string::npos) {
        data.fragShader =
                createShaderModule(kBlitFragSpirv, sizeof(kBlitFragSpirv));
    } else {
        data.fragShader =
                createShaderModule(kFxaaFragSpirv, sizeof(kFxaaFragSpirv));
    }

    if (!createGraphicsPipeline(data.vertShader, data.fragShader,
                                data.pipelineLayout, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_FORMAT_UNDEFINED, data.pipeline)) {
        if (data.vertShader)
            mVk.vkDestroyShaderModule(mDevice, data.vertShader, nullptr);
        if (data.fragShader)
            mVk.vkDestroyShaderModule(mDevice, data.fragShader, nullptr);
        return Material{};
    }

    mMaterials[id] = data;
    return Material{id};
}

Mesh RendererVulkan::createMesh(const VertexPositionUV* vertices,
                                size_t verticesSize,
                                const uint32_t* indices,
                                size_t indicesSize) {
    if (!vertices || verticesSize == 0 || !indices || indicesSize == 0) {
        return Mesh{};
    }

    AutoLock lock(mResourceLock);
    int id = mNextResourceId++;

    MeshData meshData;
    meshData.mIndexCount = indicesSize;

    VkDeviceSize vertBufferSize = sizeof(VertexPositionUV) * verticesSize;
    if (!createVulkanBuffer(vertBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            meshData.mVertexBuffer, meshData.mVertexMemory)) {
        return Mesh{};
    }

    void* mapped = nullptr;
    if (mVk.vkMapMemory(mDevice, meshData.mVertexMemory, 0, vertBufferSize, 0,
                        &mapped) != VK_SUCCESS || !mapped) {
        mVk.vkDestroyBuffer(mDevice, meshData.mVertexBuffer, nullptr);
        mVk.vkFreeMemory(mDevice, meshData.mVertexMemory, nullptr);
        return Mesh{};
    }
    memcpy(mapped, vertices, vertBufferSize);
    mVk.vkUnmapMemory(mDevice, meshData.mVertexMemory);

    VkDeviceSize indexBufferSize = sizeof(uint32_t) * indicesSize;
    if (!createVulkanBuffer(indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            meshData.mIndexBuffer, meshData.mIndexMemory)) {
        mVk.vkDestroyBuffer(mDevice, meshData.mVertexBuffer, nullptr);
        mVk.vkFreeMemory(mDevice, meshData.mVertexMemory, nullptr);
        return Mesh{};
    }

    mapped = nullptr;
    if (mVk.vkMapMemory(mDevice, meshData.mIndexMemory, 0, indexBufferSize, 0,
                        &mapped) != VK_SUCCESS || !mapped) {
        mVk.vkDestroyBuffer(mDevice, meshData.mVertexBuffer, nullptr);
        mVk.vkFreeMemory(mDevice, meshData.mVertexMemory, nullptr);
        mVk.vkDestroyBuffer(mDevice, meshData.mIndexBuffer, nullptr);
        mVk.vkFreeMemory(mDevice, meshData.mIndexMemory, nullptr);
        return Mesh{};
    }
    memcpy(mapped, indices, indexBufferSize);
    mVk.vkUnmapMemory(mDevice, meshData.mIndexMemory);

    mMeshes[id] = meshData;
    return Mesh{id};
}

Texture RendererVulkan::createEmptyTexture(uint32_t width, uint32_t height) {
    AutoLock lock(mResourceLock);
    int id = mNextResourceId++;

    TextureData data;
    data.mRefCount = 1;
    data.mState = TextureState::Loaded;
    data.mWidth = width;
    data.mHeight = height;

    if (!createVulkanImage(
                width, height, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, data.mImage, data.mImageMemory,
                data.mImageView)) {
        return Texture{-1};
    }

    executeSingleTimeCommands(
            mLoaderCommandBuffer, mLoaderFence,
            [this, &data](VkCommandBuffer cmd) {
                transitionImageLayout(cmd, data.mImage,
                                      VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      0, VK_ACCESS_SHADER_READ_BIT,
                                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            });

    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = mDescriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &mDescriptorSetLayout;

    if (mVk.vkAllocateDescriptorSets(mDevice, &allocInfo, &data.mDescriptorSet) !=
        VK_SUCCESS) {
        mVk.vkDestroyImageView(mDevice, data.mImageView, nullptr);
        mVk.vkDestroyImage(mDevice, data.mImage, nullptr);
        mVk.vkFreeMemory(mDevice, data.mImageMemory, nullptr);
        return Texture{-1};
    }

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = data.mImageView;
    imageInfo.sampler = mSampler;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = data.mDescriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;

    mVk.vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);

    mTextures[id] = data;
    return Texture{id};
}

bool RendererVulkan::isStandardMaterial(Material material) {
    if (material.id == mMaterialTextured.id) {
        return true;
    }

    return false;
}

Texture RendererVulkan::tryGetCachedTexture(const char* filename) {
    if (!filename)
        return Texture{-1};
    AutoLock lock(mResourceLock);
    auto it = mTextureCache.find(filename);
    if (it != mTextureCache.end()) {
        mTextures[it->second].mRefCount++;
        return Texture{it->second};
    }
    return Texture{-1};
}

void RendererVulkan::uploadTextureImage(TextureData& texData,
                                        const TextureUtils::Result& data) {
    if (texData.mImageView) {
        mVk.vkDestroyImageView(mDevice, texData.mImageView, nullptr);
        texData.mImageView = VK_NULL_HANDLE;
    }
    if (texData.mImage) {
        mVk.vkDestroyImage(mDevice, texData.mImage, nullptr);
        texData.mImage = VK_NULL_HANDLE;
    }
    if (texData.mImageMemory) {
        mVk.vkFreeMemory(mDevice, texData.mImageMemory, nullptr);
        texData.mImageMemory = VK_NULL_HANDLE;
    }

    texData.mWidth = data.mWidth;
    texData.mHeight = data.mHeight;

    if (!createVulkanImage(
                data.mWidth, data.mHeight, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texData.mImage,
                texData.mImageMemory, texData.mImageView)) {
        return;
    }

    const uint8_t* pixelData = nullptr;
    size_t dataSize = 0;
    std::vector<uint8_t> convertedBuffer;

    if (data.mFormat == TextureUtils::Format::RGB24) {
        size_t srcStride = (static_cast<size_t>(data.mWidth) * 3 + 3) & ~3;
        dataSize = static_cast<size_t>(data.mWidth) * data.mHeight * 4;
        convertedBuffer.resize(dataSize);
        for (size_t y = 0; y < data.mHeight; ++y) {
            const uint8_t* srcRow = data.mBuffer.data() + y * srcStride;
            uint8_t* dstRow = convertedBuffer.data() +
                              y * static_cast<size_t>(data.mWidth) * 4;
            for (size_t x = 0; x < data.mWidth; ++x) {
                dstRow[x * 4 + 0] = srcRow[x * 3 + 0];
                dstRow[x * 4 + 1] = srcRow[x * 3 + 1];
                dstRow[x * 4 + 2] = srcRow[x * 3 + 2];
                dstRow[x * 4 + 3] = 255;
            }
        }
        pixelData = convertedBuffer.data();
    } else {
        pixelData = data.mBuffer.data();
        dataSize = data.mBuffer.size();
    }

    if (pixelData && dataSize > 0) {
        VkBuffer stagingBuf = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        if (createVulkanBuffer(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               stagingBuf, stagingMem)) {
            void* mapped = nullptr;
            if (mVk.vkMapMemory(mDevice, stagingMem, 0, dataSize, 0, &mapped) ==
                VK_SUCCESS && mapped) {
                memcpy(mapped, pixelData, dataSize);
                mVk.vkUnmapMemory(mDevice, stagingMem);

                executeSingleTimeCommands(
                        mLoaderCommandBuffer, mLoaderFence,
                        [this, &texData, &data, stagingBuf](VkCommandBuffer cmd) {
                            transitionImageLayout(
                                    cmd, texData.mImage,
                                    VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT);

                            VkBufferImageCopy region = {};
                            region.imageSubresource.aspectMask =
                                    VK_IMAGE_ASPECT_COLOR_BIT;
                            region.imageSubresource.layerCount = 1;
                            region.imageExtent = {data.mWidth, data.mHeight, 1};

                            mVk.vkCmdCopyBufferToImage(
                                    cmd, stagingBuf, texData.mImage,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                    &region);

                            transitionImageLayout(
                                    cmd, texData.mImage,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_ACCESS_SHADER_READ_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                        });
            }
            mVk.vkDestroyBuffer(mDevice, stagingBuf, nullptr);
            mVk.vkFreeMemory(mDevice, stagingMem, nullptr);
        }
    } else {
        executeSingleTimeCommands(
                mLoaderCommandBuffer, mLoaderFence,
                [this, &texData](VkCommandBuffer cmd) {
                    transitionImageLayout(
                            cmd, texData.mImage, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                            VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                });
    }

    if (!texData.mDescriptorSet) {
        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = mDescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &mDescriptorSetLayout;

        if (mVk.vkAllocateDescriptorSets(mDevice, &allocInfo,
                                         &texData.mDescriptorSet) !=
            VK_SUCCESS) {
            return;
        }
    }

    VkDescriptorImageInfo imageInfo = {};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = texData.mImageView;
    imageInfo.sampler = mSampler;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = texData.mDescriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;

    mVk.vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);
}

Texture RendererVulkan::createTextureInternal(
        TextureState state,
        const char* filename,
        const TextureUtils::Result& data) {
    AutoLock lock(mResourceLock);
    int id = mNextResourceId++;

    TextureData texData;
    texData.mRefCount = 1;
    texData.mState = state;
    if (filename)
        texData.mFilename = filename;

    uploadTextureImage(texData, data);

    mTextures[id] = texData;
    if (filename)
        mTextureCache[filename] = id;
    return Texture{id};
}

bool RendererVulkan::updateMesh(Mesh meshHandle,
                                const VertexPositionUV* vertices,
                                size_t verticesSize) {
    if (!meshHandle.isValid() || !vertices || verticesSize == 0) {
        return false;
    }
    AutoLock lock(mResourceLock);
    auto it = mMeshes.find(meshHandle.id);
    if (it == mMeshes.end() || !it->second.mVertexMemory) {
        return false;
    }

    VkDeviceSize vertBufferSize = sizeof(VertexPositionUV) * verticesSize;
    void* mapped = nullptr;
    if (mVk.vkMapMemory(mDevice, it->second.mVertexMemory, 0, vertBufferSize, 0,
                        &mapped) != VK_SUCCESS || !mapped) {
        E("%s: Vulkan map memory failed", __FUNCTION__);
        return false;
    }

    memcpy(mapped, vertices, vertBufferSize);
    mVk.vkUnmapMemory(mDevice, it->second.mVertexMemory);
    return true;
}

Texture RendererVulkan::loadTexture(const char* filename) {
    if (!filename) {
        derror("Invalid texture filename");
        return Texture{-1};
    }
    const uint64_t loadStartUs = System::get()->getHighResTimeUs();

    std::string path;
    if (!PathUtils::isAbsolute(filename)) {
        path = PathUtils::join(System::get()->getLauncherDirectory(),
                               "resources", filename);
    } else {
        path = filename;
    }

    Texture cached = tryGetCachedTexture(path.c_str());
    if (cached.isValid())
        return cached;

    auto resOpt = TextureUtils::load(path.c_str());
    if (!resOpt.has_value())
        return Texture{-1};

    const uint64_t loadEndUs = System::get()->getHighResTimeUs();

    Texture texture = createTextureInternal(TextureState::Loaded, path.c_str(),
                                            resOpt.value());
    // The texture may be invalid here, but the error is already logged so pass
    // it through as-is.

    const uint64_t importEndUs = System::get()->getHighResTimeUs();
    D("%s: Sync load texture %d in %" PRIu64 "ms, [read: %" PRIu64
      "ms, import: %" PRIu64 "ms]",
      __FUNCTION__, texture.id, durationUsToMs(loadStartUs, importEndUs),
      durationUsToMs(loadStartUs, loadEndUs),
      durationUsToMs(loadEndUs, importEndUs));

    return texture;
}

Texture RendererVulkan::loadTextureAsync(const char* filename) {
    if (!filename)
        return Texture{-1};

    std::string path;
    if (!PathUtils::isAbsolute(filename)) {
        path = PathUtils::join(System::get()->getLauncherDirectory(),
                               "resources", filename);
    } else {
        path = filename;
    }

    Texture cached = tryGetCachedTexture(path.c_str());
    if (cached.isValid())
        return cached;

    TextureUtils::Result placeholder = TextureUtils::createPlaceholder();
    Texture texture = createTextureInternal(TextureState::Placeholder,
                                            path.c_str(), placeholder);

    mLoaderThread.enqueue(
            LoaderCommand(LoaderCommandType::LoadTexture, texture.id));
    return texture;
}

WorkerProcessingResult RendererVulkan::onLoaderCommand(
        LoaderCommand&& command) {
    if (command.mType == LoaderCommandType::Shutdown) {
        return WorkerProcessingResult::Stop;
    }
    if (command.mType == LoaderCommandType::LoadTexture) {
        onLoaderLoadTexture(Texture{command.mHandle});
    }
    return WorkerProcessingResult::Continue;
}

void RendererVulkan::onLoaderLoadTexture(Texture texture) {
    std::string filename;
    {
        AutoLock lock(mResourceLock);
        auto it = mTextures.find(texture.id);
        if (it == mTextures.end())
            return;
        filename = it->second.mFilename;
    }

    if (!filename.empty()) {
        auto resOpt = TextureUtils::load(filename.c_str());
        if (resOpt.has_value()) {
            replaceTextureInternal(texture, resOpt.value());
        }
    }
}

bool RendererVulkan::replaceTextureInternal(Texture texture,
                                            const TextureUtils::Result& data) {
    AutoLock lock(mResourceLock);
    auto it = mTextures.find(texture.id);
    if (it == mTextures.end())
        return false;

    uploadTextureImage(it->second, data);
    it->second.mState = TextureState::Loaded;
    return true;
}

Texture RendererVulkan::createTextureRGBA(const uint8_t* rgba,
                                          uint32_t width,
                                          uint32_t height) {
    if (!rgba || width == 0 || height == 0) {
        return Texture();
    }
    TextureUtils::Result result;
    result.mWidth = width;
    result.mHeight = height;
    result.mFormat = TextureUtils::Format::RGBA32;
    result.mBuffer.assign(rgba,
                          rgba + (static_cast<size_t>(width) * height * 4));

    return createTextureInternal(TextureState::Loaded, nullptr, result);
}

Texture RendererVulkan::duplicateTexture(Texture texture) {
    AutoLock lock(mResourceLock);
    auto it = mTextures.find(texture.id);
    if (it != mTextures.end()) {
        it->second.mRefCount++;
        return texture;
    }
    return Texture{-1};
}

void RendererVulkan::drawRenderables(
        VkCommandBuffer cmd,
        const std::vector<RenderableObject>& renderables,
        float time) {
    struct PushConstants {
        glm::mat4 mvp;
        float time;
    };

    for (const auto& obj : renderables) {
        auto matIt = mMaterials.find(obj.renderable.material.id);
        auto meshIt = mMeshes.find(obj.renderable.mesh.id);

        if (matIt == mMaterials.end() || meshIt == mMeshes.end())
            continue;

        const MaterialData& mat = matIt->second;
        const MeshData& mesh = meshIt->second;

        mVk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              mat.pipeline);

        auto texIt = mTextures.find(obj.renderable.texture.id);
        if (texIt != mTextures.end() && texIt->second.mDescriptorSet) {
            mVk.vkCmdBindDescriptorSets(
                    cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mat.pipelineLayout, 0,
                    1, &texIt->second.mDescriptorSet, 0, nullptr);
        } else {
            auto defaultTexIt = mTextures.find(mDefaultTexture.id);
            if (defaultTexIt != mTextures.end() &&
                defaultTexIt->second.mDescriptorSet) {
                mVk.vkCmdBindDescriptorSets(
                        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        mat.pipelineLayout, 0, 1,
                        &defaultTexIt->second.mDescriptorSet, 0, nullptr);
            }
        }

        PushConstants pushData;
        pushData.mvp = obj.modelViewProj;
        pushData.time = time;

        mVk.vkCmdPushConstants(
                cmd, mat.pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                sizeof(pushData), &pushData);

        VkDeviceSize offsets[] = {0};
        mVk.vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.mVertexBuffer, offsets);
        mVk.vkCmdBindIndexBuffer(cmd, mesh.mIndexBuffer, 0,
                                 VK_INDEX_TYPE_UINT32);

        mVk.vkCmdDrawIndexed(cmd, mesh.mIndexCount, 1, 0, 0, 0);
    }
}

bool RendererVulkan::render(RendererView* view,
                            const std::vector<RenderableObject>& renderables,
                            float time) {
    if (!view)
        return false;

    const int width = view->getWidthLocked();
    const int height = view->getHeightLocked();

    if (width <= 0 || height <= 0)
        return false;

    {
        AutoLock lock(mResourceLock);
        executeSingleTimeCommands(
                mCommandBuffer, mFence,
                [this, &renderables, time](VkCommandBuffer cmd) {
                    transitionImageLayout(
                            cmd, mOffscreenColorImage,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

                    transitionImageLayout(
                            cmd, mOffscreenDepthImage,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, 0,
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT);

                    // Dynamic Rendering
                    VkRenderingAttachmentInfo colorAtt = {};
                    colorAtt.sType =
                            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    colorAtt.imageView = mOffscreenColorView;
                    colorAtt.imageLayout =
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    colorAtt.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

                    VkRenderingAttachmentInfo depthAtt = {};
                    depthAtt.sType =
                            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    depthAtt.imageView = mOffscreenDepthView;
                    depthAtt.imageLayout =
                            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    depthAtt.clearValue.depthStencil = {1.0f, 0};

                    VkRenderingInfo renderInfo = {};
                    renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                    renderInfo.renderArea = {
                            {0, 0},
                            {static_cast<uint32_t>(mRenderWidth),
                             static_cast<uint32_t>(mRenderHeight)}};
                    renderInfo.layerCount = 1;
                    renderInfo.colorAttachmentCount = 1;
                    renderInfo.pColorAttachments = &colorAtt;
                    renderInfo.pDepthAttachment = &depthAtt;

                    mVk.vkCmdBeginRendering(cmd, &renderInfo);

                    VkViewport viewport = {0.0f,
                                           0.0f,
                                           static_cast<float>(mRenderWidth),
                                           static_cast<float>(mRenderHeight),
                                           0.0f,
                                           1.0f};
                    VkRect2D scissor = {{0, 0},
                                        {static_cast<uint32_t>(mRenderWidth),
                                         static_cast<uint32_t>(mRenderHeight)}};

                    mVk.vkCmdSetViewport(cmd, 0, 1, &viewport);
                    mVk.vkCmdSetScissor(cmd, 0, 1, &scissor);

                    drawRenderables(cmd, renderables, time);

                    mVk.vkCmdEndRendering(cmd);

                    transitionImageLayout(
                            cmd, mOffscreenColorImage,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);

                    VkBufferImageCopy copyRegion = {};
                    copyRegion.imageSubresource.aspectMask =
                            VK_IMAGE_ASPECT_COLOR_BIT;
                    copyRegion.imageSubresource.layerCount = 1;
                    copyRegion.imageExtent = {
                            static_cast<uint32_t>(mRenderWidth),
                            static_cast<uint32_t>(mRenderHeight), 1};

                    mVk.vkCmdCopyImageToBuffer(
                            cmd, mOffscreenColorImage,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            mStagingBuffer, 1, &copyRegion);
                },
                /*waitIdle=*/true);
    }

    // Read back RGBA pixels to view framebuffer
    void* mapped = nullptr;
    if (mVk.vkMapMemory(mDevice, mStagingMemory, 0,
                        mRenderWidth * mRenderHeight * 4, 0, &mapped) !=
                VK_SUCCESS ||
        !mapped) {
        derror("%s: Vulkan map memory failed for staging memory", __FUNCTION__);
        return false;
    }

    ImageScaler scaler(width, height, view->getFramebufferLocked().data());
    scaler.updateImage(mRenderWidth, mRenderHeight,
                       static_cast<const uint8_t*>(mapped),
                       ImageScaler::ScaleMode::ScaleToFill);

    mVk.vkUnmapMemory(mDevice, mStagingMemory);

    return true;
}

std::unique_ptr<RendererContext> RendererVulkan::makeCurrent() {
    return std::unique_ptr<RendererContext>(
            new VulkanRendererContext(mInitialized));
}

}  // namespace ver
}  // namespace android
