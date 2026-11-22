// Vulkan context and presentation (see vk_ctx.h).
#define VK_USE_PLATFORM_ANDROID_KHR
#include "vk_ctx.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>
#include "shaders_spv.h"  // build/android/gen: SPIR-V of runtime/android/shaders

namespace vkc {

namespace {
constexpr int FRAMES = 2;

VkInstance g_inst = VK_NULL_HANDLE;
VkPhysicalDevice g_pd = VK_NULL_HANDLE;
VkDevice g_dev = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;
uint32_t g_qfam = 0;
VkPhysicalDeviceMemoryProperties g_mem;

VkSurfaceKHR g_surface = VK_NULL_HANDLE;
VkSwapchainKHR g_swap = VK_NULL_HANDLE;
VkFormat g_swap_format = VK_FORMAT_R8G8B8A8_UNORM;
VkExtent2D g_extent{0, 0};
std::vector<VkImage> g_images;
std::vector<VkImageView> g_views;
std::vector<VkFramebuffer> g_fbs;
std::vector<VkSemaphore> g_render_done;  // per swapchain image
ANativeWindow* g_window = nullptr;
std::mutex g_m;

VkRenderPass g_rp = VK_NULL_HANDLE;
VkPipelineLayout g_layout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_dsl = VK_NULL_HANDLE;
VkPipeline g_pipe_opaque = VK_NULL_HANDLE, g_pipe_blend = VK_NULL_HANDLE;
VkSampler g_sampler = VK_NULL_HANDLE;
VkDescriptorPool g_dpool = VK_NULL_HANDLE;

struct Tex {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    int w = 0, h = 0;
    bool valid = false;  // holds data (layout SHADER_READ_ONLY)
};
struct Buf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    size_t size = 0;
};
struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    Buf staging, staging_ovl;
    Tex game;  // each frame its own picture texture (no wait for the previous frame's reads)
    VkDescriptorSet ds_game = VK_NULL_HANDLE;
    VkDescriptorSet ds_ext = VK_NULL_HANDLE;  // picture of the GS renderer
};
Frame g_frames[FRAMES];
int g_fi = 0;
Tex g_ovl;  // shared overlay texture (updated rarely; the update waits for both frames)
VkDescriptorSet g_ds_ovl = VK_NULL_HANDLE;

uint32_t mem_any(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}
std::mutex g_qm;  // queue access (presentation thread / GS renderer thread)
bool g_raster_order = false, g_push_desc = false;

bool make_buf(Buf& b, size_t size) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(g_dev, &bi, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(g_dev, b.buf, &r);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = mem_any(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_dev, &ai, nullptr, &b.mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(g_dev, b.buf, b.mem, 0);
    vkMapMemory(g_dev, b.mem, 0, size, 0, &b.map);
    b.size = size;
    return true;
}
void free_buf(Buf& b) {
    if (b.buf) vkDestroyBuffer(g_dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(g_dev, b.mem, nullptr);
    b = Buf{};
}

bool make_tex(Tex& t, int w, int h) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {(uint32_t)w, (uint32_t)h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_dev, &ii, nullptr, &t.img) != VK_SUCCESS) return false;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(g_dev, t.img, &r);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = mem_any(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_dev, &ai, nullptr, &t.mem) != VK_SUCCESS) return false;
    vkBindImageMemory(g_dev, t.img, t.mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(g_dev, &vi, nullptr, &t.view);
    t.w = w;
    t.h = h;
    t.valid = false;
    return true;
}
void free_tex(Tex& t) {
    if (t.view) vkDestroyImageView(g_dev, t.view, nullptr);
    if (t.img) vkDestroyImage(g_dev, t.img, nullptr);
    if (t.mem) vkFreeMemory(g_dev, t.mem, nullptr);
    t = Tex{};
}

void write_ds(VkDescriptorSet ds, VkImageView v, VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    VkDescriptorImageInfo ii{g_sampler, v, layout};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = ds;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g_dev, 1, &w, 0, nullptr);
}

