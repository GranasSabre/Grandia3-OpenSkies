// Vulkan context of the Android version: device, swapchain on the activity's window, presentation of
// the game picture and the touch overlay. The GS renderer (gs_vk.cpp) records into the same device.
#pragma once
#include "common.h"
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#include <mutex>

namespace vkc {
bool init();                         // instance and device (once; false: no usable Vulkan)
bool set_window(ANativeWindow* w);   // swapchain on the window (nullptr: release it)
bool ready();                        // a swapchain exists
// Present a picture: RGBA8 pixels (R in the low byte) w x h with row stride, its rows
// [src_y0, src_y0 + src_h) scaled into the destination rectangle (window pixels); the overlay
// (premultiplied RGBA, ow x oh, covering the whole window) is uploaded when overlay_new.
bool present(const u32* px, int w, int h, int stride, int src_y0, int src_h, int dx0, int dy0, int dx1, int dy1,
             const u32* overlay, int ow, int oh, bool overlay_new);
// The same with a picture the GS renderer composed on the GPU (RGBA8 image view in
// GENERAL layout, written by earlier submissions on the same queue).
bool present_image(VkImageView view, int w, int h, int src_y0, int src_h, int dx0, int dy0, int dx1, int dy1,
                   const u32* overlay, int ow, int oh, bool overlay_new);

// for the GS renderer
VkDevice device();
VkPhysicalDevice physical();
VkQueue queue();
uint32_t queue_family();
uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want);  // ~0u: none
std::mutex& queue_mutex();          // held around every vkQueueSubmit / present / wait idle
bool raster_order();                 // VK_EXT_rasterization_order_attachment_access enabled
bool push_descriptors();             // VK_KHR_push_descriptor enabled
}  // namespace vkc
