// GPU renderer of the Android version: the GS pixel pipeline in a Vulkan fragment shader, at an
// integer multiple S of the native resolution. Port of gs_gpu.cpp (Direct3D 11.3); the logic sections
// (layouts, per-block state, surfaces and cells, refresh/readback, decoded textures, batching, HUD
// transform, CLUT/upload/copy, savestates) are kept as close to it as possible.
//
// Differences to the Direct3D version (see docs/VULKAN_RENDERER.md):
//  - ordered read-modify-write: the frame and z surfaces are color attachments that are also input
//    attachments of the same subpass with rasterization order attachment access (no rasterizer ordered
//    views on mobile GPUs). A render pass instance stays open while the targets stay the same.
//  - everything is recorded into per-frame command buffers: a "pre" buffer (texture uploads, new
//    image layouts; never breaks the render pass) submitted before the "main" one (copies, compute,
//    draws, composition). A decoded texture already used by the open main buffer gets a new image.
//  - vertices, constants, CLUT copies and uploads come from a host-visible ring per frame.
//  - readbacks (surface -> shadow copy) submit and wait, like Map on Direct3D.
//  - no HD texture replacement yet.
#include "gs/gs_gpu.h"
#include <array>
#include "gs/gs_drawstate.h"
#include "gs/gs_mem.h"
#include "gs/gs_texrep.h"
#include "platform/platform.h"
#include "ee/kernel.h"
#include "vk_ctx.h"
#include "shaders_spv.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <atomic>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <thread>

extern std::string g3_android_dir;  // win32_shim.cpp (the app folder)
#include <unordered_set>
#include <vector>

namespace platform {
bool present_gpu(VkImageView view, int w, int h, const u32* px);  // platform_android.cpp
bool interval_shot_due();
void write_shot(const u32* pixels, int width, int height, int stride);
}  // namespace platform

namespace gs {
extern bool (*g_sw_prim_hook)(const DrawState& ds, int prim, const Vertex* v, Vertex* out, int* scax0, int* scax1);
extern void (*g_sw_present_hook)(const PresentParams& p);

namespace gpu {

// options of the PC renderer (command line); most have no effect here yet
bool g_hd_ab = false, g_sharpen_ab = false;
double g_hud_log_at = 0;
bool g_no_uav_overlap = true, g_gpu_timing = false, g_no_batch_join = false, g_frame_hashes = false, g_gpu_profile = false;

static bool g_enabled = false;
static bool g_cfg_mips = true;    // use the game's mip levels
static bool g_no_batch_join_barrier = false;  // test: GRANDIA_VK_NO_DRAW_BARRIER
static int S = 1;
static u32* shadow = nullptr;
static int g_last_ow = 0, g_last_oh = 0;

// ===========================================================================
// Vulkan basics
// ===========================================================================
static VkDevice dev = VK_NULL_HANDLE;
static PFN_vkCmdPushDescriptorSetKHR pushDS = nullptr;
static VkDeviceSize g_align = 256;  // offsets in the ring (uniform/storage/copy alignment)

struct Img {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    int w = 0, h = 0;
};
struct Buf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    u8* map = nullptr;
    size_t size = 0;
    bool coherent = true;
};

static void destroy_now(Img& i) {
    if (i.view) vkDestroyImageView(dev, i.view, nullptr);
    if (i.img) vkDestroyImage(dev, i.img, nullptr);
    if (i.mem) vkFreeMemory(dev, i.mem, nullptr);
    i = Img{};
}
static void destroy_now(Buf& b) {
    if (b.buf) vkDestroyBuffer(dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev, b.mem, nullptr);
    b = Buf{};
}

static bool make_buf(Buf& b, size_t size, VkBufferUsageFlags usage, bool readback) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    if (vkCreateBuffer(dev, &bi, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, b.buf, &r);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size;
    u32 t = ~0u;
    if (readback) {
        t = vkc::mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (t == ~0u) t = vkc::mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    if (t == ~0u) t = vkc::mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t == ~0u) return false;
    ai.memoryTypeIndex = t;
    if (vkAllocateMemory(dev, &ai, nullptr, &b.mem) != VK_SUCCESS) { destroy_now(b); return false; }
    vkBindBufferMemory(dev, b.buf, b.mem, 0);
    void* m = nullptr;
    vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &m);
    b.map = (u8*)m;
    b.size = size;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vkc::physical(), &mp);
    b.coherent = (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return true;
}
static void invalidate(Buf& b) {
    if (b.coherent) return;
    VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    r.memory = b.mem;
    r.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(dev, 1, &r);
}

static const VkImageUsageFlags SURF_USAGE = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
static const VkImageUsageFlags TEX_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

static std::vector<VkImage> g_new_images;  // created since the pre buffer was recorded: layout -> GENERAL there

static bool make_img(Img& i, int w, int h, VkFormat f, VkImageUsageFlags usage) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = f;
    ii.extent = {(uint32_t)w, (uint32_t)h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, nullptr, &i.img) != VK_SUCCESS) return false;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, i.img, &r);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = r.size;
    u32 t = vkc::mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == ~0u) t = vkc::mem_type(r.memoryTypeBits, 0);
    ai.memoryTypeIndex = t;
    if (vkAllocateMemory(dev, &ai, nullptr, &i.mem) != VK_SUCCESS) {
        vkDestroyImage(dev, i.img, nullptr);
        i = Img{};
        return false;
    }
    vkBindImageMemory(dev, i.img, i.mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = i.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev, &vi, nullptr, &i.view);
    i.w = w;
    i.h = h;
    g_new_images.push_back(i.img);
    return true;
}

// ---------------------------------------------------------------------------
// Frames: command buffers, ring, deferred destruction
// ---------------------------------------------------------------------------
constexpr int NF = 2;
static const size_t RING_BYTES = (size_t)48 << 20;
struct FrameRes {
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds;
    size_t ncmd = 0;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
    Buf ring;
    size_t ring_pos = 0;
    std::vector<Img> garbage;
    std::vector<VkFramebuffer> garbage_fb;
    Buf movie;            // copy of the composed picture during movies (bar detection)
    int movie_w = 0, movie_h = 0;
    bool movie_valid = false;
};
static FrameRes g_fr[NF];
static int g_fi = 0;
static VkCommandBuffer g_pre = VK_NULL_HANDLE, g_main = VK_NULL_HANDLE;
static u64 g_cmd_serial = 1;   // serial of the open main buffer
static u64 g_ring_gen = 1;     // changes whenever the ring restarts (CLUT copy must be rewritten)
static std::vector<u32> g_movie_px;
static int g_movie_w = 0, g_movie_h = 0;

static FrameRes& fr() { return g_fr[g_fi]; }
static void garbage(Img& i) {
    if (i.img) fr().garbage.push_back(i);
    i = Img{};
}

static VkCommandBuffer new_cmd() {
    FrameRes& f = fr();
    if (f.ncmd == f.cmds.size()) {
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = f.pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        VkCommandBuffer c;
        vkAllocateCommandBuffers(dev, &ca, &c);
        f.cmds.push_back(c);
    }
    VkCommandBuffer c = f.cmds[f.ncmd++];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c, &bi);
    return c;
}

static void full_barrier(VkCommandBuffer c) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

// --gpu-timing: GPU time from the start of a frame's first buffer to the end of its last one
static VkQueryPool g_qpool = VK_NULL_HANDLE;
static float g_ts_period = 1;
static bool g_ts_written[NF];
// --gpu-profile: timestamps around the render passes and the work between them; per second the GPU
// time by kind and the most expensive passes (surface size, draws)
static constexpr int PQ = 2048;
static VkQueryPool g_ppool = VK_NULL_HANDLE;
struct PEv { char kind; int w, h, draws; };
static std::vector<PEv> g_pev[NF];
static void prof_ts(VkCommandBuffer c, char kind, int w = 0, int h = 0) {
    if (!g_gpu_profile || !g_ppool || !c) return;
    std::vector<PEv>& ev = g_pev[g_fi];
    if ((int)ev.size() >= PQ) return;
    vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_ppool, (uint32_t)(g_fi * PQ + ev.size()));
    ev.push_back({kind, w, h, 0});
}
static void prof_draw() {
    if (g_gpu_profile && !g_pev[g_fi].empty()) g_pev[g_fi].back().draws++;
}
static void prof_read(int slot) {
    std::vector<PEv>& ev = g_pev[slot];
    if (!g_gpu_profile || !g_ppool || ev.empty()) return;
    static std::vector<u64> t;
    t.resize(ev.size());
    VkResult r = vkGetQueryPoolResults(dev, g_ppool, (uint32_t)(slot * PQ), (uint32_t)ev.size(), t.size() * 8, t.data(), 8, VK_QUERY_RESULT_64_BIT);
    static double sum[128], last = 0;
    static int frames = 0;
    struct Top { double ms; int w, h, draws; };
    static std::vector<Top> passes;
    if (r == VK_SUCCESS) {
        frames++;
        for (size_t i = 1; i < ev.size(); i++) {
            if (ev[i].kind == 'P') continue;  // a new submission: the CPU gap before it is not GPU time
            double ms = (double)(t[i] - t[i - 1]) * g_ts_period / 1e6;
            if (ms < 0 || ms > 1000) continue;
            char k = ev[i].kind;
            if (k == 'E') passes.push_back({ms, ev[i - 1].w, ev[i - 1].h, ev[i - 1].draws});
            sum[(u8)k & 127] += ms;
        }
    }
    ev.clear();
    double now = host_time_sec();
    if (now - last >= 1.0 && frames) {
        // M: pre buffer (texture uploads), E: render passes, c: refresh copies, u: refresh uploads,
        // s: self-read snapshots, B: other work before a pass, X: output merge + end of frame
        char b[512];
        double tot = 0;
        for (double v : sum) tot += v;
        snprintf(b, sizeof b, "gpu profile per frame: total %.2f ms = pre %.2f, passes %.2f (%.1f), refresh copies %.2f, refresh uploads %.2f, snapshots %.2f, other %.2f, end %.2f",
                 tot / frames, sum['M'] / frames, sum['E'] / frames, (double)passes.size() / frames, sum['c'] / frames, sum['u'] / frames,
                 sum['s'] / frames, sum['B'] / frames, sum['X'] / frames);
        log_write(LOG_GS, "%s", b);
        std::sort(passes.begin(), passes.end(), [](const Top& a, const Top& b) { return a.ms > b.ms; });
        std::string tp;
        for (size_t i = 0; i < passes.size() && i < 8; i++) {
            char x[64];
            snprintf(x, sizeof x, " %.2fms %dx%d/%dd", passes[i].ms, passes[i].w, passes[i].h, passes[i].draws);
            tp += x;
        }
        log_write(LOG_GS, "gpu profile: slowest passes%s", tp.c_str());
        for (double& v : sum) v = 0;
        passes.clear();
        frames = 0;
        last = now;
    }
}
static void begin_cmds(bool frame_start = false) {
    g_pre = new_cmd();
    g_main = new_cmd();
    // everything earlier on the queue (other frame, presentation) before this frame's work
    full_barrier(g_pre);
    full_barrier(g_main);
    if (g_gpu_profile && g_ppool) {
        if (frame_start || g_pev[g_fi].empty()) {
            g_pev[g_fi].clear();
            vkCmdResetQueryPool(g_pre, g_ppool, (uint32_t)(g_fi * PQ), PQ);
        }
        prof_ts(g_pre, 'P');
        prof_ts(g_main, 'M');
    }
    if (frame_start && g_gpu_timing && g_qpool) {
        vkCmdResetQueryPool(g_pre, g_qpool, 2 * g_fi, 2);
        vkCmdWriteTimestamp(g_pre, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_qpool, 2 * g_fi);
        g_ts_written[g_fi] = false;
    }
    g_cmd_serial++;
}
static void timing_read(int slot) {
    static double sum = 0, mx = 0, last = 0;
    static int cnt = 0;
    if (!g_gpu_timing || !g_qpool || !g_ts_written[slot]) return;
    g_ts_written[slot] = false;
    u64 t[2];
    if (vkGetQueryPoolResults(dev, g_qpool, 2 * slot, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    double ms = (double)(t[1] - t[0]) * g_ts_period / 1e6;
    sum += ms;
    mx = std::max(mx, ms);
    cnt++;
    double now = host_time_sec();
    if (now - last >= 1.0) {
        log_write(LOG_GS, "gpu frame time avg %.2f ms, max %.2f ms (%d frames)", sum / cnt, mx, cnt);
        sum = mx = 0;
        cnt = 0;
        last = now;
    }
}

// new images get their layout (GENERAL, kept forever) at the end of the pre buffer
static void pre_layouts() {
    if (g_new_images.empty()) return;
    std::vector<VkImageMemoryBarrier> b(g_new_images.size());
    for (size_t i = 0; i < b.size(); i++) {
        b[i] = VkImageMemoryBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b[i].srcAccessMask = 0;
        b[i].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = g_new_images[i];
        b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(g_pre, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                         (uint32_t)b.size(), b.data());
    g_new_images.clear();
}

// texture uploads go into the pre buffer, after the layout of a new image
struct PendingUp { VkImage img; VkDeviceSize off; int w, h; };
static std::vector<PendingUp> g_pre_up;
static void pre_flush_uploads() {
    pre_layouts();
    if (g_pre_up.empty()) return;
    for (const PendingUp& u : g_pre_up) {
        VkBufferImageCopy c{};
        c.bufferOffset = u.off;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {(uint32_t)u.w, (uint32_t)u.h, 1};
        vkCmdCopyBufferToImage(g_pre, fr().ring.buf, u.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    g_pre_up.clear();
}

static void end_pass();
static void recycle(FrameRes& f) {
    for (Img& i : f.garbage) destroy_now(i);
    f.garbage.clear();
    for (VkFramebuffer b : f.garbage_fb) vkDestroyFramebuffer(dev, b, nullptr);
    f.garbage_fb.clear();
    vkResetCommandPool(dev, f.pool, 0);
    f.ncmd = 0;
    f.ring_pos = 0;
    g_ring_gen++;
}

// submit the open buffers; fence of the current frame
static void submit() {
    end_pass();
    pre_flush_uploads();
    full_barrier(g_pre);
    full_barrier(g_main);
    vkEndCommandBuffer(g_pre);
    vkEndCommandBuffer(g_main);
    VkCommandBuffer cb[2] = {g_pre, g_main};
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 2;
    si.pCommandBuffers = cb;
    FrameRes& f = fr();
    {
        std::lock_guard<std::mutex> q(vkc::queue_mutex());
        vkQueueSubmit(vkc::queue(), 1, &si, f.fence);
    }
    f.pending = true;
    g_pre = g_main = VK_NULL_HANDLE;
}
static void wait_frame(FrameRes& f) {
    if (!f.pending) return;
    vkWaitForFences(dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &f.fence);
    f.pending = false;
}
static void read_movie(FrameRes& f) {
    if (!f.movie_valid) return;
    invalidate(f.movie);
    g_movie_px.resize((size_t)f.movie_w * f.movie_h);
    memcpy(g_movie_px.data(), f.movie.map, g_movie_px.size() * 4);
    g_movie_w = f.movie_w;
    g_movie_h = f.movie_h;
    f.movie_valid = false;
}
// mid-frame: submit, wait for the GPU, continue with fresh buffers (readbacks, a full ring)
static void submit_wait() {
    submit();
    FrameRes& f = fr();
    wait_frame(f);
    read_movie(f);
    recycle(f);
    begin_cmds();
}
// end of frame: submit, continue on the next frame's resources
static void next_frame() {
    submit();
    g_fi = (g_fi + 1) % NF;
    FrameRes& f = fr();
    wait_frame(f);
    read_movie(f);
    recycle(f);
    begin_cmds();
}

// room for several allocations that must come from the same ring generation (a full ring submits
// and starts over, which would lose earlier allocations whose commands are not recorded yet)
static void ring_reserve(size_t bytes) {
    FrameRes& f = fr();
    size_t a = (f.ring_pos + g_align - 1) & ~(size_t)(g_align - 1);
    if (a + bytes > f.ring.size) submit_wait();
}
// host-visible ring memory of the current frame
static VkDeviceSize ring_alloc(size_t bytes, u8** p) {
    FrameRes* f = &fr();
    size_t a = (f->ring_pos + g_align - 1) & ~(size_t)(g_align - 1);
    if (a + bytes > f->ring.size) {
        submit_wait();
        f = &fr();
        a = 0;
        if (bytes > f->ring.size) { *p = nullptr; return 0; }
    }
    f->ring_pos = a + bytes;
    *p = f->ring.map + a;
    return a;
}

// ===========================================================================
// Layouts (as gs_gpu.cpp)
// ===========================================================================
enum Layout { L_C32, L_C16, L_C16S, L_Z32, L_Z16, L_Z16S, L_NONE };
static Layout layout_of(u32 psm) {
    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMT8H: case PSMT4HL: case PSMT4HH: return L_C32;
    case PSMCT16: return L_C16;
    case PSMCT16S: return L_C16S;
    case PSMZ32: case PSMZ24: return L_Z32;
    case PSMZ16: return L_Z16;
    case PSMZ16S: return L_Z16S;
    }
    return L_NONE;
}
static inline bool is16(Layout l) { return l == L_C16 || l == L_C16S || l == L_Z16 || l == L_Z16S; }
static int psm_family(u32 psm) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: return 8;
    case PSMT4: return 4;
    }
    return 32;
}
static inline u32 raw_addr(Layout l, int x, int y, u32 bp, u32 bw) {
    switch (l) {
    case L_C32: return addr32(x, y, bp, bw);
    case L_Z32: return addr32z(x, y, bp, bw);
    case L_C16: return addr16(x, y, bp, bw);
    case L_C16S: return addr16s(x, y, bp, bw);
    case L_Z16: return addr16z(x, y, bp, bw);
    default: return addr16sz(x, y, bp, bw);
    }
}
static inline u32 raw_block(Layout l, int x, int y, u32 bp, u32 bw) {
    u32 a = raw_addr(l, x, y, bp, bw);
    return (is16(l) ? (a >> 7) : (a >> 6)) & 16383;
}
static inline u32 shadow_read_raw(Layout l, int x, int y, u32 bp, u32 bw) {
    u32 a = raw_addr(l, x, y, bp, bw);
    return is16(l) ? ((const u16*)shadow)[a] : shadow[a];
}
static inline void shadow_write_raw(Layout l, int x, int y, u32 bp, u32 bw, u32 v) {
    u32 a = raw_addr(l, x, y, bp, bw);
    if (is16(l)) ((u16*)shadow)[a] = (u16)v;
    else shadow[a] = v;
}
static void psm_block_dims(u32 psm, int& bw, int& bh) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: bw = 16; bh = 8; break;
    case PSMT8: bw = 16; bh = 16; break;
    case PSMT4: bw = 32; bh = 16; break;
    default: bw = 8; bh = 8; break;
    }
}
static u32 psm_block(u32 psm, int x, int y, u32 bp, u32 bw) {
    switch (psm) {
    case PSMCT16: return (addr16(x, y, bp, bw) >> 7) & 16383;
    case PSMCT16S: return (addr16s(x, y, bp, bw) >> 7) & 16383;
    case PSMZ16: return (addr16z(x, y, bp, bw) >> 7) & 16383;
    case PSMZ16S: return (addr16sz(x, y, bp, bw) >> 7) & 16383;
    case PSMT8: return (addr8(x, y, bp, bw) >> 8) & 16383;
    case PSMT4: return (addr4(x, y, bp, bw) >> 9) & 16383;
    case PSMZ32: case PSMZ24: return (addr32z(x, y, bp, bw) >> 6) & 16383;
    default: return (addr32(x, y, bp, bw) >> 6) & 16383;
    }
}
template <class F>
static void for_rect_blocks(u32 psm, u32 bp, u32 bw, int x0, int y0, int x1, int y1, F f) {
    int cw, ch;
    psm_block_dims(psm, cw, ch);
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(2048, x1); y1 = std::min(2048, y1);
    for (int y = y0 & ~(ch - 1); y < y1; y += ch)
        for (int x = x0 & ~(cw - 1); x < x1; x += cw) {
            bool full = x >= x0 && y >= y0 && x + cw <= x1 && y + ch <= y1;
            f(psm_block(psm, x, y, bp, bw), full);
        }
}