// record: upload staging -> texture (whole image), leaving it ready for sampling
void upload(VkCommandBuffer cmd, Buf& st, Tex& t) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = t.valid ? VK_ACCESS_SHADER_READ_BIT : 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = t.valid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {(uint32_t)t.w, (uint32_t)t.h, 1};
    vkCmdCopyBufferToImage(cmd, st.buf, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    t.valid = true;
}

VkShaderModule module(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g_dev, &ci, nullptr, &m);
    return m;
}

VkPipeline make_pipeline(bool blend) {
    VkShaderModule vs = module(SPV_PRESENT_VERT, sizeof(SPV_PRESENT_VERT)), fs = module(SPV_PRESENT_FRAG, sizeof(SPV_PRESENT_FRAG));
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = blend ? 0xF : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT);
    if (blend) {  // premultiplied alpha
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vin;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &ds;
    pi.layout = g_layout;
    pi.renderPass = g_rp;
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p);
    vkDestroyShaderModule(g_dev, vs, nullptr);
    vkDestroyShaderModule(g_dev, fs, nullptr);
    return p;
}

void destroy_swapchain() {
    if (!g_dev) return;
    {
        std::lock_guard<std::mutex> q(g_qm);
        vkDeviceWaitIdle(g_dev);
    }
    for (VkFramebuffer f : g_fbs) vkDestroyFramebuffer(g_dev, f, nullptr);
    for (VkImageView v : g_views) vkDestroyImageView(g_dev, v, nullptr);
    for (VkSemaphore s : g_render_done) vkDestroySemaphore(g_dev, s, nullptr);
    g_fbs.clear();
    g_views.clear();
    g_images.clear();
    g_render_done.clear();
    if (g_swap) vkDestroySwapchainKHR(g_dev, g_swap, nullptr);
    g_swap = VK_NULL_HANDLE;
}

bool create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surface, &caps);
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surface, &nf, fmts.data());
    VkSurfaceFormatKHR f = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
    for (auto& x : fmts)
        if (x.format == VK_FORMAT_R8G8B8A8_UNORM) f = x;
    g_swap_format = f.format;
    // the window's own (landscape) size with the identity transform: the system rotates if needed
    int ww = ANativeWindow_getWidth(g_window), wh = ANativeWindow_getHeight(g_window);
    g_extent = {(uint32_t)std::clamp<int>(ww, caps.minImageExtent.width, caps.maxImageExtent.width),
                (uint32_t)std::clamp<int>(wh, caps.minImageExtent.height, caps.maxImageExtent.height)};
    VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    si.surface = g_surface;
    si.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) si.minImageCount = std::min(si.minImageCount, caps.maxImageCount);
    si.imageFormat = f.format;
    si.imageColorSpace = f.colorSpace;
    si.imageExtent = g_extent;
    si.imageArrayLayers = 1;
    si.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    si.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    si.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                          : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    si.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    si.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(g_dev, &si, nullptr, &g_swap) != VK_SUCCESS) return false;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(g_dev, g_swap, &n, nullptr);
    g_images.resize(n);
    vkGetSwapchainImagesKHR(g_dev, g_swap, &n, g_images.data());
    if (!g_rp) {
        VkAttachmentDescription a{};
        a.format = g_swap_format;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &a;
        rp.subpassCount = 1;
        rp.pSubpasses = &sp;
        rp.dependencyCount = 1;
        rp.pDependencies = &dep;
        vkCreateRenderPass(g_dev, &rp, nullptr, &g_rp);
        g_pipe_opaque = make_pipeline(false);
        g_pipe_blend = make_pipeline(true);
    }
    for (VkImage img : g_images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g_swap_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        vkCreateImageView(g_dev, &vi, nullptr, &v);
        g_views.push_back(v);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g_rp;
        fi.attachmentCount = 1;
        fi.pAttachments = &v;
        fi.width = g_extent.width;
        fi.height = g_extent.height;
        fi.layers = 1;
        VkFramebuffer fb;
        vkCreateFramebuffer(g_dev, &fi, nullptr, &fb);
        g_fbs.push_back(fb);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s;
        vkCreateSemaphore(g_dev, &sci, nullptr, &s);
        g_render_done.push_back(s);
    }
    log_write(LOG_MAIN, "vulkan: swapchain %ux%u, %u images, format %d", g_extent.width, g_extent.height, n, (int)g_swap_format);
    return true;
}
}  // namespace

