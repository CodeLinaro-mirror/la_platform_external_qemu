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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
inline void* ver_dlopen(const char* filename, int flags) {
    return (void*)LoadLibraryA(filename);
}
inline void* ver_dlsym(void* handle, const char* symbol) {
    return (void*)GetProcAddress((HMODULE)handle, symbol);
}
inline int ver_dlclose(void* handle) {
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}
#else
#include <dlfcn.h>
inline void* ver_dlopen(const char* filename, int flags) {
    return dlopen(filename, flags);
}
inline void* ver_dlsym(void* handle, const char* symbol) {
    return dlsym(handle, symbol);
}
inline int ver_dlclose(void* handle) {
    return dlclose(handle);
}
#endif

#ifndef RTLD_NOW
#define RTLD_NOW 0
#endif
#ifndef RTLD_LOCAL
#define RTLD_LOCAL 0
#endif
#ifndef RTLD_NODELETE
#define RTLD_NODELETE 0
#endif

#define VK_NO_PROTOTYPES
#include "third_party/vulkan/vulkan.h"

namespace android {
namespace ver {

struct VulkanDispatchTable {
    void* libHandle = nullptr;

    // Global
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;

    // Instance
    PFN_vkDestroyInstance vkDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateDevice vkCreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;

    // Device
    PFN_vkDestroyDevice vkDestroyDevice = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
    PFN_vkCreateFence vkCreateFence = nullptr;
    PFN_vkDestroyFence vkDestroyFence = nullptr;
    PFN_vkWaitForFences vkWaitForFences = nullptr;
    PFN_vkResetFences vkResetFences = nullptr;
    PFN_vkCreateImage vkCreateImage = nullptr;
    PFN_vkDestroyImage vkDestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
    PFN_vkCreateBuffer vkCreateBuffer = nullptr;
    PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
    PFN_vkBindImageMemory vkBindImageMemory = nullptr;
    PFN_vkMapMemory vkMapMemory = nullptr;
    PFN_vkUnmapMemory vkUnmapMemory = nullptr;
    PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges = nullptr;
    PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges = nullptr;
    PFN_vkCreateImageView vkCreateImageView = nullptr;
    PFN_vkDestroyImageView vkDestroyImageView = nullptr;
    PFN_vkCreateSampler vkCreateSampler = nullptr;
    PFN_vkDestroySampler vkDestroySampler = nullptr;
    PFN_vkCreateShaderModule vkCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule vkDestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout = nullptr;
    PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets = nullptr;
    PFN_vkFreeDescriptorSets vkFreeDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets = nullptr;
    PFN_vkCreatePipelineLayout vkCreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline vkDestroyPipeline = nullptr;

    // Dynamic Rendering
    PFN_vkCmdBeginRendering vkCmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering vkCmdEndRendering = nullptr;

    PFN_vkCmdBindPipeline vkCmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets = nullptr;
    PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers = nullptr;
    PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer = nullptr;
    PFN_vkCmdSetViewport vkCmdSetViewport = nullptr;
    PFN_vkCmdSetScissor vkCmdSetScissor = nullptr;
    PFN_vkCmdDraw vkCmdDraw = nullptr;
    PFN_vkCmdDrawIndexed vkCmdDrawIndexed = nullptr;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = nullptr;
    PFN_vkCmdCopyBuffer vkCmdCopyBuffer = nullptr;
    PFN_vkCmdPushConstants vkCmdPushConstants = nullptr;

    VulkanDispatchTable() = default;
    ~VulkanDispatchTable() {
        if (libHandle) {
            ver_dlclose(libHandle);
            libHandle = nullptr;
        }
    }
    VulkanDispatchTable(const VulkanDispatchTable&) = delete;
    VulkanDispatchTable& operator=(const VulkanDispatchTable&) = delete;

