// Vulkan loaded at run time (vk_dyn.cpp): the system driver (libvulkan.so) or a custom driver such as
// Turnip through libadrenotools (third_party/libadrenotools). Every Vulkan function the app uses is a
// function pointer with the usual name, so the renderer code stays unchanged. Include this instead of
// <vulkan/vulkan.h>.
#pragma once
#define VK_NO_PROTOTYPES
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>
#include <string>

#define G3_VK_GLOBAL_FUNCS(X) \
    X(vkCreateInstance)       \
    X(vkEnumerateInstanceExtensionProperties)

#define G3_VK_INSTANCE_FUNCS(X)                                                                                      \
    X(vkAcquireNextImageKHR) X(vkAllocateCommandBuffers) X(vkAllocateDescriptorSets) X(vkAllocateMemory)            \
    X(vkBeginCommandBuffer) X(vkBindBufferMemory) X(vkBindImageMemory) X(vkCmdBeginRenderPass)                      \
    X(vkCmdBindDescriptorSets) X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdClearColorImage)               \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) X(vkCmdDispatch) X(vkCmdDraw)            \
    X(vkCmdEndRenderPass) X(vkCmdPipelineBarrier) X(vkCmdPushConstants) X(vkCmdResetQueryPool)                      \
    X(vkCmdSetBlendConstants) X(vkCmdSetScissor) X(vkCmdSetViewport) X(vkCmdWriteTimestamp)                         \
    X(vkCreateAndroidSurfaceKHR) X(vkCreateBuffer) X(vkCreateCommandPool) X(vkCreateComputePipelines)               \
    X(vkCreateDescriptorPool) X(vkCreateDescriptorSetLayout) X(vkCreateDevice) X(vkCreateFence)                     \
    X(vkCreateFramebuffer) X(vkCreateGraphicsPipelines) X(vkCreateImage) X(vkCreateImageView)                       \
    X(vkCreatePipelineCache) X(vkCreatePipelineLayout) X(vkCreateQueryPool) X(vkCreateRenderPass)                   \
    X(vkCreateSampler) X(vkCreateSemaphore) X(vkCreateShaderModule) X(vkCreateSwapchainKHR) X(vkDestroyBuffer)      \
    X(vkDestroyFramebuffer) X(vkDestroyImage) X(vkDestroyImageView) X(vkDestroyInstance) X(vkDestroySemaphore)      \
    X(vkDestroyShaderModule) X(vkDestroySurfaceKHR) X(vkDestroySwapchainKHR) X(vkDeviceWaitIdle)                    \
    X(vkEndCommandBuffer) X(vkEnumerateDeviceExtensionProperties) X(vkEnumeratePhysicalDevices) X(vkFreeMemory)    \
    X(vkGetBufferMemoryRequirements) X(vkGetDeviceProcAddr) X(vkGetDeviceQueue) X(vkGetImageMemoryRequirements)     \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceMemoryProperties)   \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties)                                    \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)                            \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPipelineCacheData) X(vkGetQueryPoolResults)                      \
    X(vkGetSwapchainImagesKHR) X(vkInvalidateMappedMemoryRanges) X(vkMapMemory) X(vkQueuePresentKHR)               \
    X(vkQueueSubmit) X(vkResetCommandBuffer) X(vkResetCommandPool) X(vkResetFences) X(vkUpdateDescriptorSets)      \
    X(vkWaitForFences)

#define G3_VK_DECL(n) extern PFN_##n n;
G3_VK_GLOBAL_FUNCS(G3_VK_DECL)
G3_VK_INSTANCE_FUNCS(G3_VK_DECL)
#undef G3_VK_DECL

std::string g3_gpu_driver_setting();             // platform_android.cpp: chosen custom driver ("" = system)
void g3_set_gpu_driver_setting(const std::string& name);  // (saved in grandia3.ini, used from the next start)

namespace vkdyn {
// Opens the Vulkan driver: custom (a folder under files/gpu_drivers with the driver's .so, see
// vk_dyn.cpp) or, with an empty name or on failure, the system driver. False: no Vulkan at all.
bool load(const std::string& custom_driver);
void load_instance(VkInstance inst);  // the remaining functions, after vkCreateInstance
void unload();                        // back to nothing loaded (then load() again, e.g. the system driver)
bool custom_active();                 // the loaded driver is a custom one
std::string driver_label();           // "System" or the custom driver's name (for the log and the menu)
}  // namespace vkdyn