// ===========================================================================
// Per-block state (as gs_gpu.cpp)
// ===========================================================================
static s16 g_owner[16384];
static u8 g_shadow_ok[16384];
static u64 g_stamp[16384];
static u64 g_stamp_next = 1;
static u16 g_page_stale[512];
static u64 g_page_stamp[512];
static inline void set_shadow_ok(u32 b, bool ok) {
    if (g_shadow_ok[b] == (u8)ok) return;
    g_shadow_ok[b] = ok;
    if (ok) g_page_stale[b >> 5]--;
    else g_page_stale[b >> 5]++;
}
static inline void stamp_block(u32 b) {
    g_stamp[b] = g_stamp_next++;
    g_page_stamp[b >> 5] = g_stamp[b];
}

static u64 st_draws, st_batches, st_prims, st_refresh_cells, st_readback_cells, st_readbacks, st_tex_decodes, st_tex_hits,
    st_stale_texels, st_self, st_surfaces, st_passes, st_submits, st_tex_versions;

// ===========================================================================
// Surfaces
// ===========================================================================
struct Surface {
    int id = -1;
    u32 bp = 0, bw = 0;
    Layout lay = L_C32;
    int w = 0, h = 0;
    int cbw = 8, cbh = 8;
    int cw = 0, ch = 0;
    Img im;
    std::vector<u16> cell_block;
    std::vector<u8> dirty;
    std::vector<int> block_cell;
    int ndirty = 0;
    u64 last_use = 0;
};
static std::vector<Surface*> g_surf;
static u64 g_frame = 0;
static Surface* surf_by_id(int id) { return (id >= 0 && id < (int)g_surf.size()) ? g_surf[id] : nullptr; }

static bool create_tex(Surface* s) { return make_img(s->im, s->w * S, s->h * S, VK_FORMAT_R32_UINT, SURF_USAGE); }

static void build_cells(Surface* s) {
    int ph = is16(s->lay) ? 64 : 32;
    s->cbw = is16(s->lay) ? 16 : 8;
    s->cbh = 8;
    s->h = (s->h + ph - 1) / ph * ph;
    s->cw = s->w / s->cbw;
    s->ch = s->h / s->cbh;
    s->cell_block.assign((size_t)s->cw * s->ch, 0);
    s->block_cell.assign(16384, -1);
    for (int cy = 0; cy < s->ch; cy++)
        for (int cx = 0; cx < s->cw; cx++) {
            u32 b = raw_block(s->lay, cx * s->cbw, cy * s->cbh, s->bp, s->bw);
            int ci = cy * s->cw + cx;
            s->cell_block[ci] = (u16)b;
            if (s->block_cell[b] < 0) s->block_cell[b] = ci;
        }
}

static Surface* create_surface(u32 bp, u32 bw, Layout lay, int h) {
    Surface* s = new Surface;
    s->bp = bp;
    s->bw = std::max<u32>(bw, 1);
    s->lay = lay;
    s->w = (int)s->bw * 64;
    s->h = std::min(std::max(h, 32), 2048);
    build_cells(s);
    if (!create_tex(s)) {
        log_write(LOG_GS, "gpu: surface image %dx%d failed", s->w * S, s->h * S);
        delete s;
        return nullptr;
    }
    s->dirty.assign(s->cell_block.size(), 1);
    s->ndirty = (int)s->dirty.size();
    int id = -1;
    for (int i = 0; i < (int)g_surf.size(); i++)
        if (!g_surf[i]) { id = i; break; }
    if (id < 0) { id = (int)g_surf.size(); g_surf.push_back(nullptr); }
    s->id = id;
    g_surf[id] = s;
    st_surfaces++;
    log_write(LOG_GS, "gpu: surface %d %05x/%u layout %d %dx%d (x%d)", id, bp, s->bw, (int)lay, s->w, s->h, S);
    return s;
}

// framebuffers per (frame image, z image)
struct FbEntry { VkImage a, b; VkFramebuffer fb; };
static std::vector<FbEntry> g_fbs;
static void purge_fbs(VkImage img) {
    for (size_t i = 0; i < g_fbs.size();) {
        if (g_fbs[i].a == img || g_fbs[i].b == img) {
            fr().garbage_fb.push_back(g_fbs[i].fb);
            g_fbs.erase(g_fbs.begin() + i);
        } else i++;
    }
}

static void grow_surface(Surface* s, int h) {
    h = std::min(h, 2048);
    if (h <= s->h) return;
    Img old = s->im;
    int oldh = s->h;
    std::vector<u8> olddirty = s->dirty;
    s->im = Img{};
    s->h = h;
    build_cells(s);
    create_tex(s);
    end_pass();
    pre_layouts();  // the new image's layout before the copy below (pre runs first)
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {(uint32_t)(s->w * S), (uint32_t)(oldh * S), 1};
    vkCmdCopyImage(g_main, old.img, VK_IMAGE_LAYOUT_GENERAL, s->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(g_main);
    s->dirty.assign(s->cell_block.size(), 1);
    for (size_t i = 0; i < olddirty.size() && i < s->dirty.size(); i++) s->dirty[i] = olddirty[i];
    s->ndirty = 0;
    for (u8 d : s->dirty) s->ndirty += d;
    purge_fbs(old.img);
    garbage(old);
}

// ===========================================================================
// Pipeline objects
// ===========================================================================
static VkSampler g_nearest = VK_NULL_HANDLE, g_linear = VK_NULL_HANDLE;
static VkDescriptorSetLayout g_dsl_draw[2], g_dsl_util, g_dsl_merge, g_dsl_cell;
static VkPipelineLayout g_pl_draw[2], g_pl_util, g_pl_merge, g_pl_cell;
static VkRenderPass g_rp_draw[2], g_rp_out;
static VkPipeline g_pipe_draw[2], g_pipe_up, g_pipe_down, g_pipe_merge, g_pipe_cell;
static Img g_native;      // native-resolution scratch (refresh source / readback target)
static Buf g_rb;          // CPU readback of g_native / of the composed picture
static Img g_dummy_u, g_dummy_rgba;
static Img g_out;         // display composition (RGBA8)
static VkFramebuffer g_out_fb = VK_NULL_HANDLE;
static Img g_snap;        // copy of a surface sampled while drawn to

static VkShaderModule module(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

static VkDescriptorSetLayout make_dsl(const std::vector<VkDescriptorSetLayoutBinding>& b) {
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    ci.bindingCount = (uint32_t)b.size();
    ci.pBindings = b.data();
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(dev, &ci, nullptr, &l);
    return l;
}
static VkPipelineLayout make_pl(VkDescriptorSetLayout dsl, VkShaderStageFlags pcs, uint32_t pcbytes) {
    VkPushConstantRange pc{pcs, 0, pcbytes};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &dsl;
    ci.pushConstantRangeCount = pcbytes ? 1 : 0;
    ci.pPushConstantRanges = &pc;
    VkPipelineLayout l = VK_NULL_HANDLE;
    vkCreatePipelineLayout(dev, &ci, nullptr, &l);
    return l;
}

static VkRenderPass make_draw_rp(bool z) {
    VkAttachmentDescription a[2]{};
    for (auto& x : a) {
        x.format = VK_FORMAT_R32_UINT;
        x.samples = VK_SAMPLE_COUNT_1_BIT;
        x.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        x.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        x.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        x.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        x.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        x.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    VkAttachmentReference refs[2] = {{0, VK_IMAGE_LAYOUT_GENERAL}, {1, VK_IMAGE_LAYOUT_GENERAL}};
    VkSubpassDescription sp{};
    sp.flags = VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = z ? 2 : 1;
    sp.pColorAttachments = refs;
    sp.inputAttachmentCount = z ? 2 : 1;
    sp.pInputAttachments = refs;
    // feedback loop: a barrier between draws makes one draw's writes visible to the next one's reads
    VkSubpassDependency dep{};
    dep.srcSubpass = dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = z ? 2 : 1;
    rp.pAttachments = a;
    rp.subpassCount = 1;
    rp.pSubpasses = &sp;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    VkRenderPass r = VK_NULL_HANDLE;
    vkCreateRenderPass(dev, &rp, nullptr, &r);
    return r;
}

// draw pipelines specialized per batch: the fragment shader's constant_id 0..22 (draw.frag SC_*)
static constexpr int NSPEC = 23;
static bool g_texrep_on = false;   // texture replacement images exist (lang/<code>_tex, lang/all_tex)
static VkPipelineCache g_pcache = VK_NULL_HANDLE;
// g_draw_fs: specialized shader (draw.frag); g_draw_gen_fs: the general pipeline (draw_general.frag)
static VkShaderModule g_draw_vs = VK_NULL_HANDLE, g_draw_fs[2] = {}, g_draw_gen_fs[2] = {};
static VkPipeline make_draw_pipe(bool z, const u32* spec = nullptr) {
    if (!g_draw_vs) {
        g_draw_vs = module(SPV_DRAW_VERT, sizeof(SPV_DRAW_VERT));
        g_draw_fs[0] = module(SPV_DRAW_FRAG, sizeof(SPV_DRAW_FRAG));
        g_draw_fs[1] = module(SPV_DRAW_Z_FRAG, sizeof(SPV_DRAW_Z_FRAG));
        g_draw_gen_fs[0] = module(SPV_DRAW_GENERAL_FRAG, sizeof(SPV_DRAW_GENERAL_FRAG));
        g_draw_gen_fs[1] = module(SPV_DRAW_GENERAL_Z_FRAG, sizeof(SPV_DRAW_GENERAL_Z_FRAG));
    }
    VkSpecializationMapEntry me[NSPEC];
    for (int i = 0; i < NSPEC; i++) me[i] = {(uint32_t)i, (uint32_t)(i * 4), 4};
    VkSpecializationInfo si{NSPEC, me, NSPEC * 4, spec};
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, g_draw_vs, "main"};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, spec ? g_draw_fs[z] : g_draw_gen_fs[z], "main",
             spec ? &si : nullptr};
    VkVertexInputBindingDescription vb{0, 68, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[7] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},        {1, 0, VK_FORMAT_R32_SFLOAT, 8},  {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 28},    {4, 0, VK_FORMAT_R32_SFLOAT, 40}, {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 44},
        {6, 0, VK_FORMAT_R32G32_UINT, 60}};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &vb;
    vin.vertexAttributeDescriptionCount = 7;
    vin.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba[2]{};
    ba[0].colorWriteMask = ba[1].colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.flags = VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT;
    cb.attachmentCount = z ? 2 : 1;
    cb.pAttachments = ba;
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
    pi.layout = g_pl_draw[z];
    pi.renderPass = g_rp_draw[z];
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(dev, g_pcache, 1, &pi, nullptr, &p);
    if (r != VK_SUCCESS) log_write(LOG_GS, "gpu: draw pipeline (z %d) failed: %d", (int)z, (int)r);
    return p;
}

