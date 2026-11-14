// Vulkan capability probe (Android test version): logs the device and the features the GS renderer
// design depends on (ordered read-modify-write of the target pixels like Direct3D's rasterizer
// ordered views: fragment shader interlock or rasterization-order attachment access).
#include "common.h"
#include <vulkan/vulkan.h>
#include <string>
#include <vector>

void vk_probe() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Grandia3";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        log_write(LOG_MAIN, "vulkan: no instance");
        return;
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    for (VkPhysicalDevice pd : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        log_write(LOG_MAIN, "vulkan: %s, API %u.%u.%u, driver %08x", p.deviceName, VK_VERSION_MAJOR(p.apiVersion),
                  VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion), p.driverVersion);
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ex(ne);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, ex.data());
        std::string line;
        for (auto& e : ex) {
            line += e.extensionName;
            line += ' ';
            if (line.size() > 900) { log_write(LOG_MAIN, "vulkan ext: %s", line.c_str()); line.clear(); }
        }
        if (!line.empty()) log_write(LOG_MAIN, "vulkan ext: %s", line.c_str());
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT il{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
        VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT ro{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
        il.pNext = &ro;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &il;
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        log_write(LOG_MAIN, "vulkan features: interlock pixel %d sample %d shading-rate %d | raster order color %d depth %d stencil %d | "
                  "fragmentStoresAndAtomics %d, shaderStorageImageExtendedFormats %d, independentBlend %d, maxImageDim2D %u, maxFramebuffer %ux%u",
                  il.fragmentShaderPixelInterlock, il.fragmentShaderSampleInterlock, il.fragmentShaderShadingRateInterlock,
                  ro.rasterizationOrderColorAttachmentAccess, ro.rasterizationOrderDepthAttachmentAccess,
                  ro.rasterizationOrderStencilAttachmentAccess, f2.features.fragmentStoresAndAtomics,
                  f2.features.shaderStorageImageExtendedFormats, f2.features.independentBlend, p.limits.maxImageDimension2D,
                  p.limits.maxFramebufferWidth, p.limits.maxFramebufferHeight);
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R32_UINT, &fp);
        log_write(LOG_MAIN, "vulkan R32_UINT optimal: %08x (storage %d, atomic %d, color attachment %d)", fp.optimalTilingFeatures,
                  !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT),
                  !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT),
                  !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT));
    }
    vkDestroyInstance(inst, nullptr);
}