static bool init_device();

static bool g_fallback = false;
bool init() {
    if (g_dev) return true;
    if (getenv("GRANDIA_NO_VULKAN")) return false;  // test: a phone without usable Vulkan (software renderer, CPU presentation)
    std::string want = g3_gpu_driver_setting();
    if (vkdyn::load(want) && init_device()) {
        // which driver really answers (libadrenotools quietly falls back to the vendor driver when the
        // custom one does not load): the driver id of VK_KHR_driver_properties (core in Vulkan 1.2)
        VkPhysicalDeviceDriverProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &dp;
        if (vkGetPhysicalDeviceProperties2) vkGetPhysicalDeviceProperties2(g_pd, &p2);
        log_write(LOG_MAIN, "vulkan: driver %s (id %d, %s %s)", vkdyn::driver_label().c_str(), (int)dp.driverID, dp.driverName, dp.driverInfo);
        if (!want.empty() && dp.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
            log_write(LOG_MAIN, "vulkan: the custom driver '%s' did not load, the phone's own driver answers", want.c_str());
            g_fallback = true;
        }
        return true;
    }
    if (want.empty()) return false;
    // the custom driver failed: start again with the system driver (the half-made objects are dropped)
    log_write(LOG_MAIN, "vulkan: custom driver '%s' failed, using the system driver", want.c_str());
    g_dev = VK_NULL_HANDLE;
    g_inst = VK_NULL_HANDLE;
    g_pd = VK_NULL_HANDLE;
    g_fallback = true;
    return vkdyn::load("") && init_device();
}
bool driver_fallback() { return g_fallback; }