    bool initDriver(const std::filesystem::path& driverFolder = {}) {
        if(libHandle) {
            fprintf(stderr, "VulkanDispatch: Init called twice\n");
            return true;
        }

        const std::vector<std::string> libNames = {
#ifdef _WIN32
            "libvulkan_lvp.dll",
            "vulkan-1.dll",
#elif defined(__APPLE__)
            "libvulkan_lvp.dylib",
            "libvulkan.dylib",
#else
            "libvulkan_lvp.so",
            "libvulkan.so",
            "libvulkan.so.1",
#endif
        };

        auto tryLoad = [this](const std::string& pathStr) -> bool {
            libHandle = ver_dlopen(pathStr.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
            if (!libHandle) {
                return false;
            }
            vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)ver_dlsym(libHandle, "vkGetInstanceProcAddr");
            if (!vkGetInstanceProcAddr) {
                vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)ver_dlsym(libHandle, "vk_icdGetInstanceProcAddr");
            }
            if (!vkGetInstanceProcAddr) {
                ver_dlclose(libHandle);
                libHandle = nullptr;
                return false;
            }
            return true;
        };

        if (!driverFolder.empty()) {
            for (const auto& name : libNames) {
                std::filesystem::path fullPath = driverFolder / name;
                if (tryLoad(fullPath.string())) {
                    break;
                }
            }
        }

        if (!libHandle) {
            for (const auto& name : libNames) {
                if (tryLoad(name)) {
                    break;
                }
            }
        }

        if (!libHandle) {
            fprintf(stderr, "VulkanDispatch: Cannot find Vulkan driver\n");
            return false;
        }

        vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
        if (!vkCreateInstance) {
            fprintf(stderr, "VulkanDispatch: Cannot find vkCreateInstance\n");
            return false;
        }
        vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties");
        if (!vkEnumerateInstanceExtensionProperties) {
            fprintf(stderr, "VulkanDispatch: Cannot find vkEnumerateInstanceExtensionProperties\n");
            return false;
        }
        return true;
    }

    bool initInstance(VkInstance instance) {
        if (!instance || !vkGetInstanceProcAddr) return false;
#define LOAD_INS_PROC(name) name = (PFN_##name)vkGetInstanceProcAddr(instance, #name)
        LOAD_INS_PROC(vkDestroyInstance);
        LOAD_INS_PROC(vkEnumeratePhysicalDevices);
        LOAD_INS_PROC(vkGetPhysicalDeviceProperties);
        LOAD_INS_PROC(vkGetPhysicalDeviceProperties2);
        if (!vkGetPhysicalDeviceProperties2) {
            vkGetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR");
        }
        LOAD_INS_PROC(vkGetPhysicalDeviceFeatures);
        LOAD_INS_PROC(vkGetPhysicalDeviceFeatures2);
        LOAD_INS_PROC(vkGetPhysicalDeviceQueueFamilyProperties);
        LOAD_INS_PROC(vkGetPhysicalDeviceMemoryProperties);
        LOAD_INS_PROC(vkCreateDevice);
        LOAD_INS_PROC(vkGetDeviceProcAddr);
#undef LOAD_INS_PROC
        return vkCreateDevice != nullptr;
    }

