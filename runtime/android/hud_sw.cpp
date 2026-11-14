// Widescreen HUD correction for the software renderer (the GPU renderer has its own, gs_gpu.cpp
// hud_transform): 2D elements (all vertices at z = 0 without perspective, no depth test) drawn into a
// displayed frame buffer are narrowed to their 4:3 proportions around the screen center, their
// scissor with them. Full-width layers (backgrounds, fades, screen filters) keep the full width.
// Same rules as on the GPU; the GPU's "texture came from an upload" test becomes "texture is not in a
// buffer drawn to lately" (screen effects sample the rendered picture, menus use uploaded textures).
#include "gs/gs_drawstate.h"
#include "gs/gs_state.h"
#include "platform/platform.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gs {
extern bool (*g_sw_prim_hook)(const DrawState& ds, int prim, const Vertex* v, Vertex* out, int* scax0, int* scax1);
extern void (*g_sw_present_hook)(const PresentParams& p);
}

namespace {
using namespace gs;

u32 g_display_fbp[2] = {~0u, ~0u};  // the two most recently displayed frame buffers (block addresses)
struct Target { u32 bp, blocks; };
std::vector<Target> g_targets, g_targets_prev;  // buffers drawn to this frame and the last one

struct HudSpan { int x0 = 1 << 30, x1 = -(1 << 30), width = 0, n = 0; };
std::unordered_map<u32, HudSpan> g_span;  // this frame: x range of the 2D pieces per texture
std::unordered_set<u32> g_fullwidth;      // textures whose pieces spanned the width last frame

void note_target(const DrawState& ds) {
    for (const Target& t : g_targets)
        if (t.bp == ds.fbp) return;
    g_targets.push_back({ds.fbp, ds.fbw * 64u * 512u * 4u / 256u});  // up to 512 rows of 32-bit pixels
}
bool drawn_lately(u32 tbp) {
    for (const auto* list : {&g_targets, &g_targets_prev})
        for (const Target& t : *list)
            if (tbp >= t.bp && tbp < t.bp + t.blocks) return true;
    return false;
}

bool prim_hook(const DrawState& ds, int prim, const Vertex* v, Vertex* out, int* scax0, int* scax1) {
    note_target(ds);
    const platform::Config& cfg = platform::config();
    if (!cfg.widescreen_hack || !cfg.hud_fix) return false;
    if (ds.fbp != g_display_fbp[0] && ds.fbp != g_display_fbp[1]) return false;
    if (ds.tme && drawn_lately(ds.tbp & 16383)) return false;
    int n = prim == 3 ? 3 : prim == 0 ? 1 : 2;
    if (prim > 6) return false;
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
    if (maxx - minx >= (width - 24) * 16) return false;  // full-width layer
    if (maxy - miny >= 400 * 16) return false;           // full-height strips
    bool black = v[0].r == 0 && v[0].g == 0 && v[0].b == 0;
    u32 key = (ds.tme ? (ds.tbp & 16383) : 0x3FFFu) | (ds.tw & 15) << 14 | (ds.th & 15) << 18 | (black ? 1u << 22 : 0);
    HudSpan& sp = g_span[key];
    sp.x0 = std::min(sp.x0, minx);
    sp.x1 = std::max(sp.x1, maxx);
    sp.n++;
    sp.width = width;
    bool pin_edges = g_fullwidth.count(key) != 0;
    int cx = width * 8;  // center in 12.4 fixed point
    float hf = platform::hud_factor();
    for (int i = 0; i < n; i++) {
        out[i] = v[i];
        if (pin_edges && (v[i].x <= 4 * 16 || v[i].x >= (width - 4) * 16)) continue;
        out[i].x = cx + (int)lrintf((v[i].x - cx) * hf);
    }
    // the scissor of a narrowed element (text boxes clip their contents) narrows with it
    if (ds.scax0 > 0 || ds.scax1 < width - 1) {
        int c = width / 2;
        *scax0 = (int)floorf(c + (ds.scax0 - c) * hf);
        *scax1 = (int)ceilf(c + (ds.scax1 + 1 - c) * hf) - 1;
    }
    return true;
}

void present_hook(const PresentParams& p) {
    // double buffering: the last two distinct buffers shown are the frame buffers
    u64 fb = ((p.pmode >> 1) & 1) ? p.dispfb[1] : p.dispfb[0];
    u32 b = (u32)(fb & 0x1FF) * 32;
    if (b != g_display_fbp[1]) {
        g_display_fbp[0] = g_display_fbp[1];
        g_display_fbp[1] = b;
    }
    g_fullwidth.clear();
    for (auto& [k, s] : g_span)
        if (s.width > 0 && s.x0 <= 4 * 16 && s.x1 >= (s.width - 4) * 16) g_fullwidth.insert(k);
    g_span.clear();
    g_targets_prev.swap(g_targets);
    g_targets.clear();
}
}  // namespace

void hud_sw_install() {
    gs::g_sw_prim_hook = prim_hook;
    gs::g_sw_present_hook = present_hook;
}