static bool init_device() {
    const char* iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Grandia3";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    if (vkCreateInstance(&ici, nullptr, &g_inst) != VK_SUCCESS) return false;
    vkdyn::load_instance(g_inst);
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(g_inst, &n, &g_pd) < 0 || n == 0) return false;
    vkGetPhysicalDeviceMemoryProperties(g_pd, &g_mem);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, qf.data());
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_qfam = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = g_qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    // the GS renderer needs ordered read-modify-write of attachments and push descriptors
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_pd, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> exts(ne);
    vkEnumerateDeviceExtensionProperties(g_pd, nullptr, &ne, exts.data());
    auto has = [&](const char* n) {
        for (auto& e : exts)
            if (!strcmp(e.extensionName, n)) return true;
        return false;
    };
    std::vector<const char*> dext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT ro{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &ro;
    vkGetPhysicalDeviceFeatures2(g_pd, &f2);
    // Adreno: the EXT name; Mali (Exynos, Tensor, Dimensity) may offer the same as the older ARM extension
    // (identical structure and flags)
    const char* ro_ext = has(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME) ? VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME
                         : has("VK_ARM_rasterization_order_attachment_access")       ? "VK_ARM_rasterization_order_attachment_access"
                                                                                     : nullptr;
    if (ro_ext && ro.rasterizationOrderColorAttachmentAccess) {
        dext.push_back(ro_ext);
        g_raster_order = true;
    }
    ro.rasterizationOrderDepthAttachmentAccess = ro.rasterizationOrderStencilAttachmentAccess = VK_FALSE;
    ro.pNext = nullptr;
    if (has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        dext.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        g_push_desc = true;
    }
    VkPhysicalDeviceFeatures2 en{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    en.pNext = g_raster_order ? &ro : nullptr;
    en.features.fragmentStoresAndAtomics = f2.features.fragmentStoresAndAtomics;
    en.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &en;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = (uint32_t)dext.size();
    di.ppEnabledExtensionNames = dext.data();
    if (vkCreateDevice(g_pd, &di, nullptr, &g_dev) != VK_SUCCESS) return false;
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);

    VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sm.magFilter = sm.minFilter = VK_FILTER_LINEAR;
    sm.addressModeU = sm.addressModeV = sm.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(g_dev, &sm, nullptr, &g_sampler);
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &b;
    vkCreateDescriptorSetLayout(g_dev, &dl, nullptr, &g_dsl);
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    vkCreatePipelineLayout(g_dev, &pl, nullptr, &g_layout);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * FRAMES + 1};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 2 * FRAMES + 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    vkCreateDescriptorPool(g_dev, &dp, nullptr, &g_dpool);
    for (Frame& f : g_frames) {
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cp.queueFamilyIndex = g_qfam;
        vkCreateCommandPool(g_dev, &cp, nullptr, &f.pool);
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = f.pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        vkAllocateCommandBuffers(g_dev, &ca, &f.cmd);
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(g_dev, &fc, nullptr, &f.fence);
        VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(g_dev, &sc, nullptr, &f.acquired);
        make_buf(f.staging, 2048 * 1024 * 4);
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = g_dpool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &g_dsl;
        vkAllocateDescriptorSets(g_dev, &da, &f.ds_game);
        vkAllocateDescriptorSets(g_dev, &da, &f.ds_ext);
    }
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = g_dpool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &g_dsl;
    vkAllocateDescriptorSets(g_dev, &da, &g_ds_ovl);
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g_pd, &p);
    log_write(LOG_MAIN, "vulkan: device %s ready (raster order attachment access %d, push descriptors %d)", p.deviceName,
              (int)g_raster_order, (int)g_push_desc);
    return true;
}

bool set_window(ANativeWindow* w) {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_dev) return false;
    destroy_swapchain();
    if (g_surface) vkDestroySurfaceKHR(g_inst, g_surface, nullptr);
    g_surface = VK_NULL_HANDLE;
    g_window = w;
    if (!w) return true;
    VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    si.window = w;
    if (vkCreateAndroidSurfaceKHR(g_inst, &si, nullptr, &g_surface) != VK_SUCCESS) return false;
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qfam, g_surface, &ok);
    return ok && create_swapchain();
}

bool ready() { return g_swap != VK_NULL_HANDLE; }