    bool initDevice(VkDevice device) {
        if (!device || !vkGetDeviceProcAddr) return false;
#define LOAD_DEV_PROC(name) name = (PFN_##name)vkGetDeviceProcAddr(device, #name)
        LOAD_DEV_PROC(vkDestroyDevice);
        LOAD_DEV_PROC(vkGetDeviceQueue);
        LOAD_DEV_PROC(vkCreateCommandPool);
        LOAD_DEV_PROC(vkDestroyCommandPool);
        LOAD_DEV_PROC(vkAllocateCommandBuffers);
        LOAD_DEV_PROC(vkFreeCommandBuffers);
        LOAD_DEV_PROC(vkBeginCommandBuffer);
        LOAD_DEV_PROC(vkEndCommandBuffer);
        LOAD_DEV_PROC(vkResetCommandBuffer);
        LOAD_DEV_PROC(vkQueueSubmit);
        LOAD_DEV_PROC(vkQueueWaitIdle);
        LOAD_DEV_PROC(vkDeviceWaitIdle);
        LOAD_DEV_PROC(vkCreateFence);
        LOAD_DEV_PROC(vkDestroyFence);
        LOAD_DEV_PROC(vkWaitForFences);
        LOAD_DEV_PROC(vkResetFences);
        LOAD_DEV_PROC(vkCreateImage);
        LOAD_DEV_PROC(vkDestroyImage);
        LOAD_DEV_PROC(vkGetImageMemoryRequirements);
        LOAD_DEV_PROC(vkCreateBuffer);
        LOAD_DEV_PROC(vkDestroyBuffer);
        LOAD_DEV_PROC(vkGetBufferMemoryRequirements);
        LOAD_DEV_PROC(vkAllocateMemory);
        LOAD_DEV_PROC(vkFreeMemory);
        LOAD_DEV_PROC(vkBindBufferMemory);
        LOAD_DEV_PROC(vkBindImageMemory);
        LOAD_DEV_PROC(vkMapMemory);
        LOAD_DEV_PROC(vkUnmapMemory);
        LOAD_DEV_PROC(vkFlushMappedMemoryRanges);
        LOAD_DEV_PROC(vkInvalidateMappedMemoryRanges);
        LOAD_DEV_PROC(vkCreateImageView);
        LOAD_DEV_PROC(vkDestroyImageView);
        LOAD_DEV_PROC(vkCreateSampler);
        LOAD_DEV_PROC(vkDestroySampler);
        LOAD_DEV_PROC(vkCreateShaderModule);
        LOAD_DEV_PROC(vkDestroyShaderModule);
        LOAD_DEV_PROC(vkCreateDescriptorSetLayout);
        LOAD_DEV_PROC(vkDestroyDescriptorSetLayout);
        LOAD_DEV_PROC(vkCreateDescriptorPool);
        LOAD_DEV_PROC(vkDestroyDescriptorPool);
        LOAD_DEV_PROC(vkAllocateDescriptorSets);
        LOAD_DEV_PROC(vkFreeDescriptorSets);
        LOAD_DEV_PROC(vkUpdateDescriptorSets);
        LOAD_DEV_PROC(vkCreatePipelineLayout);
        LOAD_DEV_PROC(vkDestroyPipelineLayout);
        LOAD_DEV_PROC(vkCreateGraphicsPipelines);
        LOAD_DEV_PROC(vkDestroyPipeline);

        LOAD_DEV_PROC(vkCmdBeginRendering);
        if (!vkCmdBeginRendering) {
            vkCmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR");
        }
        LOAD_DEV_PROC(vkCmdEndRendering);
        if (!vkCmdEndRendering) {
            vkCmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR");
        }

        LOAD_DEV_PROC(vkCmdBindPipeline);
        LOAD_DEV_PROC(vkCmdBindDescriptorSets);
        LOAD_DEV_PROC(vkCmdBindVertexBuffers);
        LOAD_DEV_PROC(vkCmdBindIndexBuffer);
        LOAD_DEV_PROC(vkCmdSetViewport);
        LOAD_DEV_PROC(vkCmdSetScissor);
        LOAD_DEV_PROC(vkCmdDraw);
        LOAD_DEV_PROC(vkCmdDrawIndexed);
        LOAD_DEV_PROC(vkCmdPipelineBarrier);
        LOAD_DEV_PROC(vkCmdCopyImageToBuffer);
        LOAD_DEV_PROC(vkCmdCopyBufferToImage);
        LOAD_DEV_PROC(vkCmdCopyBuffer);
        LOAD_DEV_PROC(vkCmdPushConstants);
#undef LOAD_DEV_PROC
        return vkDestroyDevice != nullptr;
    }
};

}  // namespace ver
}  // namespace android