// the driver's compiled pipelines kept in the app folder (no compile stutter on the next start)
static std::string pcache_path() { return g3_android_dir + "/vk_pipelines.bin"; }
static void pcache_load() {
    std::vector<u8> data;
    if (FILE* f = fopen(pcache_path().c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < (64 << 20)) {
            data.resize((size_t)n);
            if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        }
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(dev, &ci, nullptr, &g_pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0;  // (data of another driver version: start empty)
        ci.pInitialData = nullptr;
        vkCreatePipelineCache(dev, &ci, nullptr, &g_pcache);
    }
    log_write(LOG_GS, "gpu: pipeline cache %zu bytes loaded", data.size());
}
static std::atomic<bool> g_pcache_dirty{false};
static void pcache_save() {
    static double last = 0;
    double now = host_time_sec();
    if (!g_pcache_dirty || !g_pcache || now - last < 5.0) return;
    last = now;
    g_pcache_dirty = false;
    size_t n = 0;
    if (vkGetPipelineCacheData(dev, g_pcache, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<u8> data(n);
    if (vkGetPipelineCacheData(dev, g_pcache, &n, data.data()) != VK_SUCCESS) return;
    std::string tmp = pcache_path() + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        bool ok = fwrite(data.data(), 1, n, f) == n;
        fclose(f);
        if (ok) rename(tmp.c_str(), pcache_path().c_str());
    }
}
static std::unordered_map<u64, VkPipeline> g_draw_pipes;  // (VK_NULL_HANDLE: being compiled)
static std::atomic<u64> st_pipes_new{0};
static double st_pipes_ms = 0;  // (under g_pjob_mu)
struct PJob { u64 key; bool z; u32 spec[NSPEC]; };
static std::mutex g_pjob_mu;
static std::condition_variable g_pjob_cv;
static std::deque<PJob> g_pjobs;
static std::vector<std::pair<u64, VkPipeline>> g_pjob_done;
static bool g_pjob_thread = false;
static VkPipeline draw_pipe_for(bool z, const u32* spec) {
    static const bool no_spec = getenv("GRANDIA_VK_NO_SPEC") != nullptr;  // test: the general pipeline only
    if (no_spec) return g_pipe_draw[z];
    // test GRANDIA_VK_SPEC_GEN=i:v: batches whose constant i is v draw with the general pipeline
    static int gen_i = -1; static u32 gen_v = 0;
    static const bool gen_init = [] {
        if (const char* e = getenv("GRANDIA_VK_SPEC_GEN")) { gen_i = atoi(e); if (const char* c = strchr(e, ':')) gen_v = (u32)atoi(c + 1); }
        return true;
    }();
    (void)gen_init;
    if (gen_i >= 0 && gen_i < NSPEC && spec[gen_i] == gen_v) return g_pipe_draw[z];
    u64 key = z ? 1 : 0;
    static const int BITS[NSPEC] = {1, 4, 2, 1, 2, 2, 1, 1, 1, 1, 1, 3, 1, 1, 1, 1, 2, 1, 1, 1, 2, 1, 2};
    int sh = 1;
    for (int i = 0; i < NSPEC; i++) {
        key |= (u64)(spec[i] & ((1u << BITS[i]) - 1)) << sh;
        sh += BITS[i];
    }
    // compiled pipelines from the background thread
    {
        std::lock_guard<std::mutex> lk(g_pjob_mu);
        for (auto& [k, p] : g_pjob_done) g_draw_pipes[k] = p;
        g_pjob_done.clear();
    }
    auto it = g_draw_pipes.find(key);
    if (it != g_draw_pipes.end()) return it->second ? it->second : g_pipe_draw[z];
    // a new combination: compiled in the background (a driver compile takes ~0.4 s on the phone, a
    // stutter per new effect); until then the general pipeline (the same shader without constants) draws
    g_draw_pipes[key] = VK_NULL_HANDLE;
    PJob j{key, z, {}};
    memcpy(j.spec, spec, sizeof j.spec);
    {
        std::lock_guard<std::mutex> lk(g_pjob_mu);
        g_pjobs.push_back(j);
        if (!g_pjob_thread) {
            g_pjob_thread = true;
            std::thread([] {
                for (;;) {
                    PJob job;
                    {
                        std::unique_lock<std::mutex> lk(g_pjob_mu);
                        g_pjob_cv.wait(lk, [] { return !g_pjobs.empty(); });
                        job = g_pjobs.front();
                        g_pjobs.pop_front();
                    }
                    double t0 = host_time_sec();
                    VkPipeline p = make_draw_pipe(job.z, job.spec);
                    double ms = (host_time_sec() - t0) * 1000.0;
                    std::lock_guard<std::mutex> lk(g_pjob_mu);
                    g_pjob_done.push_back({job.key, p});
                    st_pipes_new++;
                    st_pipes_ms += ms;
                    static int nlog = 0;
                    if (nlog++ < 40) log_write(LOG_GS, "gpu: draw pipeline (key %llx) in %.1f ms (background)", (unsigned long long)job.key, ms);
                    g_pcache_dirty = true;
                }
            }).detach();
        }
    }
    g_pjob_cv.notify_one();
    return g_pipe_draw[z];
}

static VkPipeline make_compute(const uint32_t* code, size_t bytes, VkPipelineLayout pl = g_pl_util) {
    VkShaderModule cs = module(code, bytes);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main"};
    ci.layout = pl;
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
    vkDestroyShaderModule(dev, cs, nullptr);
    return p;
}

static VkPipeline make_merge_pipe() {
    VkShaderModule vs = module(SPV_FULL_VERT, sizeof(SPV_FULL_VERT)), fs = module(SPV_MERGE_FRAG, sizeof(SPV_MERGE_FRAG));
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
    ba.colorWriteMask = 0xF;
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
    pi.layout = g_pl_merge;
    pi.renderPass = g_rp_out;
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p);
    vkDestroyShaderModule(dev, vs, nullptr);
    vkDestroyShaderModule(dev, fs, nullptr);
    return p;
}

static void clear_img(Img& i) {
    VkClearColorValue c{};
    VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(g_main, i.img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r);
}

bool init(int scale) {
    if (!vkc::init()) return false;
    if (!vkc::raster_order() || !vkc::push_descriptors()) {
        log_write(LOG_GS, "gpu: Vulkan lacks rasterization order attachment access or push descriptors, using the software renderer");
        platform::message(platform::config().language == "de"
                              ? L"Die Grafikeinheit dieses Handys kann den Grafik-Renderer nicht ausführen. Das Spiel läuft mit dem Software-Renderer und wird sehr langsam sein."
                              : L"This phone's graphics unit cannot run the GPU renderer. The game uses the software renderer and will be very slow.");
        return false;
    }
    dev = vkc::device();
    pushDS = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(dev, "vkCmdPushDescriptorSetKHR");
    if (!pushDS) return false;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vkc::physical(), &props);
    g_align = std::max<VkDeviceSize>({(VkDeviceSize)256, props.limits.minUniformBufferOffsetAlignment, props.limits.minStorageBufferOffsetAlignment,
                                      props.limits.optimalBufferCopyOffsetAlignment});
    if (scale <= 0) {
        // automatic: one internal line per screen line would be x3 on a 1220 line phone; the fragment
        // shader is long (every pixel reads and writes its target). x2 is measured on an Adreno 840
        // (Snapdragon 8 Elite Gen 5, the heaviest scene 22 of 33 ms); older or unknown GPUs start at x1
        // (the menu offers x2 to x4)
        int adreno = 0;
        if (const char* a = strstr(props.deviceName, "Adreno")) {
            while (*a && (*a < '0' || *a > '9')) a++;
            adreno = atoi(a);
        }
        scale = adreno >= 830 ? 2 : 1;
        log_write(LOG_GS, "gpu: %s, automatic internal resolution x%d", props.deviceName, scale);
    }
    S = std::max(1, std::min(4, scale));
    if (getenv("GRANDIA_VK_NO_MIPS")) { g_cfg_mips = false; log_write(LOG_GS, "gpu test: no mip levels"); }
    {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * NF;
        vkCreateQueryPool(dev, &qi, nullptr, &g_qpool);
        g_ts_period = props.limits.timestampPeriod;
        if (g_gpu_profile) {
            qi.queryCount = PQ * NF;
            vkCreateQueryPool(dev, &qi, nullptr, &g_ppool);
            log_write(LOG_GS, "gpu profile: timestamps around passes and copies (--gpu-profile)");
        }
    }
    if (!g_no_uav_overlap) log_write(LOG_GS, "gpu test: a render pass per batch (--uav-overlap)");
    if (getenv("GRANDIA_VK_NO_DRAW_BARRIER")) g_no_batch_join_barrier = true;

    VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sm.magFilter = sm.minFilter = VK_FILTER_NEAREST;
    sm.addressModeU = sm.addressModeV = sm.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev, &sm, nullptr, &g_nearest);
    sm.magFilter = sm.minFilter = VK_FILTER_LINEAR;
    sm.addressModeU = sm.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(dev, &sm, nullptr, &g_linear);

    const VkShaderStageFlags FS = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (int z = 0; z < 2; z++) {
        std::vector<VkDescriptorSetLayoutBinding> b = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | FS, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FS, &g_nearest},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, FS, nullptr},
        };
        for (u32 i = 3; i <= 8; i++) b.push_back({i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FS, &g_nearest});
        b.push_back({9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FS, &g_linear});
        b.push_back({10, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, FS, nullptr});
        if (z) b.push_back({11, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, FS, nullptr});
        g_dsl_draw[z] = make_dsl(b);
        g_pl_draw[z] = make_pl(g_dsl_draw[z], 0, 0);
        g_rp_draw[z] = make_draw_rp(z);
    }
    g_dsl_util = make_dsl({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_nearest},
                           {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}});
    g_pl_util = make_pl(g_dsl_util, VK_SHADER_STAGE_COMPUTE_BIT, 32);
    g_dsl_cell = make_dsl({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g_nearest},
                           {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                           {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}});
    g_pl_cell = make_pl(g_dsl_cell, VK_SHADER_STAGE_COMPUTE_BIT, 16);
    g_dsl_merge = make_dsl({{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FS, &g_nearest},
                            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, FS, &g_nearest}});
    g_pl_merge = make_pl(g_dsl_merge, FS, 64);
    {
        VkAttachmentDescription a{};
        a.format = VK_FORMAT_R8G8B8A8_UNORM;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &a;
        rp.subpassCount = 1;
        rp.pSubpasses = &sp;
        vkCreateRenderPass(dev, &rp, nullptr, &g_rp_out);
    }
    pcache_load();
    static bool texrep_init = false;
    if (!texrep_init) {
        texrep_init = true;
        gs::texrep::init(false);
        g_texrep_on = gs::texrep::have_pack();
    }
    g_pipe_draw[0] = make_draw_pipe(false);
    g_pipe_draw[1] = make_draw_pipe(true);
    g_pipe_up = make_compute(SPV_UPSCALE_COMP, sizeof(SPV_UPSCALE_COMP));
    g_pipe_down = make_compute(SPV_DOWNSAMPLE_COMP, sizeof(SPV_DOWNSAMPLE_COMP));
    g_pipe_cell = make_compute(SPV_CELLCOPY_COMP, sizeof(SPV_CELLCOPY_COMP), g_pl_cell);
    g_pipe_merge = make_merge_pipe();
    if (!g_pipe_draw[0] || !g_pipe_draw[1] || !g_pipe_up || !g_pipe_down || !g_pipe_cell || !g_pipe_merge) return false;

    for (FrameRes& f : g_fr) {
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        cp.queueFamilyIndex = vkc::queue_family();
        vkCreateCommandPool(dev, &cp, nullptr, &f.pool);
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(dev, &fc, nullptr, &f.fence);
        if (!make_buf(f.ring, RING_BYTES,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      false))
            return false;
    }
    if (!make_buf(g_rb, (size_t)2048 * 2048 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    if (!make_img(g_native, 2048, 2048, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    make_img(g_dummy_u, 1, 1, VK_FORMAT_R32_UINT, TEX_USAGE);
    make_img(g_dummy_rgba, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, TEX_USAGE);
    begin_cmds();
    pre_layouts();
    clear_img(g_dummy_u);
    clear_img(g_dummy_rgba);
    full_barrier(g_main);

    shadow = new u32[1024 * 1024]();
    for (int i = 0; i < 16384; i++) { g_owner[i] = -1; g_shadow_ok[i] = 1; }
    // the GPU renderer has its own HUD correction (hud_transform below)
    g_sw_prim_hook = nullptr;
    g_sw_present_hook = nullptr;
    g_enabled = true;
    log_write(LOG_GS, "gpu: Vulkan renderer active, internal resolution x%d", S);
    return true;
}

bool enabled() { return g_enabled; }
int scale() { return S; }

// ===========================================================================
// Render pass state
// ===========================================================================
static bool g_in_pass = false;
static VkPipeline g_bound_pipe = VK_NULL_HANDLE;
static VkImage g_pass_a = VK_NULL_HANDLE, g_pass_b = VK_NULL_HANDLE;
static int g_pass_z = 0;
static VkRect2D g_pass_sc{};

static void end_pass() {
    if (!g_in_pass) return;
    vkCmdEndRenderPass(g_main);
    prof_ts(g_main, 'E');
    full_barrier(g_main);
    g_in_pass = false;
}

static VkFramebuffer get_fb(Surface* fs, Surface* zs) {
    VkImage b = zs ? zs->im.img : VK_NULL_HANDLE;
    for (const FbEntry& e : g_fbs)
        if (e.a == fs->im.img && e.b == b) return e.fb;
    VkImageView v[2] = {fs->im.view, zs ? zs->im.view : VK_NULL_HANDLE};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = g_rp_draw[zs ? 1 : 0];
    fi.attachmentCount = zs ? 2 : 1;
    fi.pAttachments = v;
    fi.width = fs->w * S;
    fi.height = fs->h * S;
    fi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    vkCreateFramebuffer(dev, &fi, nullptr, &fb);
    if (g_fbs.size() > 64) {
        fr().garbage_fb.push_back(g_fbs.front().fb);
        g_fbs.erase(g_fbs.begin());
    }
    g_fbs.push_back({fs->im.img, b, fb});
    return fb;
}

static void begin_pass(Surface* fs, Surface* zs) {
    VkImage b = zs ? zs->im.img : VK_NULL_HANDLE;
    if (g_in_pass && g_pass_a == fs->im.img && g_pass_b == b) return;
    end_pass();
    pre_layouts();
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g_rp_draw[zs ? 1 : 0];
    rb.framebuffer = get_fb(fs, zs);
    rb.renderArea = {{0, 0}, {(uint32_t)(fs->w * S), (uint32_t)(fs->h * S)}};
    prof_ts(g_main, 'B', fs->w * S, fs->h * S);
    vkCmdBeginRenderPass(g_main, &rb, VK_SUBPASS_CONTENTS_INLINE);
    g_bound_pipe = VK_NULL_HANDLE;  // (flush binds the batch's specialized pipeline)
    VkViewport vp{0, 0, (float)(fs->w * S), (float)(fs->h * S), 0, 1};
    vkCmdSetViewport(g_main, 0, 1, &vp);
    g_pass_sc = VkRect2D{{-1, -1}, {0, 0}};
    g_in_pass = true;
    g_pass_a = fs->im.img;
    g_pass_b = b;
    g_pass_z = zs ? 1 : 0;
    st_passes++;
}

// ===========================================================================
// Refresh / readback
// ===========================================================================
static void util_dispatch(VkPipeline p, VkImageView src, VkImageView dst, int x0, int y0, int x1, int y1) {
    if (x1 <= x0 || y1 <= y0) return;
    VkDescriptorImageInfo si{VK_NULL_HANDLE, src, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo di{VK_NULL_HANDLE, dst, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &si, nullptr, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &di, nullptr, nullptr};
    vkCmdBindPipeline(g_main, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    pushDS(g_main, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_util, 0, 2, w);
    int pc[8] = {x0, y0, x1, y1, S, 0, 0, 0};
    vkCmdPushConstants(g_main, g_pl_util, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(g_main, (uint32_t)(x1 - x0 + 7) / 8, (uint32_t)(y1 - y0 + 7) / 8, 1);
}

// Copy the given cells of surface o into the shadow copy (native resolution, top-left samples).
static void readback_cells(Surface* o, const std::vector<int>& cells) {
    if (cells.empty()) return;
    end_pass();
    int x0 = 1 << 30, y0 = 1 << 30, x1 = 0, y1 = 0;
    for (int ci : cells) {
        int cx = ci % o->cw, cy = ci / o->cw;
        x0 = std::min(x0, cx * o->cbw); y0 = std::min(y0, cy * o->cbh);
        x1 = std::max(x1, (cx + 1) * o->cbw); y1 = std::max(y1, (cy + 1) * o->cbh);
    }
    pre_layouts();
    util_dispatch(g_pipe_down, o->im.view, g_native.view, x0, y0, x1, y1);
    full_barrier(g_main);
    int rw = x1 - x0;
    VkBufferImageCopy c{};
    c.bufferOffset = 0;
    c.bufferRowLength = (uint32_t)rw;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {x0, y0, 0};
    c.imageExtent = {(uint32_t)rw, (uint32_t)(y1 - y0), 1};
    vkCmdCopyImageToBuffer(g_main, g_native.img, VK_IMAGE_LAYOUT_GENERAL, g_rb.buf, 1, &c);
    submit_wait();
    invalidate(g_rb);
    const u32* base = (const u32*)g_rb.map;
    for (int ci : cells) {
        int cx = ci % o->cw, cy = ci / o->cw;
        for (int y = cy * o->cbh; y < (cy + 1) * o->cbh; y++) {
            const u32* row = base + (size_t)(y - y0) * rw - x0;
            for (int x = cx * o->cbw; x < (cx + 1) * o->cbw; x++) shadow_write_raw(o->lay, x, y, o->bp, o->bw, row[x]);
        }
        set_shadow_ok(o->cell_block[ci], true);
    }
    st_readbacks++;
    st_readback_cells += cells.size();
}

static const char* g_why = "";
static std::vector<u32> same_family(const std::vector<u32>& blocks, u32 psm);
static u32 g_display_fbp[2] = {~0u, ~0u};
static void make_shadow_current(const std::vector<u32>& blocks) {
    std::unordered_map<int, std::vector<int>> by_owner;
    for (u32 b : blocks) {
        if (g_shadow_ok[b]) continue;
        Surface* o = surf_by_id(g_owner[b]);
        if (!o || o->block_cell[b] < 0) { set_shadow_ok(b, true); continue; }
        by_owner[o->id].push_back(o->block_cell[b]);
    }
    for (auto& [id, cells] : by_owner) {
        static int nlog = 0;
        if (nlog++ < 60)
            log_write(LOG_GS, "gpu readback (%s): surface %d %05x/%u lay %d, %zu cells, first block %x", g_why, id, g_surf[id]->bp,
                      g_surf[id]->bw, (int)g_surf[id]->lay, cells.size(), g_surf[id]->cell_block[cells[0]]);
        std::sort(cells.begin(), cells.end());
        cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
        readback_cells(g_surf[id], cells);
    }
}

static u64 st_gpu_copy_cells = 0, st_skip_cells = 0;
struct PixRect { int x0, y0, x1, y1; };
static void refresh(Surface* s, int x0, int y0, int x1, int y1, const std::vector<PixRect>* skip = nullptr) {
    if (!s || s->ndirty == 0) return;
    int cx0 = std::max(0, x0 / s->cbw), cx1 = std::min(s->cw, (x1 + s->cbw - 1) / s->cbw);
    int cy0 = std::max(0, y0 / s->cbh), cy1 = std::min(s->ch, (y1 + s->cbh - 1) / s->cbh);
    if (cx1 <= cx0 || cy1 <= cy0) return;
    if (skip && !skip->empty()) {
        for (int cy = cy0; cy < cy1; cy++)
            for (int cx = cx0; cx < cx1; cx++) {
                int ci = cy * s->cw + cx;
                if (!s->dirty[ci]) continue;
                int px = cx * s->cbw, py = cy * s->cbh;
                for (const PixRect& r : *skip)
                    if (px >= r.x0 && py >= r.y0 && px + s->cbw <= r.x1 && py + s->cbh <= r.y1) {
                        s->dirty[ci] = 0;
                        s->ndirty--;
                        st_skip_cells++;
                        break;
                    }
            }
    }
    // cells whose newest data is in another surface of the same pixel size: GPU copies, all cells of
    // one source surface in one compute dispatch
    std::vector<u32> need;
    static std::vector<std::pair<int, std::array<int, 4>>> cells;  // source surface id, (sx, sy, dx, dy) native
    cells.clear();
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++) {
            int ci = cy * s->cw + cx;
            if (!s->dirty[ci]) continue;
            u32 b = s->cell_block[ci];
            if (g_shadow_ok[b]) continue;
            Surface* o = surf_by_id(g_owner[b]);
            int oc = o ? o->block_cell[b] : -1;
            if (!o || o == s || oc < 0 || is16(o->lay) != is16(s->lay)) {
                need.push_back(b);
                continue;
            }
            cells.push_back({o->id, {(oc % o->cw) * o->cbw, (oc / o->cw) * o->cbh, cx * s->cbw, cy * s->cbh}});
            s->dirty[ci] = 0;
            s->ndirty--;
            st_gpu_copy_cells++;
        }
    if (!cells.empty()) {
        end_pass();
        pre_layouts();
        std::stable_sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        ring_reserve(cells.size() * 16 + 16 * g_align);
        for (size_t i = 0; i < cells.size();) {
            size_t j = i;
            while (j < cells.size() && cells[j].first == cells[i].first) j++;
            Surface* o = g_surf[cells[i].first];
            u8* p;
            VkDeviceSize off = ring_alloc((j - i) * 16, &p);
            if (!p) break;
            vkCmdBindPipeline(g_main, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe_cell);
            int* q = (int*)p;
            for (size_t k = i; k < j; k++, q += 4)
                for (int m = 0; m < 4; m++) q[m] = cells[k].second[m] * S;
            VkDescriptorImageInfo si{VK_NULL_HANDLE, o->im.view, VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorImageInfo di{VK_NULL_HANDLE, s->im.view, VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorBufferInfo bi{fr().ring.buf, off, (j - i) * 16};
            VkWriteDescriptorSet w[3]{};
            w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &si, nullptr, nullptr};
            w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &di, nullptr, nullptr};
            w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi, nullptr};
            pushDS(g_main, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_cell, 0, 3, w);
            int pc[4] = {s->cbw * S, s->cbh * S, (int)(j - i), 0};
            vkCmdPushConstants(g_main, g_pl_cell, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
            vkCmdDispatch(g_main, (uint32_t)(s->cbw * S + 7) / 8, (uint32_t)(s->cbh * S + 7) / 8, (uint32_t)(j - i));
            i = j;
        }
        full_barrier(g_main);
        prof_ts(g_main, 'c');
    }
    if (!need.empty()) {
        static char why[96];
        snprintf(why, sizeof(why), "refresh %05x/%u lay %d", s->bp, s->bw, (int)s->lay);
        g_why = why;
        make_shadow_current(need);
    }
    // upload the stale cells (runs of consecutive cells per row) into the native scratch, then upscale
    struct Run { int px0, py0, px1, py1; VkDeviceSize off; };
    std::vector<Run> runs;
    size_t total = 0;
    for (int cy = cy0; cy < cy1; cy++) {
        int cx = cx0;
        while (cx < cx1) {
            if (!s->dirty[cy * s->cw + cx]) { cx++; continue; }
            int run0 = cx;
            while (cx < cx1 && s->dirty[cy * s->cw + cx]) cx++;
            int px0 = run0 * s->cbw, px1 = cx * s->cbw, py0 = cy * s->cbh, py1 = py0 + s->cbh;
            runs.push_back({px0, py0, px1, py1, 0});
            total += (size_t)(px1 - px0) * s->cbh * 4 + g_align;
            for (int c = run0; c < cx; c++) {
                s->dirty[cy * s->cw + c] = 0;
                s->ndirty--;
                st_refresh_cells++;
            }
        }
    }
    if (runs.empty()) return;
    ring_reserve(total);
    for (Run& r : runs) {
        int rw = r.px1 - r.px0;
        u8* p;
        r.off = ring_alloc((size_t)rw * (r.py1 - r.py0) * 4, &p);
        if (!p) return;
        u32* buf = (u32*)p;
        for (int y = r.py0; y < r.py1; y++)
            for (int x = r.px0; x < r.px1; x++) buf[(size_t)(y - r.py0) * rw + (x - r.px0)] = shadow_read_raw(s->lay, x, y, s->bp, s->bw);
    }
    end_pass();
    pre_layouts();
    std::vector<VkBufferImageCopy> cp(runs.size());
    for (size_t i = 0; i < runs.size(); i++) {
        const Run& r = runs[i];
        cp[i] = VkBufferImageCopy{};
        cp[i].bufferOffset = r.off;
        cp[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp[i].imageOffset = {r.px0, r.py0, 0};
        cp[i].imageExtent = {(uint32_t)(r.px1 - r.px0), (uint32_t)(r.py1 - r.py0), 1};
    }
    vkCmdCopyBufferToImage(g_main, fr().ring.buf, g_native.img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)cp.size(), cp.data());
    full_barrier(g_main);
    for (const Run& r : runs) util_dispatch(g_pipe_up, g_native.view, s->im.view, r.px0 * S, r.py0 * S, r.px1 * S, r.py1 * S);
    full_barrier(g_main);
    prof_ts(g_main, 'u');
}

static void mark_written(Surface* s, const std::vector<u8>& written, int cx0, int cy0, int cx1, int cy1) {
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++) {
            int ci = cy * s->cw + cx;
            if (!written[ci]) continue;
            u32 b = s->cell_block[ci];
            stamp_block(b);
            if (s->dirty[ci]) { s->dirty[ci] = 0; s->ndirty--; }
            if (g_owner[b] == s->id && !g_shadow_ok[b]) continue;
            g_owner[b] = (s16)s->id;
            set_shadow_ok(b, false);
            for (Surface* t : g_surf) {
                if (!t || t == s) continue;
                int tc = t->block_cell[b];
                if (tc >= 0 && !t->dirty[tc]) { t->dirty[tc] = 1; t->ndirty++; }
            }
        }
}

static void mark_cpu_written(u32 b) {
    stamp_block(b);
    g_owner[b] = -1;
    set_shadow_ok(b, true);
    for (Surface* t : g_surf) {
        if (!t) continue;
        int tc = t->block_cell[b];
        if (tc >= 0 && !t->dirty[tc]) { t->dirty[tc] = 1; t->ndirty++; }
    }
}

static std::vector<u32> same_family(const std::vector<u32>& blocks, u32 psm) {
    std::vector<u32> r;
    int fam = psm_family(psm);
    for (u32 b : blocks) {
        if (g_shadow_ok[b]) continue;
        Surface* o = surf_by_id(g_owner[b]);
        if (o && (is16(o->lay) ? 16 : 32) == fam) r.push_back(b);
    }
    return r;
}

static Surface* find_surface(u32 bp, u32 bw, Layout lay) {
    bw = std::max<u32>(bw, 1);
    for (Surface* s : g_surf)
        if (s && s->bp == bp && s->bw == bw && s->lay == lay) return s;
    return nullptr;
}
static Surface* get_surface(u32 bp, u32 bw, Layout lay, int h) {
    Surface* s = find_surface(bp, bw, lay);
    if (!s) s = create_surface(bp, bw, lay, h);
    else if (h > s->h) grow_surface(s, h);
    if (s) s->last_use = g_frame;
    return s;
}

// ===========================================================================
// Decoded textures (from the shadow copy)
// ===========================================================================
struct TexEntry {
    u32 tbp, tbw, tpsm, tw, th;
    Img im;
    u64 used = 0;              // main buffer serial that last sampled it
    std::vector<u16> blocks;
    std::vector<u16> pages;
    u64 stamp = 0;
    u64 hash = 0;
    bool decoded = false;
    u64 last_use = 0;
    // texture replacement (texrep): hash of the decoded texels, the inputs of the last key, the key
    u64 data_hash = 0, rep_inputs = ~0ull, rep_key = 0;
};
static u64 st_tex_replaced = 0;
static u64 st_tex_rehash = 0, st_up_same = 0;  // (st_up_same: uploaded blocks with unchanged content)

static u64 blocks_hash(const std::vector<u16>& blocks) {
    u64 h = 0x9E3779B97F4A7C15ull;
    for (u16 b : blocks) {
        const u64* p = (const u64*)(shadow + (size_t)b * 64);
        for (int i = 0; i < 32; i++) h = (h ^ p[i]) * 0x100000001B3ull + (h >> 29);
    }
    return h;
}

static void decode_blocks(u32 tpsm, u32 tbp, u32 tbw, u32 tw, u32 th, u8* out, int bpp) {
    int cw, ch;
    psm_block_dims(tpsm, cw, ch);
    const u8* m8 = (const u8*)shadow;
    const u16* m16 = (const u16*)shadow;
    for (u32 by = 0; by < th; by += ch)
        for (u32 bx = 0; bx < tw; bx += cw) {
            u32 blk = psm_block(tpsm, (int)bx, (int)by, tbp, tbw);
            u32 yn = std::min<u32>(ch, th - by), xn = std::min<u32>(cw, tw - bx);
            for (u32 yy = 0; yy < yn; yy++) {
                size_t row = (size_t)(by + yy) * tw + bx;
                switch (tpsm) {
                case PSMT8: {
                    const u8* src = m8 + ((size_t)blk << 8);
                    for (u32 xx = 0; xx < xn; xx++) out[row + xx] = src[columnTable8[yy][xx]];
                    break;
                }
                case PSMT4: {
                    u32 base = blk << 9;
                    for (u32 xx = 0; xx < xn; xx++) {
                        u32 a = base + columnTable4[yy][xx];
                        out[row + xx] = (m8[a >> 1] >> ((a & 1) * 4)) & 0xF;
                    }
                    break;
                }
                case PSMCT32: case PSMCT24: case PSMZ32: case PSMZ24: {
                    const u32* src = shadow + ((size_t)blk << 6);
                    for (u32 xx = 0; xx < xn; xx++) ((u32*)out)[row + xx] = src[columnTable32[yy][xx]];
                    break;
                }
                case PSMT8H: case PSMT4HL: case PSMT4HH: {
                    const u32* src = shadow + ((size_t)blk << 6);
                    int sh = tpsm == PSMT4HH ? 28 : 24;
                    u32 mask = tpsm == PSMT8H ? 0xFF : 0xF;
                    for (u32 xx = 0; xx < xn; xx++) out[row + xx] = (u8)((src[columnTable32[yy][xx]] >> sh) & mask);
                    break;
                }
                default: {
                    const u16* src = m16 + ((size_t)blk << 7);
                    for (u32 xx = 0; xx < xn; xx++) ((u16*)out)[row + xx] = src[columnTable16[yy][xx]];
                    break;
                }
                }
            }
        }
}
static std::unordered_map<u64, TexEntry> g_tex;

static u64 tex_key(u32 tbp, u32 tbw, u32 tpsm, u32 tw, u32 th) {
    return (u64)tbp | ((u64)tbw << 14) | ((u64)tpsm << 20) | ((u64)tw << 26) | ((u64)th << 38);
}

static TexEntry* decoded_texture(u32 tbp, u32 tbw, u32 tpsm, u32 tw, u32 th) {
    u64 key = tex_key(tbp, tbw, tpsm, tw, th);
    TexEntry& e = g_tex[key];
    if (e.blocks.empty()) {
        e.tbp = tbp; e.tbw = tbw; e.tpsm = tpsm; e.tw = tw; e.th = th;
        for_rect_blocks(tpsm, tbp, tbw, 0, 0, (int)tw, (int)th, [&](u32 b, bool) { e.blocks.push_back((u16)b); });
        std::sort(e.blocks.begin(), e.blocks.end());
        e.blocks.erase(std::unique(e.blocks.begin(), e.blocks.end()), e.blocks.end());
        for (u16 b : e.blocks)
            if (e.pages.empty() || e.pages.back() != (b >> 5)) e.pages.push_back(b >> 5);
    }
    e.last_use = g_frame;
    if (e.decoded) {
        bool ok = true;
        for (u16 p : e.pages)
            if (g_page_stamp[p] > e.stamp) { ok = false; break; }
        if (!ok) {
            ok = true;
            for (u16 b : e.blocks)
                if (g_stamp[b] > e.stamp) { ok = false; break; }
        }
        bool all_ok = true;
        if (!ok)
            for (u16 p : e.pages)
                if (g_page_stale[p]) { all_ok = false; break; }
        if (!ok && all_ok && blocks_hash(e.blocks) == e.hash) {
            ok = true;
            st_tex_rehash++;
        }
        if (ok) {
            e.stamp = g_stamp_next - 1;
            st_tex_hits++;
            return &e;
        }
    }
    for (u16 b : e.blocks)
        if (!g_shadow_ok[b]) st_stale_texels++;
    VkFormat f;
    int bpp;
    switch (tpsm) {
    case PSMT8: case PSMT4: case PSMT8H: case PSMT4HL: case PSMT4HH: f = VK_FORMAT_R8_UINT; bpp = 1; break;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: f = VK_FORMAT_R16_UINT; bpp = 2; break;
    default: f = VK_FORMAT_R32_UINT; bpp = 4; break;
    }
    // the open main buffer samples the old content: a new image (the upload runs before that buffer)
    if (e.im.img && e.used == g_cmd_serial) {
        garbage(e.im);
        st_tex_versions++;
    }
    if (!e.im.img && !make_img(e.im, (int)tw, (int)th, f, TEX_USAGE)) return &e;
    u8* p;
    size_t bytes = (size_t)tw * th * bpp;
    VkDeviceSize off = ring_alloc(bytes, &p);
    if (p) {
        if (g_texrep_on) {
            // decoded in normal memory first (the ring is written once, not read back)
            static std::vector<u8> buf;
            buf.resize(bytes);
            decode_blocks(tpsm, tbp, tbw, tw, th, buf.data(), bpp);
            memcpy(p, buf.data(), bytes);
            e.data_hash = gs::texrep::hash(buf.data(), bytes, tpsm | tw << 8 | (u64)th << 24);
            e.rep_inputs = ~0ull;
        } else {
            decode_blocks(tpsm, tbp, tbw, tw, th, p, bpp);
        }
        g_pre_up.push_back({e.im.img, off, (int)tw, (int)th});
        if (g_pre_up.size() >= 256) pre_flush_uploads();
    }
    e.stamp = g_stamp_next - 1;
    e.hash = blocks_hash(e.blocks);
    e.decoded = true;
    st_tex_decodes++;
    return &e;
}

static void evict_textures() {
    if (g_tex.size() < 768) return;
    std::vector<std::pair<u64, u64>> v;
    for (auto& [k, e] : g_tex) v.push_back({e.last_use, k});
    std::sort(v.begin(), v.end());
    for (size_t i = 0; i < v.size() / 4; i++) {
        TexEntry& e = g_tex[v[i].second];
        garbage(e.im);
        g_tex.erase(v[i].second);
    }
}

// ===========================================================================
// Batches (as gs_gpu.cpp)
// ===========================================================================
struct GV {
    float x, y, z;
    float r, g, b, a;
    float s, t, q;
    float fog;
    float fr, fg, fb, fa;
    u32 st0, st1;
};
static_assert(sizeof(GV) == 68, "vertex layout");
static const size_t VB_BYTES = 16 << 20;

static std::vector<GV> g_verts;
static DrawState g_bds;
static u32 g_bid = ~0u;
static bool g_bvalid = false;
static int g_bx0, g_by0, g_bx1, g_by1;
static float g_bu0, g_bv0, g_bu1, g_bv1;
static bool g_bself = false;
static bool g_bself_tex = false;
static bool g_btme = false;
static bool g_bover = true;
static u32 g_ps0 = 0, g_ps1 = 0;
static bool g_bhud = false;
static bool g_ball_sprites = true;
static std::vector<PixRect> g_bsprites;
static int g_bprims = 0;
static u64 st_why[16];
static DrawState g_prev_raw;
static u32 g_clut[512];
static bool g_clut_valid = false;
static bool g_clut_dirty = true;   // CLUT changed since its last copy into the ring
static u64 g_clut_version = 0;     // counts CLUT changes (replacement keys of palette textures)
static u64 g_clut_gen = 0;         // ring generation of that copy
static VkDeviceSize g_clut_off = 0;

static int tex_format_code(u32 tpsm, bool decoded) {
    switch (tpsm) {
    case PSMCT32: case PSMZ32: return 0;
    case PSMCT24: return 1;
    case PSMZ24: return 8;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 2;
    case PSMT8: return 3;
    case PSMT4: return 4;
    case PSMT8H: return decoded ? 3 : 5;
    case PSMT4HL: return decoded ? 4 : 6;
    case PSMT4HH: return decoded ? 4 : 7;
    }
    return 0;
}

struct DrawCB {
    float vp[4];
    u32 f0[4], f1[4], f2[4], f3[4], f4[4], f5[4], f6[4], f7[4], f8[4], f9[4], f10[4];
    float f11[4];
    u32 f12[4];
    s32 f13[4];
    u32 f14[4];
    float f15[4];
    s32 dimx[16];
};

static void flush();

static void row_blocks(u32 bp, u32 bw, u32 psm, int y0, int y1, u32& lo, u32& hi) {
    int ph = 32, pw = 64;
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: ph = 64; break;
    case PSMT8: case PSMT8H: ph = 64; pw = 128; break;
    case PSMT4: case PSMT4HL: case PSMT4HH: ph = 128; pw = 128; break;
    }
    if (psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH) { ph = 32; pw = 64; }
    u32 ppr = std::max<u32>(1, (bw * 64 + pw - 1) / pw);
    y0 = std::max(y0, 0);
    y1 = std::max(y1, y0 + 1);
    lo = bp + (u32)(y0 / ph) * ppr * 32;
    hi = bp + (u32)((y1 + ph - 1) / ph) * ppr * 32;
}
static bool samples_own_writes(const DrawState& ds) {
    int th = 1 << ds.th, t0 = 0, t1 = th;
    if (ds.wmt == 2) { t0 = std::min<int>(ds.minv, th - 1); t1 = std::min<int>(ds.maxv + 1, th); }
    u32 tl, thi;
    row_blocks(ds.tbp, ds.tbw, ds.tpsm, t0, std::max(t1, t0 + 1), tl, thi);
    auto hit = [&](u32 bp, u32 psm) {
        u32 l, h;
        row_blocks(bp, ds.fbw, psm, ds.scay0, ds.scay1 + 1, l, h);
        for (int k = -1; k <= 1; k++) {
            s64 o = (s64)k * 16384;
            if ((s64)tl < (s64)h + o && (s64)l + o < (s64)thi) return true;
        }
        return false;
    };
    if (ds.fbmsk != 0xFFFFFFFFu && hit(ds.fbp, ds.fpsm)) return true;
    return ds.zte && !ds.zmsk && ds.ztst != 0 && hit(ds.zbp, ds.zpsm);
}

struct TexBind {
    VkImageView view = VK_NULL_HANDLE;
    float tscale = 1;
    int ox = 0, oy = 0;
    int tfmt = 0;
    Surface* surf = nullptr;
    bool rep = false;  // a replacement image (no mip levels: the game's levels are the original format)
};

// ---------------------------------------------------------------------------
// Texture replacement (as gs_gpu.cpp apply_texrep): a decoded texture whose key has an image in
// lang/<code>_tex or lang/all_tex is sampled from that image instead, a 32-bit texture k times the
// size (the shader's path for scaled surfaces: format 0, texture scale k).
// ---------------------------------------------------------------------------
struct Rep {
    Img im;
    int k = 1;
    bool none = false;
};
static std::unordered_map<u64, Rep> g_rep;

// key of texel data + the palette entries / alpha expansion it is shown with (gs_gpu.cpp rep_key)
static u64 rep_key(const DrawState& ds, const TexEntry* e, int tfmt) {
    bool c16 = ds.cpsm == PSMCT16 || ds.cpsm == PSMCT16S;
    bool pal = tfmt == 3 || tfmt == 4;
    u64 k = e->data_hash;
    if (pal) {
        u32 n = tfmt == 3 ? 256 : 16, ent[256];
        for (u32 i = 0; i < n; i++)
            ent[i] = c16 ? (g_clut[(i + ds.csa * 16) & 511] & 0xFFFF) : (tfmt == 4 ? g_clut[(i + (ds.csa & 15) * 16) & 255] : g_clut[i]);
        k = gs::texrep::hash(ent, n * 4, k ^ (c16 ? 0x16 : 0x32));
    }
    if ((pal && c16) || tfmt == 1 || tfmt == 2 || tfmt == 8) {
        u32 t[3] = {ds.ta0, ds.ta1, (u32)ds.aem};
        k = gs::texrep::hash(t, sizeof t, k);
    }
    return k;
}

static void create_rep(Rep& r, const gs::texrep::Image& img, u32 tw, u32 th, u64 key) {
    int k = img.w / (int)tw;
    if (k < 1 || k > 8 || img.w != k * (int)tw || img.h != k * (int)th) {
        log_write(LOG_GS, "replacement %016llx: %dx%d is no integer multiple of %ux%u, ignored", (unsigned long long)key, img.w, img.h, tw, th);
        r.none = true;
        return;
    }
    // more detail than the internal resolution shows: box filter down to scale S (memory, aliasing)
    int f = (k > S && k % S == 0) ? k / S : 1;
    int w = img.w / f, h = img.h / f;
    u8* p = nullptr;
    VkDeviceSize off = 0;
    if (!make_img(r.im, w, h, VK_FORMAT_R32_UINT, TEX_USAGE) || !(off = ring_alloc((size_t)w * h * 4, &p), p)) {
        garbage(r.im);
        r.none = true;
        return;
    }
    u32* out = (u32*)p;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (f == 1) { out[(size_t)y * w + x] = img.px[(size_t)y * img.w + x]; continue; }
            // straight alpha: colours weighted by alpha
            u32 sr = 0, sg = 0, sb = 0, sa = 0, cr = 0, cg = 0, cb = 0;
            for (int yy = 0; yy < f; yy++)
                for (int xx = 0; xx < f; xx++) {
                    u32 c = img.px[(size_t)(y * f + yy) * img.w + x * f + xx], a = c >> 24;
                    sr += (c & 0xFF) * a; sg += (c >> 8 & 0xFF) * a; sb += (c >> 16 & 0xFF) * a; sa += a;
                    cr += c & 0xFF; cg += c >> 8 & 0xFF; cb += c >> 16 & 0xFF;
                }
            u32 n = (u32)(f * f), a = (sa + n / 2) / n;
            u32 r_ = sa ? (sr + sa / 2) / sa : cr / n, g_ = sa ? (sg + sa / 2) / sa : cg / n, b_ = sa ? (sb + sa / 2) / sa : cb / n;
            out[(size_t)y * w + x] = r_ | g_ << 8 | b_ << 16 | a << 24;
        }
    g_pre_up.push_back({r.im.img, off, w, h});
    if (g_pre_up.size() >= 256) pre_flush_uploads();
    r.k = k / f;
    log_write(LOG_GS, "replacement %016llx: %dx%d for %ux%u (scale %d)", (unsigned long long)key, w, h, tw, th, r.k);
}

static void apply_texrep(const DrawState& ds, TexEntry* e, TexBind& tb) {
    bool pal = tb.tfmt == 3 || tb.tfmt == 4;
    u64 in = e->data_hash * 0x9E3779B97F4A7C15ull ^ (pal ? g_clut_version : 0) * 0xC2B2AE3D27D4EB4Full ^
             ((u64)ds.csa | (u64)ds.cpsm << 8 | (u64)ds.ta0 << 16 | (u64)ds.ta1 << 24 | (u64)ds.aem << 32);
    if (in != e->rep_inputs) {
        e->rep_inputs = in;
        e->rep_key = rep_key(ds, e, tb.tfmt);
    }
    auto it = g_rep.find(e->rep_key);
    if (it == g_rep.end()) {
        const gs::texrep::Image* img = nullptr;
        gs::texrep::State st = gs::texrep::lookup(e->rep_key, &img);
        if (st == gs::texrep::LOADING) return;  // the original until the PNG is decoded
        Rep& r = g_rep[e->rep_key];
        if (st == gs::texrep::NONE) r.none = true;
        else {
            create_rep(r, *img, e->tw, e->th, e->rep_key);
            gs::texrep::release(e->rep_key);
        }
        it = g_rep.find(e->rep_key);
    }
    Rep& r = it->second;
    if (r.none) return;
    tb.view = r.im.view;
    tb.tscale = (float)r.k;
    tb.tfmt = 0;
    tb.rep = true;
    st_tex_replaced++;
}

static TexBind resolve_texture(const DrawState& ds, Surface* fs, Surface* zs) {
    TexBind tb;
    int tw = 1 << ds.tw, th = 1 << ds.th;
    int u0 = 0, u1 = tw, v0 = 0, v1 = th;
    if (ds.wms != 0 && g_bu0 > -1e6f && g_bu1 < 1e6f) { u0 = std::max(0, (int)floorf(g_bu0) - 1); u1 = std::min(tw, (int)ceilf(g_bu1) + 2); }
    if (ds.wmt != 0 && g_bv0 > -1e6f && g_bv1 < 1e6f) { v0 = std::max(0, (int)floorf(g_bv0) - 1); v1 = std::min(th, (int)ceilf(g_bv1) + 2); }
    if (ds.wms == 2) { u0 = std::max(u0, (int)ds.minu); u1 = std::min(u1, (int)ds.maxu + 1); }
    if (ds.wmt == 2) { v0 = std::max(v0, (int)ds.minv); v1 = std::min(v1, (int)ds.maxv + 1); }
    if (u1 <= u0) { u0 = 0; u1 = tw; }
    if (v1 <= v0) { v0 = 0; v1 = th; }
    TexEntry* geo = nullptr;
    {
        u64 key = tex_key(ds.tbp, ds.tbw, ds.tpsm, (u32)tw, (u32)th);
        auto it = g_tex.find(key);
        if (it != g_tex.end() && !it->second.pages.empty()) geo = &it->second;
    }
    bool maybe_gpu = true;
    if (geo) {
        maybe_gpu = false;
        for (u16 p : geo->pages)
            if (g_page_stale[p]) { maybe_gpu = true; break; }
    }
    std::vector<u32> blocks;
    if (maybe_gpu) for_rect_blocks(ds.tpsm, ds.tbp, ds.tbw, u0, v0, u1, v1, [&](u32 b, bool) { blocks.push_back(b); });
    Layout tl = layout_of(ds.tpsm);
    bool gpu_owned = false;
    for (u32 b : blocks)
        if (!g_shadow_ok[b]) { gpu_owned = true; break; }
    if (tl != L_NONE && gpu_owned) {
        Surface* best = nullptr;
        int ox = 0, oy = 0;
        for (Surface* s : g_surf) {
            if (!s || s->lay != tl || s->bw != std::max<u32>(ds.tbw, 1)) continue;
            if (ds.tbp < s->bp) continue;
            u32 d = ds.tbp - s->bp;
            if (d % 32) continue;
            u32 page = d / 32;
            int ph = is16(tl) ? 64 : 32;
            int px = (int)(page % s->bw) * 64, py = (int)(page / s->bw) * ph;
            if (py >= s->h) continue;
            if (!best || g_owner[blocks[0]] == s->id) { best = s; ox = px; oy = py; }
        }
        if (best) {
            refresh(best, ox + u0, oy + v0, ox + u1, oy + v1);
            tb.view = best->im.view;
            tb.tscale = (float)S;
            tb.ox = ox;
            tb.oy = oy;
            tb.tfmt = tex_format_code(ds.tpsm, false);
            tb.surf = best;
            return tb;
        }
    }
    if (gpu_owned) { g_why = "texture decode"; make_shadow_current(blocks); }
    TexEntry* e = decoded_texture(ds.tbp, ds.tbw, ds.tpsm, (u32)tw, (u32)th);
    e->used = g_cmd_serial;
    tb.view = e->im.view;
    tb.tscale = 1;
    tb.tfmt = tex_format_code(ds.tpsm, true);
    if (g_texrep_on && !gpu_owned && e->decoded && e->im.view) apply_texrep(ds, e, tb);
    return tb;
}

static void bind_snapshot(Surface* s, TexBind& tb) {
    if (!g_snap.img || g_snap.w < s->w * S || g_snap.h < s->h * S) {
        int w = std::max(g_snap.w, s->w * S), h = std::max(g_snap.h, s->h * S);
        garbage(g_snap);
        make_img(g_snap, w, h, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    end_pass();
    pre_layouts();
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {(uint32_t)(s->w * S), (uint32_t)(s->h * S), 1};
    vkCmdCopyImage(g_main, s->im.img, VK_IMAGE_LAYOUT_GENERAL, g_snap.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(g_main);
    prof_ts(g_main, 's');
    tb.view = g_snap.view;
    st_self++;
}

static VkDeviceSize clut_offset() {
    if (g_clut_dirty || g_clut_gen != g_ring_gen) {
        u8* p;
        VkDeviceSize off = ring_alloc(sizeof(g_clut), &p);
        if (p) memcpy(p, g_clut, sizeof(g_clut));
        g_clut_off = off;
        g_clut_gen = g_ring_gen;
        g_clut_dirty = false;
    }
    return g_clut_off;
}

static void flush() {
    if (!g_bvalid || g_verts.empty()) {
        g_verts.clear();
        g_bprims = 0;
        return;
    }
    st_why[15]++;
    const DrawState& ds = g_bds;
    Layout fl = layout_of(ds.fpsm);
    if (fl == L_NONE) { g_verts.clear(); g_bprims = 0; return; }
    int need_h = std::min(2048, std::max(ds.scay1 + 1, g_by1));
    Surface* fs = get_surface(ds.fbp, ds.fbw, fl, need_h);
    if (!fs) { g_verts.clear(); g_bprims = 0; return; }
    Surface* zs = nullptr;
    bool zuse = ds.zte && (ds.ztst != 1 || !ds.zmsk);
    if (zuse) {
        zs = get_surface(ds.zbp, ds.fbw, layout_of(ds.zpsm), fs->h);
        if (zs && zs->h < fs->h) grow_surface(zs, fs->h);
    }
    if (zs && (zs->h < fs->h || zs == fs)) zs = nullptr;
    TexBind tb;
    if (g_btme) tb = resolve_texture(ds, fs, zs);
    bool color_overwrites = g_ball_sprites && ds.fbmsk == 0 && ds.fpsm != PSMCT24 && g_bover;
    bool z_overwrites = g_ball_sprites && ds.zte && ds.ztst == 1 && !ds.zmsk;
    refresh(fs, g_bx0, g_by0, g_bx1, g_by1, color_overwrites ? &g_bsprites : nullptr);
    if (zs) refresh(zs, g_bx0, g_by0, g_bx1, g_by1, z_overwrites ? &g_bsprites : nullptr);
    if (g_btme && tb.surf && (tb.surf == fs || tb.surf == zs)) bind_snapshot(tb.surf, tb);
    int nmips = 0;
    VkImageView mip_view[6] = {};
    if (g_btme && ds.mxl > 0 && ds.mmin >= 2 && ds.mmin <= 5 && !ds.mtba && !tb.surf && !tb.rep && g_cfg_mips) {
        nmips = std::min<int>(ds.mxl, 6);
        for (int l = 1; l <= nmips; l++) {
            u64 m = l <= 3 ? ds.miptbp1 : ds.miptbp2;
            int k = (l - 1) % 3;
            u32 tbp = bits(m, k * 20, 14), tbw = bits(m, k * 20 + 14, 6);
            u32 w = std::max(1u, (1u << ds.tw) >> l), h = std::max(1u, (1u << ds.th) >> l);
            TexEntry* e = decoded_texture(tbp, tbw, ds.tpsm, w, h);
            e->used = g_cmd_serial;
            mip_view[l - 1] = e->im.view;
        }
    }

    DrawCB c{};
    if (g_btme && S > 1 && tb.tfmt == 4 && !tb.surf && platform::config().sharp_text && ds.cpsm != PSMCT16 && ds.cpsm != PSMCT16S) {
        const u32* pal = &g_clut[(ds.csa & 15) * 16];
        u32 amin = 255, amax = 0;
        bool one_color = true;
        for (int i = 0; i < 16; i++) {
            if ((pal[i] & 0xFFFFFF) != (pal[0] & 0xFFFFFF)) one_color = false;
            amin = std::min(amin, pal[i] >> 24);
            amax = std::max(amax, pal[i] >> 24);
        }
        if (one_color && amin == 0 && amax >= 0x40) { c.f15[2] = 1.0f; c.f15[3] = (float)amax; }
    }
    c.vp[0] = (float)fs->w; c.vp[1] = (float)fs->h; c.vp[2] = (float)S;
    u32 fpk = (ds.fpsm == PSMCT24 || ds.fpsm == PSMZ24) ? 1 : (is16(fl) ? 2 : 0);
    c.f0[0] = fpk; c.f0[1] = ds.fbmsk;
    c.f1[0] = ds.dthe;
    c.f2[3] = zs ? 1 : 0;
    u32 zf = ds.zpsm == PSMZ32 ? 0 : ds.zpsm == PSMZ24 ? 1 : 2;
    c.f3[0] = ds.ztst; c.f3[1] = ds.zmsk; c.f3[2] = zf;
    c.f6[1] = ds.tfx; c.f6[2] = ds.tcc; c.f6[3] = (u32)tb.tfmt;
    c.f7[0] = 1u << ds.tw; c.f7[1] = 1u << ds.th; c.f7[2] = ds.wms; c.f7[3] = ds.wmt;
    c.f8[0] = ds.minu; c.f8[1] = ds.maxu; c.f8[2] = ds.minv; c.f8[3] = ds.maxv;
    c.f9[0] = (ds.cpsm == PSMCT16 || ds.cpsm == PSMCT16S) ? 1 : 0; c.f9[1] = ds.csa; c.f9[2] = ds.ta0; c.f9[3] = ds.ta1;
    bool need_lod = g_btme && !ds.lcm && ds.mag_linear != ds.min_linear;
    c.f10[0] = ds.aem; c.f10[1] = ds.mag_linear; c.f10[2] = ds.min_linear; c.f10[3] = need_lod;
    float lodk = ds.lodk - (platform::config().lod_scale ? log2f((float)S) : 0.0f);
    c.f11[0] = lodk; c.f11[1] = need_lod ? exp2f(lodk / (float)(1 << ds.lodl)) : 0.0f; c.f11[2] = tb.tscale;
    c.f13[0] = tb.ox; c.f13[1] = tb.oy;
    c.f14[0] = (u32)nmips; c.f14[1] = ds.mmin; c.f14[2] = ds.lcm; c.f14[3] = ds.lodl;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) c.dimx[y * 4 + x] = ds.dimx[y][x];
    // specialization (draw.frag SC_*): fields that do not matter are 0, so equal batches share a pipeline
    u32 sp[NSPEC] = {};
    if (g_btme) {
        sp[0] = 1; sp[1] = c.f6[3]; sp[2] = c.f6[1]; sp[3] = c.f6[2]; sp[4] = c.f7[2]; sp[5] = c.f7[3];
        sp[6] = c.f11[2] == 1.0f; sp[7] = c.f10[1]; sp[8] = c.f10[2]; sp[9] = c.f10[3];
        sp[10] = nmips > 0;
        if (nmips > 0) { sp[11] = c.f14[1]; sp[12] = c.f14[2]; }
        sp[13] = c.f15[2] != 0.0f;
        sp[14] = c.f9[0]; sp[15] = c.f10[0];
    } else {
        sp[6] = 1;
    }
    sp[16] = c.f0[0]; sp[17] = c.f0[1] == 0; sp[18] = c.f1[0] != 0;
    if (zs) { sp[19] = 1; sp[20] = c.f3[0]; sp[21] = c.f3[1] != 0; sp[22] = c.f3[2]; }

    // ring memory first (a full ring submits, which ends the render pass)
    size_t bytes = g_verts.size() * sizeof(GV);
    if (bytes > VB_BYTES) bytes = VB_BYTES / sizeof(GV) / 3 * 3 * sizeof(GV);
    ring_reserve(sizeof(g_clut) + sizeof(DrawCB) + bytes + 3 * g_align);
    VkDeviceSize clut_off = clut_offset();
    u8* pc;
    VkDeviceSize cb_off = ring_alloc(sizeof(DrawCB), &pc);
    u8* pv;
    VkDeviceSize vb_off = ring_alloc(bytes, &pv);
    if (!pc || !pv) { g_verts.clear(); g_bprims = 0; return; }
    memcpy(pc, &c, sizeof(c));
    memcpy(pv, g_verts.data(), bytes);

    begin_pass(fs, zs);
    VkPipeline pipe = draw_pipe_for(g_pass_z != 0, sp);
    if (pipe != g_bound_pipe) {
        vkCmdBindPipeline(g_main, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        g_bound_pipe = pipe;
    }
    VkBuffer rb = fr().ring.buf;
    VkDescriptorBufferInfo ubo{rb, cb_off, sizeof(DrawCB)};
    VkDescriptorBufferInfo clut{rb, clut_off, sizeof(g_clut)};
    VkDescriptorImageInfo ti{VK_NULL_HANDLE, tb.view ? tb.view : g_dummy_u.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo mi[6];
    for (int l = 0; l < 6; l++) mi[l] = {VK_NULL_HANDLE, l < nmips && mip_view[l] ? mip_view[l] : g_dummy_u.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo hd{VK_NULL_HANDLE, g_dummy_rgba.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo fin{VK_NULL_HANDLE, fs->im.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo zin{VK_NULL_HANDLE, zs ? zs->im.view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet all[12];
    int na = 0;
    auto wr = [&](u32 binding, VkDescriptorType t, const VkDescriptorImageInfo* ii, const VkDescriptorBufferInfo* bi) {
        all[na++] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, binding, 0, 1, t, ii, bi, nullptr};
    };
    wr(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo);
    wr(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ti, nullptr);
    wr(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &clut);
    for (int l = 0; l < 6; l++) wr((u32)(3 + l), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mi[l], nullptr);
    wr(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hd, nullptr);
    wr(10, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &fin, nullptr);
    if (zs) wr(11, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &zin, nullptr);
    pushDS(g_main, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl_draw[g_pass_z], 0, (uint32_t)na, all);
    int fw = fs->w * S, fh = fs->h * S;
    int sx0 = std::clamp(ds.scax0 * S, 0, fw), sy0 = std::clamp(ds.scay0 * S, 0, fh);
    int sx1 = std::clamp((ds.scax1 + 1) * S, 0, fw), sy1 = std::clamp((ds.scay1 + 1) * S, 0, fh);
    VkRect2D sc{{sx0, sy0}, {(uint32_t)std::max(0, sx1 - sx0), (uint32_t)std::max(0, sy1 - sy0)}};
    if (memcmp(&sc, &g_pass_sc, sizeof sc) != 0) {
        vkCmdSetScissor(g_main, 0, 1, &sc);
        g_pass_sc = sc;
    }
    vkCmdBindVertexBuffers(g_main, 0, 1, &rb, &vb_off);
    if (!platform::skip_active() && sc.extent.width && sc.extent.height) {
        vkCmdDraw(g_main, (uint32_t)(bytes / sizeof(GV)), 1, 0, 0);
        prof_draw();
    }
    st_batches++;
    if (!g_no_uav_overlap) end_pass();  // test: --uav-overlap = a render pass per batch
    else if (!g_no_batch_join_barrier) {
        // the next draw of this pass reads what this one wrote (tile-local barrier)
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
        vkCmdPipelineBarrier(g_main, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, nullptr, 0, nullptr);
    }

    bool color_write = ds.fbmsk != 0xFFFFFFFFu;
    auto mark = [&](Surface* s) {
        int cx0 = std::max(0, g_bx0 / s->cbw), cx1 = std::min(s->cw, (g_bx1 + s->cbw - 1) / s->cbw);
        int cy0 = std::max(0, g_by0 / s->cbh), cy1 = std::min(s->ch, (g_by1 + s->cbh - 1) / s->cbh);
        if (cx1 <= cx0 || cy1 <= cy0) return;
        static std::vector<u8> wm;
        wm.assign(s->cell_block.size(), 0);
        for (int cy = cy0; cy < cy1; cy++)
            for (int cx = cx0; cx < cx1; cx++) wm[cy * s->cw + cx] = 1;
        mark_written(s, wm, cx0, cy0, cx1, cy1);
    };
    if (color_write) mark(fs);
    if (zs && ds.zte && !ds.zmsk && ds.ztst != 0) mark(zs);
    g_verts.clear();
    g_bprims = 0;
}

static inline void bounds_add(const DrawState& ds, int x0, int y0, int x1, int y1) {
    x0 = std::max(x0, ds.scax0); y0 = std::max(y0, ds.scay0);
    x1 = std::min(x1, ds.scax1 + 1); y1 = std::min(y1, ds.scay1 + 1);
    if (x1 <= x0 || y1 <= y0) return;
    g_bx0 = std::min(g_bx0, x0); g_by0 = std::min(g_by0, y0);
    g_bx1 = std::max(g_bx1, x1); g_by1 = std::max(g_by1, y1);
}

static inline void uv_add(const DrawState& ds, const Vertex& v) {
    float u, t;
    if (ds.fst) { u = v.u / 16.0f; t = v.v / 16.0f; }
    else {
        float q = v.q != 0.0f ? v.q : 1e-10f;
        u = v.s / q * (float)(1 << ds.tw);
        t = v.t / q * (float)(1 << ds.th);
    }
    if (!(u > -1e7f && u < 1e7f)) u = u > 0 ? 1e7f : -1e7f;
    if (!(t > -1e7f && t < 1e7f)) t = t > 0 ? 1e7f : -1e7f;
    g_bu0 = std::min(g_bu0, u); g_bu1 = std::max(g_bu1, u);
    g_bv0 = std::min(g_bv0, t); g_bv1 = std::max(g_bv1, t);
}

static inline GV mkv(const DrawState& ds, const Vertex& v, const Vertex& flat) {
    GV o;
    o.st0 = g_ps0;
    o.st1 = g_ps1;
    o.x = v.x / 16.0f;
    o.y = v.y / 16.0f;
    o.z = (float)v.z;
    o.r = v.r; o.g = v.g; o.b = v.b; o.a = v.a;
    if (ds.fst) { o.s = v.u / 16.0f; o.t = v.v / 16.0f; o.q = 1.0f; }
    else { o.s = v.s; o.t = v.t; o.q = v.q; }
    o.fog = v.f;
    o.fr = flat.r; o.fg = flat.g; o.fb = flat.b; o.fa = flat.a;
    return o;
}

// Widescreen HUD correction (as gs_gpu.cpp)
static u64 st_hud = 0;
struct HudSpan { int x0 = 1 << 30, x1 = -(1 << 30), width = 0, y0 = 1 << 30, y1 = -(1 << 30), n = 0; };
static std::unordered_map<u32, HudSpan> g_hud_span;
static std::unordered_set<u32> g_hud_fullwidth;
static void hud_frame_end() {
    g_hud_fullwidth.clear();
    for (auto& [k, s] : g_hud_span)
        if (s.width > 0 && s.x0 <= 4 * 16 && s.x1 >= (s.width - 4) * 16) g_hud_fullwidth.insert(k);
    g_hud_span.clear();
}
static bool hud_transform(const DrawState& ds, int prim, const Vertex* v, Vertex* out) {
    const platform::Config& cfg = platform::config();
    if (!cfg.widescreen_hack || !cfg.hud_fix) return false;
    if (ds.fbp != g_display_fbp[0] && ds.fbp != g_display_fbp[1]) return false;
    if (ds.tme && !g_shadow_ok[ds.tbp & 16383]) return false;
    int n = prim == 3 ? 3 : prim == 0 ? 1 : 2;
    if (ds.zte && ds.ztst != 1) return false;
    int minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (int i = 0; i < n; i++) {
        if (v[i].z != 0) return false;
        if (ds.tme && !ds.fst && fabsf(v[i].q - 1.0f) > 1e-3f) return false;
        minx = std::min(minx, v[i].x);
        maxx = std::max(maxx, v[i].x);
        miny = std::min(miny, v[i].y);
        maxy = std::max(maxy, v[i].y);
    }
    int width = (int)ds.fbw * 64;
    if (maxx - minx >= (width - 24) * 16) return false;
    if (maxy - miny >= 400 * 16) return false;
    bool black = v[0].r == 0 && v[0].g == 0 && v[0].b == 0;
    u32 key = (ds.tme ? (ds.tbp & 16383) : 0x3FFFu) | (ds.tw & 15) << 14 | (ds.th & 15) << 18 | (black ? 1u << 22 : 0);
    HudSpan& sp = g_hud_span[key];
    sp.x0 = std::min(sp.x0, minx);
    sp.x1 = std::max(sp.x1, maxx);
    sp.y0 = std::min(sp.y0, miny);
    sp.y1 = std::max(sp.y1, maxy);
    sp.n++;
    sp.width = width;
    bool pin_edges = g_hud_fullwidth.count(key) != 0;
    int cx = width * 8;
    float hf = platform::hud_factor();
    for (int i = 0; i < n; i++) {
        out[i] = v[i];
        if (pin_edges && (v[i].x <= 4 * 16 || v[i].x >= (width - 4) * 16)) continue;
        out[i].x = cx + (int)lrintf((v[i].x - cx) * hf);
    }
    st_hud++;
    return true;
}

static void prim_state(const DrawState& ds, u32& s0, u32& s1) {
    s0 = (ds.ate ? 1u : 0u) | (ds.atst & 7) << 1 | (ds.afail & 3) << 4 | (ds.date ? 1u : 0u) << 6 | (ds.datm ? 1u : 0u) << 7 |
         (ds.aref & 0xFF) << 8 | (ds.abe ? 1u : 0u) << 16 | (ds.ba & 3) << 17 | (ds.bb & 3) << 19 | (ds.bc & 3) << 21 |
         (ds.bd & 3) << 23 | (ds.pabe ? 1u : 0u) << 25 | (ds.fba ? 1u : 0u) << 26 | (ds.colclamp ? 1u : 0u) << 27 |
         (ds.fge ? 1u : 0u) << 28 | (ds.iip ? 1u : 0u) << 29 | (ds.fst ? 1u : 0u) << 30 | (ds.tme ? 1u : 0u) << 31;
    s1 = (ds.bfix & 0xFF) | (ds.fogcol & 0xFFFFFF) << 8;
}
static bool same_batch_state(const DrawState& a, const DrawState& b) {
    DrawState x = b;
    x.ate = a.ate; x.atst = a.atst; x.aref = a.aref; x.afail = a.afail; x.date = a.date; x.datm = a.datm;
    x.abe = a.abe; x.ba = a.ba; x.bb = a.bb; x.bc = a.bc; x.bd = a.bd; x.bfix = a.bfix; x.pabe = a.pabe; x.fba = a.fba;
    x.colclamp = a.colclamp; x.fge = a.fge; x.fogcol = a.fogcol; x.iip = a.iip; x.fst = a.fst; x.tme = a.tme;
    return same_draw_state(a, x);
}
static bool prim_overwrites(const DrawState& ds) {
    bool blend_reads_dst = ds.abe && (ds.ba == 1 || ds.bb == 1 || ds.bd == 1 || ds.bc == 1) &&
                           !(ds.ba == 0 && ds.bb == 1 && ds.bc == 2 && ds.bfix == 0x80 && ds.bd == 1);
    return !ds.date && (!ds.ate || ds.atst == 1) && !blend_reads_dst;
}

void on_draw(const DrawState& ds, u32 state_id, int prim, const Vertex* vin) {
    st_draws++;
    Vertex hv[3];
    bool hud = hud_transform(ds, prim, vin, hv);
    const Vertex* v = hud ? hv : vin;
    if (state_id != g_bid && g_bvalid && !g_verts.empty() && !g_bself && hud == g_bhud && !(ds.tme && g_bself_tex) &&
        !g_no_batch_join && same_batch_state(g_prev_raw, ds)) {
        st_why[2]++;
        g_prev_raw = ds;
        g_bid = state_id;
        g_btme |= ds.tme;
        g_bover &= prim_overwrites(ds);
        prim_state(ds, g_ps0, g_ps1);
    }
    if (state_id != g_bid || hud != g_bhud || g_bself) {
        g_prev_raw = ds;
        flush();
        g_bds = ds;
        g_bhud = hud;
        if (hud) {
            int width = (int)ds.fbw * 64, cx = width / 2;
            if (ds.scax0 > 0 || ds.scax1 < width - 1) {
                float hf = platform::hud_factor();
                g_bds.scax0 = (int)floorf(cx + (ds.scax0 - cx) * hf);
                g_bds.scax1 = (int)ceilf(cx + (ds.scax1 + 1 - cx) * hf) - 1;
            }
        }
        g_bid = state_id;
        g_bvalid = true;
        g_bx0 = g_by0 = 1 << 30;
        g_bx1 = g_by1 = -(1 << 30);
        g_bu0 = g_bv0 = 1e30f;
        g_bu1 = g_bv1 = -1e30f;
        g_ball_sprites = true;
        g_bsprites.clear();
        g_bself_tex = samples_own_writes(ds);
        g_bself = ds.tme && g_bself_tex;
        g_btme = ds.tme;
        g_bover = prim_overwrites(ds);
        prim_state(ds, g_ps0, g_ps1);
    }
    const DrawState& b = g_bds;
    if (prim != 6) g_ball_sprites = false;
    switch (prim) {
    case 3: {
        int minx = std::min({v[0].x, v[1].x, v[2].x}), maxx = std::max({v[0].x, v[1].x, v[2].x});
        int miny = std::min({v[0].y, v[1].y, v[2].y}), maxy = std::max({v[0].y, v[1].y, v[2].y});
        bounds_add(b, (minx + 15) >> 4, (miny + 15) >> 4, (maxx + 15) >> 4, (maxy + 15) >> 4);
        for (int i = 0; i < 3; i++) g_verts.push_back(mkv(ds, v[i], v[2]));
        if (ds.tme) for (int i = 0; i < 3; i++) uv_add(ds, v[i]);
        break;
    }
    case 6: {
        const Vertex& a = v[0];
        const Vertex& c = v[1];
        bounds_add(b, (std::min(a.x, c.x) + 15) >> 4, (std::min(a.y, c.y) + 15) >> 4, (std::max(a.x, c.x) + 15) >> 4,
                   (std::max(a.y, c.y) + 15) >> 4);
        if (g_ball_sprites && g_bsprites.size() < 64) {
            PixRect r{std::max((std::min(a.x, c.x) + 15) >> 4, b.scax0), std::max((std::min(a.y, c.y) + 15) >> 4, b.scay0),
                      std::min((std::max(a.x, c.x) + 15) >> 4, b.scax1 + 1), std::min((std::max(a.y, c.y) + 15) >> 4, b.scay1 + 1)};
            if (r.x1 > r.x0 && r.y1 > r.y0) g_bsprites.push_back(r);
        }
        GV p00 = mkv(ds, a, c), p11 = mkv(ds, c, c);
        GV p10 = p11, p01 = p11, q00 = p11;
        q00.x = p00.x; q00.y = p00.y; q00.s = p00.s; q00.t = p00.t;
        p10.x = p11.x; p10.y = p00.y; p10.s = p11.s; p10.t = p00.t;
        p01.x = p00.x; p01.y = p11.y; p01.s = p00.s; p01.t = p11.t;
        g_verts.push_back(q00); g_verts.push_back(p10); g_verts.push_back(p01);
        g_verts.push_back(p10); g_verts.push_back(p11); g_verts.push_back(p01);
        if (ds.tme) { uv_add(ds, a); uv_add(ds, c); }
        break;
    }
    case 1: {
        const Vertex& a = v[0];
        const Vertex& c = v[1];
        GV p0 = mkv(ds, a, c), p1 = mkv(ds, c, c);
        float dx = fabsf(p1.x - p0.x), dy = fabsf(p1.y - p0.y);
        GV p2 = p0, p3 = p1;
        if (dx >= dy) { p2.y += 1; p3.y += 1; } else { p2.x += 1; p3.x += 1; }
        g_verts.push_back(p0); g_verts.push_back(p1); g_verts.push_back(p2);
        g_verts.push_back(p1); g_verts.push_back(p3); g_verts.push_back(p2);
        bounds_add(b, (int)floorf(std::min(p0.x, p1.x)), (int)floorf(std::min(p0.y, p1.y)), (int)ceilf(std::max(p0.x, p1.x)) + 1,
                   (int)ceilf(std::max(p0.y, p1.y)) + 1);
        if (ds.tme) { uv_add(ds, a); uv_add(ds, c); }
        break;
    }
    case 0: {
        const Vertex& a = v[0];
        GV p = mkv(ds, a, a);
        p.x = (float)((a.x + 8) >> 4);
        p.y = (float)((a.y + 8) >> 4);
        GV p10 = p, p01 = p, p11 = p;
        p10.x += 1; p01.y += 1; p11.x += 1; p11.y += 1;
        g_verts.push_back(p); g_verts.push_back(p10); g_verts.push_back(p01);
        g_verts.push_back(p10); g_verts.push_back(p11); g_verts.push_back(p01);
        bounds_add(b, (int)p.x, (int)p.y, (int)p.x + 1, (int)p.y + 1);
        if (ds.tme) uv_add(ds, a);
        break;
    }
    }
    st_prims++;
    g_bprims++;
    if (g_verts.size() * sizeof(GV) > VB_BYTES - 1024 || g_frame_hashes) flush();  // test: --frame-hashes = a draw per primitive
}

// ===========================================================================
// Other commands (as gs_gpu.cpp)
// ===========================================================================
void on_clut_load(u64 tex0, u64 texclut) {
    u32 cbp = bits(tex0, 37, 14), cpsm = bits(tex0, 51, 4), csm = bits(tex0, 55, 1);
    std::vector<u32> blocks;
    if (csm == 0) for_rect_blocks(cpsm, cbp, 1, 0, 0, 16, 16, [&](u32 b, bool) { blocks.push_back(b); });
    else for_rect_blocks(cpsm, cbp, bits(texclut, 0, 6), bits(texclut, 6, 6) * 16, bits(texclut, 12, 10), bits(texclut, 6, 6) * 16 + 256,
                         bits(texclut, 12, 10) + 1, [&](u32 b, bool) { blocks.push_back(b); });
    bool stale = false;
    for (u32 b : blocks) stale |= !g_shadow_ok[b];
    if (stale) {
        std::vector<u32> need = same_family(blocks, cpsm);
        for (u32 b : blocks) set_shadow_ok(b, true);
        if (!need.empty()) { flush(); g_why = "clut"; make_shadow_current(need); }
    }
    u32 tmp[512];
    memcpy(tmp, g_clut, sizeof(tmp));
    exec_clut_load_m(shadow, tmp, tex0, texclut);
    if (memcmp(tmp, g_clut, sizeof(tmp)) != 0 || !g_clut_valid) {
        u32 t = g_bds.tpsm;
        bool uses_clut = g_btme && (t == PSMT8 || t == PSMT4 || t == PSMT8H || t == PSMT4HL || t == PSMT4HH);
        if (!g_verts.empty() && uses_clut) {
            st_why[14]++;
            flush();
        }
        memcpy(g_clut, tmp, sizeof(tmp));
        g_clut_dirty = true;
        g_clut_version++;
        g_clut_valid = true;
    }
}

void on_upload(const UploadInfo& u, const u8* data, size_t len) {
    flush();
    std::vector<u32> partial, all;
    for_rect_blocks(u.dpsm, u.dbp, u.dbw, (int)u.dx, (int)u.dy, (int)(u.dx + u.w), (int)(u.dy + u.h), [&](u32 b, bool full) {
        all.push_back(b);
        if (!full && !g_shadow_ok[b]) partial.push_back(b);
    });
    partial = same_family(partial, u.dpsm);
    partial.erase(std::remove_if(partial.begin(), partial.end(), [](u32 b) {
        Surface* o = surf_by_id(g_owner[b]);
        return !o || (o->bp != g_display_fbp[0] && o->bp != g_display_fbp[1]);
    }), partial.end());
    if (!partial.empty()) { g_why = "partial upload"; make_shadow_current(partial); }
    // Many games upload the same texture data again every frame: blocks whose content did not
    // change (CPU-owned, shadow current) keep their stamp, so textures on them need no rehash.
    static std::vector<u32> old;
    old.resize(all.size() * 64);
    for (size_t i = 0; i < all.size(); i++) memcpy(&old[i * 64], shadow + (size_t)all[i] * 64, 256);
    exec_upload_rows_m(shadow, u, data, len, 0, 1);
    for (size_t i = 0; i < all.size(); i++) {
        u32 b = all[i];
        if (g_owner[b] == -1 && g_shadow_ok[b] && memcmp(&old[i * 64], shadow + (size_t)b * 64, 256) == 0) {
            st_up_same++;
            continue;
        }
        mark_cpu_written(b);
    }
}

void on_copy(const CopyInfo& c) {
    flush();
    std::vector<u32> src;
    for_rect_blocks(c.spsm, c.sbp, c.sbw, (int)c.sx, (int)c.sy, (int)(c.sx + c.w), (int)(c.sy + c.h), [&](u32 b, bool) { src.push_back(b); });
    std::vector<u32> partial, all;
    for_rect_blocks(c.dpsm, c.dbp, c.dbw, (int)c.dx, (int)c.dy, (int)(c.dx + c.w), (int)(c.dy + c.h), [&](u32 b, bool full) {
        all.push_back(b);
        if (!full) partial.push_back(b);
    });
    src.insert(src.end(), partial.begin(), partial.end());
    g_why = "copy";
    make_shadow_current(src);
    static std::vector<u32> tmp;
    tmp.resize((size_t)c.w * c.h);
    for (u32 y = 0; y < c.h; y++)
        for (u32 x = 0; x < c.w; x++)
            tmp[(size_t)y * c.w + x] = read_pixel_m(shadow, c.spsm, (int)((c.sx + x) & 2047), (int)((c.sy + y) & 2047), c.sbp, c.sbw);
    for (u32 y = 0; y < c.h; y++)
        for (u32 x = 0; x < c.w; x++)
            write_pixel_m(shadow, c.dpsm, (int)((c.dx + x) & 2047), (int)((c.dy + y) & 2047), c.dbp, c.dbw, tmp[(size_t)y * c.w + x]);
    for (u32 b : all) mark_cpu_written(b);
}

// ===========================================================================
// Display
// ===========================================================================
static void ensure_out(int w, int h) {
    if (g_out.img && g_out.w == w && g_out.h == h) return;
    if (g_out_fb) fr().garbage_fb.push_back(g_out_fb);
    g_out_fb = VK_NULL_HANDLE;
    garbage(g_out);
    make_img(g_out, w, h, VK_FORMAT_R8G8B8A8_UNORM,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = g_rp_out;
    fi.attachmentCount = 1;
    fi.pAttachments = &g_out.view;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    vkCreateFramebuffer(dev, &fi, nullptr, &g_out_fb);
}

// the composed picture into a host buffer (recorded into the main buffer)
static void copy_out(Buf& b) {
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {(uint32_t)g_out.w, (uint32_t)g_out.h, 1};
    vkCmdCopyImageToBuffer(g_main, g_out.img, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &c);
}

void on_present(const PresentParams& p, const u32* sw_frame) {
    (void)sw_frame;
    flush();
    {
        u32 b = ((p.pmode >> 1) & 1) ? bits(p.dispfb[1], 0, 9) * 32 : bits(p.dispfb[0], 0, 9) * 32;
        if (b != g_display_fbp[1]) {
            g_display_fbp[0] = g_display_fbp[1];
            g_display_fbp[1] = b;
        }
    }
    g_frame++;
    hud_frame_end();
    if (g_frame % 600 == 0) evict_textures();
    bool en1 = p.pmode & 1, en2 = (p.pmode >> 1) & 1;
    struct PCB { s32 c1[4], c2[4], m[4], bg[4]; } pc{};
    Surface* s1 = nullptr;
    Surface* s2 = nullptr;
    auto setup = [&](int ci, s32* cc, Surface*& surf) {
        u64 fb = p.dispfb[ci];
        u32 fbp = bits(fb, 0, 9) * 32, fbw = bits(fb, 9, 6), psm = bits(fb, 15, 5);
        int dbx = (int)bits(fb, 32, 11), dby = (int)bits(fb, 43, 11);
        Layout l = layout_of(psm);
        if (l != L_C32 && l != L_C16 && l != L_C16S) return;
        int rows = p.field_mode ? (p.out_h + 1) / 2 : p.out_h;
        surf = get_surface(fbp, fbw, l, dby + rows);
        if (!surf) return;
        refresh(surf, dbx, dby, dbx + p.w, dby + rows);
        cc[0] = dbx; cc[1] = dby; cc[2] = psm == PSMCT24 ? 1 : (is16(l) ? 2 : 0); cc[3] = 1;
    };
    if (en1) setup(0, pc.c1, s1);
    if (en2) setup(1, pc.c2, s2);
    pc.m[0] = (s32)bits(p.pmode, 8, 8); pc.m[1] = (s32)bits(p.pmode, 5, 1); pc.m[2] = (s32)bits(p.pmode, 7, 1); pc.m[3] = p.field_mode;
    pc.bg[0] = (s32)(p.bgcolor & 0xFFFFFF); pc.bg[3] = S;
    int ow = p.w * S, oh = p.out_h * S;
    if (ow <= 0 || oh <= 0) return;
    end_pass();
    ensure_out(ow, oh);
    pre_layouts();
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g_rp_out;
    rb.framebuffer = g_out_fb;
    rb.renderArea = {{0, 0}, {(uint32_t)ow, (uint32_t)oh}};
    vkCmdBeginRenderPass(g_main, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)ow, (float)oh, 0, 1};
    VkRect2D sc{{0, 0}, {(uint32_t)ow, (uint32_t)oh}};
    vkCmdSetViewport(g_main, 0, 1, &vp);
    vkCmdSetScissor(g_main, 0, 1, &sc);
    vkCmdBindPipeline(g_main, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe_merge);
    VkDescriptorImageInfo i1{VK_NULL_HANDLE, s1 ? s1->im.view : g_dummy_u.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo i2{VK_NULL_HANDLE, s2 ? s2->im.view : g_dummy_u.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2];
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i1, nullptr, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &i2, nullptr, nullptr};
    pushDS(g_main, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl_merge, 0, 2, w);
    vkCmdPushConstants(g_main, g_pl_merge, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(g_main, 4, 1, 0, 0);
    vkCmdEndRenderPass(g_main);
    full_barrier(g_main);

    // --gpu-compare (x1): the software rasterizer's composition of the same frame next to ours
    if (sw_frame && S == 1) {
        copy_out(g_rb);
        submit_wait();
        invalidate(g_rb);
        const u32* g = (const u32*)g_rb.map;
        u64 bad = 0;
        int maxd = 0;
        for (int i = 0; i < ow * oh; i++) {
            u32 a = g[i] & 0xFFFFFF, b = sw_frame[i] & 0xFFFFFF;
            if (a != b) {
                bad++;
                for (int k = 0; k < 3; k++) maxd = std::max(maxd, std::abs((int)((a >> (k * 8)) & 0xFF) - (int)((b >> (k * 8)) & 0xFF)));
            }
        }
        static u64 frames = 0, bad_frames = 0, last_dump = 0;
        frames++;
        if (bad) {
            bad_frames++;
            static int logged = 0;
            if (logged++ < 100 || g_frame % 60 == 0)
                log_write(LOG_GS, "gpu compare: frame %llu: %llu pixels differ (max channel diff %d), bad frames %llu/%llu",
                          (unsigned long long)g_frame, (unsigned long long)bad, maxd, (unsigned long long)bad_frames, (unsigned long long)frames);
            if (bad > 200 && g_frame - last_dump > 120 && platform::shots_enabled()) {
                last_dump = g_frame;
                platform::write_shot(g, ow, oh, ow);
                platform::write_shot(sw_frame, ow, oh, ow);
            }
        }
    }
    // --shots: the picture right away (waits for the GPU)
    else if (platform::interval_shot_due()) {
        copy_out(g_rb);
        submit_wait();
        invalidate(g_rb);
        platform::write_shot((const u32*)g_rb.map, ow, oh, ow);
    }
    // movies: a copy for the letterbox detection, read when this frame's resources come back
    FrameRes& f = fr();
    if (kernel::movie_running()) {
        size_t need = (size_t)ow * oh * 4;
        if (f.movie.size < need) {
            destroy_now(f.movie);  // (this frame's resources are not in use: recycled at its start)
            make_buf(f.movie, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        }
        if (f.movie.buf) {
            copy_out(f.movie);
            f.movie_w = ow;
            f.movie_h = oh;
            f.movie_valid = true;
        }
    } else {
        g_movie_w = g_movie_h = 0;
    }
    full_barrier(g_main);
    prof_ts(g_main, 'X');
    if (g_gpu_timing && g_qpool) {
        vkCmdWriteTimestamp(g_main, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qpool, 2 * g_fi + 1);
        g_ts_written[g_fi] = true;
    }
    submit();
    g_last_ow = ow;
    g_last_oh = oh;
    bool mv = kernel::movie_running() && g_movie_w == ow && g_movie_h == oh;
    platform::present_gpu(g_out.view, ow, oh, mv ? g_movie_px.data() : nullptr);
    // continue on the other frame's resources (waits for the frame before this one)
    g_fi = (g_fi + 1) % NF;
    FrameRes& n = fr();
    wait_frame(n);
    read_movie(n);
    timing_read(g_fi);
    prof_read(g_fi);
    pcache_save();
    recycle(n);
    begin_cmds(true);
}

void on_represent() {
    if (!g_out.img || g_last_ow <= 0) return;
    platform::present_gpu(g_out.view, g_last_ow, g_last_oh, nullptr);
}

void on_readback(const CopyInfo& r) {
    flush();
    std::vector<u32> blocks;
    for_rect_blocks(r.spsm, r.sbp, r.sbw, (int)r.sx, (int)r.sy, (int)(r.sx + r.w), (int)(r.sy + r.h), [&](u32 b, bool) { blocks.push_back(b); });
    g_why = "readback to EE";
    make_shadow_current(blocks);
}

const u32* memory() { return shadow; }

std::string stats() {
    static double l_pms = 0;
    static u64 l_pnew, l_up_same, l_gcopy, l_skip, l_rehash, l_draws, l_batches, l_refresh, l_rbc, l_rb, l_dec, l_hits, l_stale, l_self, l_passes, l_submits, l_vers;
    char b[1024];
    snprintf(b, sizeof(b), "gpu: %llu prims in %llu batches, %llu passes, refresh %llu cells (gpu copies %llu, skipped %llu), readback %llu cells in %llu, tex decode %llu hit %llu (same data %llu, new versions %llu) stale %llu, self %llu, surfaces %d, unchanged upload blocks %llu, new pipelines %llu (%.0f ms)",
             (unsigned long long)(st_draws - l_draws), (unsigned long long)(st_batches - l_batches), (unsigned long long)(st_passes - l_passes),
             (unsigned long long)(st_refresh_cells - l_refresh), (unsigned long long)(st_gpu_copy_cells - l_gcopy), (unsigned long long)(st_skip_cells - l_skip),
             (unsigned long long)(st_readback_cells - l_rbc), (unsigned long long)(st_readbacks - l_rb), (unsigned long long)(st_tex_decodes - l_dec),
             (unsigned long long)(st_tex_hits - l_hits), (unsigned long long)(st_tex_rehash - l_rehash), (unsigned long long)(st_tex_versions - l_vers),
             (unsigned long long)(st_stale_texels - l_stale), (unsigned long long)(st_self - l_self),
             (int)std::count_if(g_surf.begin(), g_surf.end(), [](Surface* s) { return s != nullptr; }),
             (unsigned long long)(st_up_same - l_up_same), (unsigned long long)(st_pipes_new - l_pnew), st_pipes_ms - l_pms);
    l_pnew = st_pipes_new;
    l_pms = st_pipes_ms;
    (void)l_submits;
    l_up_same = st_up_same;
    l_gcopy = st_gpu_copy_cells; l_skip = st_skip_cells; l_rehash = st_tex_rehash; l_passes = st_passes; l_vers = st_tex_versions;
    l_draws = st_draws; l_batches = st_batches; l_refresh = st_refresh_cells; l_rbc = st_readback_cells; l_rb = st_readbacks;
    l_dec = st_tex_decodes; l_hits = st_tex_hits; l_stale = st_stale_texels; l_self = st_self;
    static u64 l_rep = 0;
    if (st_tex_replaced != l_rep) {
        size_t n = strlen(b);
        snprintf(b + n, sizeof(b) - n, ", replaced textures %llu", (unsigned long long)(st_tex_replaced - l_rep));
        l_rep = st_tex_replaced;
    }
    return b;
}

void state_vram(u8* vram, u32* clut, bool load) {
    flush();
    if (!load) {
        std::vector<u32> stale;
        for (u32 b = 0; b < 16384; b++)
            if (!g_shadow_ok[b]) stale.push_back(b);
        g_why = "savestate";
        make_shadow_current(stale);
        memcpy(vram, shadow, 4u << 20);
        memcpy(clut, g_clut, sizeof(g_clut));
        return;
    }
    memcpy(shadow, vram, 4u << 20);
    for (u32 b = 0; b < 16384; b++) mark_cpu_written(b);
    memcpy(g_clut, clut, sizeof(g_clut));
    g_clut_dirty = true;
    g_clut_version++;
    g_clut_valid = true;
}

}  // namespace gpu
}  // namespace gs