namespace {
// px: CPU picture (uploaded), or null: ext is the picture
bool present_common(const u32* px, VkImageView ext, int w, int h, int stride, int src_y0, int src_h, int dx0, int dy0, int dx1, int dy1,
                    const u32* overlay, int ow, int oh, bool overlay_new) {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_swap || w <= 0 || h <= 0) return false;
    Frame& f = g_frames[g_fi];
    vkWaitForFences(g_dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(g_dev, g_swap, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        destroy_swapchain();
        create_swapchain();
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;
    vkResetFences(g_dev, 1, &f.fence);
    // the picture into this frame's staging buffer and texture
    if (px) {
        if (f.game.w != w || f.game.h != h) {
            free_tex(f.game);
            make_tex(f.game, w, h);
            write_ds(f.ds_game, f.game.view);
        }
        for (int y = 0; y < h; y++) memcpy((u8*)f.staging.map + (size_t)y * w * 4, px + (size_t)y * stride, (size_t)w * 4);
    } else {
        write_ds(f.ds_ext, ext, VK_IMAGE_LAYOUT_GENERAL);  // the frame's previous use of the set is finished (fence above)
    }
    bool ovl = overlay && ow > 0 && oh > 0;
    if (ovl && (overlay_new || !g_ovl.valid || g_ovl.w != ow || g_ovl.h != oh)) {
        // the shared overlay texture changes: no frame may still read it
        for (Frame& o : g_frames)
            if (&o != &f) vkWaitForFences(g_dev, 1, &o.fence, VK_TRUE, UINT64_MAX);
        if (g_ovl.w != ow || g_ovl.h != oh) {
            free_tex(g_ovl);
            make_tex(g_ovl, ow, oh);
            write_ds(g_ds_ovl, g_ovl.view);
        }
        if (f.staging_ovl.size < (size_t)ow * oh * 4) {
            free_buf(f.staging_ovl);
            make_buf(f.staging_ovl, (size_t)ow * oh * 4);
        }
        memcpy(f.staging_ovl.map, overlay, (size_t)ow * oh * 4);
    } else {
        overlay_new = false;
    }
    VkCommandBuffer cmd = f.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    if (px) upload(cmd, f.staging, f.game);
    if (ovl && (overlay_new || !g_ovl.valid)) upload(cmd, f.staging_ovl, g_ovl);
    VkClearValue clear{};
    clear.color = {{0, 0, 0, 1}};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g_rp;
    rb.framebuffer = g_fbs[idx];
    rb.renderArea = {{0, 0}, g_extent};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)g_extent.width, (float)g_extent.height, 0, 1};
    VkRect2D sc{{0, 0}, g_extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    auto ndc_x = [&](int x) { return 2.0f * x / g_extent.width - 1.0f; };
    auto ndc_y = [&](int y) { return 2.0f * y / g_extent.height - 1.0f; };
    float pc[8] = {ndc_x(dx0), ndc_y(dy0), ndc_x(dx1), ndc_y(dy1), 0.0f, (float)src_y0 / h, 1.0f, (float)(src_y0 + src_h) / h};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe_opaque);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, px ? &f.ds_game : &f.ds_ext, 0, nullptr);
    vkCmdPushConstants(cmd, g_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
    vkCmdDraw(cmd, 4, 1, 0, 0);
    if (ovl && g_ovl.valid) {
        float po[8] = {-1, -1, 1, 1, 0, 0, 1, 1};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe_blend);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &g_ds_ovl, 0, nullptr);
        vkCmdPushConstants(cmd, g_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(po), po);
        vkCmdDraw(cmd, 4, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_render_done[idx];
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_render_done[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swap;
    pi.pImageIndices = &idx;
    {
        std::lock_guard<std::mutex> q(g_qm);
        vkQueueSubmit(g_queue, 1, &si, f.fence);
        r = vkQueuePresentKHR(g_queue, &pi);
    }
    g_fi = (g_fi + 1) % FRAMES;
    // SUBOPTIMAL comes with every frame while the system rotates the landscape picture (identity
    // transform on a portrait display): harmless, only a size change needs a new swapchain
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        destroy_swapchain();
        create_swapchain();
    }
    return true;
}
}  // namespace

bool present(const u32* px, int w, int h, int stride, int src_y0, int src_h, int dx0, int dy0, int dx1, int dy1,
             const u32* overlay, int ow, int oh, bool overlay_new) {
    return px && present_common(px, VK_NULL_HANDLE, w, h, stride, src_y0, src_h, dx0, dy0, dx1, dy1, overlay, ow, oh, overlay_new);
}
bool present_image(VkImageView view, int w, int h, int src_y0, int src_h, int dx0, int dy0, int dx1, int dy1,
                   const u32* overlay, int ow, int oh, bool overlay_new) {
    return view && present_common(nullptr, view, w, h, 0, src_y0, src_h, dx0, dy0, dx1, dy1, overlay, ow, oh, overlay_new);
}

VkDevice device() { return g_dev; }
VkPhysicalDevice physical() { return g_pd; }
VkQueue queue() { return g_queue; }
uint32_t queue_family() { return g_qfam; }
uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}
std::mutex& queue_mutex() { return g_qm; }
bool raster_order() { return g_raster_order; }
bool push_descriptors() { return g_push_desc; }

}  // namespace vkc
