// Touch controls: a translucent virtual pad drawn over the picture (left: L1/L2, d-pad, analog
// stick; right: R1/R2, triangle/circle/cross/square; bottom center: SELECT, START; top center: the
// settings menu). Multi-touch;
// sliding between buttons works. Hidden while a game controller is used, back on the next touch.
// Layout in window-buffer pixels: the present thread passes the buffer and window sizes.
#include "touch_android.h"
#include <android/input.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <cstring>
#include <mutex>

namespace touch {

namespace {
std::mutex g_m;
int g_bw = 0, g_bh = 0, g_ww = 1, g_wh = 1;  // buffer and window sizes
std::atomic<bool> g_visible{true};
std::atomic<u32> g_buttons{0};
std::atomic<int> g_lx{0}, g_ly{0};
std::atomic<bool> g_stick_on{false};
std::atomic<u64> g_version{1};  // changes with every change of the overlay's looks

enum Ctl { NONE, STICK, BUTTONS };
struct Pointer { Ctl ctl; float x, y; };
std::map<int32_t, Pointer> g_ptr;  // pointer id -> what it controls (input thread)

struct Btn { u32 bit; float x0, y0, x1, y1; bool round; };  // round: circle in the box
struct Layout {
    float u = 1;
    float stick_x = 0, stick_y = 0, stick_r = 0;
    float dpad_x = 0, dpad_y = 0, dpad_r = 0;
    Btn face[4], shoulder[4], small[2], menu;
};
Layout g_lay;

// libpad bits
constexpr u32 SELECT = 0x0001, START = 0x0008, UP = 0x0010, RIGHT = 0x0020, DOWN = 0x0040, LEFT = 0x0080, L2 = 0x0100,
              R2 = 0x0200, L1 = 0x0400, R1 = 0x0800, TRI = 0x1000, CIR = 0x2000, CRO = 0x4000, SQU = 0x8000;
constexpr u32 MENU = 0x10000;  // not a pad button: opens the settings menu
std::atomic<bool> g_menu_tap{false};

Layout make_layout(int bw, int bh) {
    Layout l;
    float u = bh / 448.0f;
    l.u = u;
    l.stick_x = 112 * u, l.stick_y = 320 * u, l.stick_r = 70 * u;
    l.dpad_x = 112 * u, l.dpad_y = 150 * u, l.dpad_r = 60 * u;
    float fx = bw - 112 * u, fy = 250 * u, fo = 52 * u, fr = 27 * u;
    l.face[0] = {TRI, fx - fr, fy - fo - fr, fx + fr, fy - fo + fr, true};
    l.face[1] = {CIR, fx + fo - fr, fy - fr, fx + fo + fr, fy + fr, true};
    l.face[2] = {CRO, fx - fr, fy + fo - fr, fx + fr, fy + fo + fr, true};
    l.face[3] = {SQU, fx - fo - fr, fy - fr, fx - fo + fr, fy + fr, true};
    l.shoulder[0] = {L2, 14 * u, 10 * u, 104 * u, 44 * u, false};
    l.shoulder[1] = {L1, 114 * u, 10 * u, 204 * u, 44 * u, false};
    l.shoulder[2] = {R1, bw - 204 * u, 10 * u, bw - 114 * u, 44 * u, false};
    l.shoulder[3] = {R2, bw - 104 * u, 10 * u, bw - 14 * u, 44 * u, false};
    l.small[0] = {SELECT, bw / 2.0f - 112 * u, 410 * u, bw / 2.0f - 24 * u, 438 * u, false};
    l.small[1] = {START, bw / 2.0f + 24 * u, 410 * u, bw / 2.0f + 112 * u, 438 * u, false};
    l.menu = {MENU, bw / 2.0f - 30 * u, 10 * u, bw / 2.0f + 30 * u, 40 * u, false};
    return l;
}

// which buttons a point presses (hit areas a little larger than the drawn shapes)
u32 hit(const Layout& l, float x, float y) {
    u32 b = 0;
    float m = 10 * l.u;
    for (const Btn& k : l.face) {
        float cx = (k.x0 + k.x1) / 2, cy = (k.y0 + k.y1) / 2, r = (k.x1 - k.x0) / 2 + m;
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) b |= k.bit;
    }
    for (const Btn* k : {&l.shoulder[0], &l.shoulder[1], &l.shoulder[2], &l.shoulder[3], &l.small[0], &l.small[1], &l.menu})
        if (x >= k->x0 - m && x <= k->x1 + m && y >= k->y0 - m && y <= k->y1 + m) b |= k->bit;
    float dx = x - l.dpad_x, dy = y - l.dpad_y, d = std::sqrt(dx * dx + dy * dy);
    if (d <= l.dpad_r + m && d > 8 * l.u) {
        float a = std::atan2(dy, dx);  // 8 sectors: diagonals press two directions
        int s = (int)std::lround(a / (3.14159265f / 4)) & 7;
        static const u32 dirs[8] = {RIGHT, RIGHT | DOWN, DOWN, DOWN | LEFT, LEFT, LEFT | UP, UP, UP | RIGHT};
        b |= dirs[s];
    }
    return b;
}

void update_from_pointers(const Layout& l) {
    u32 b = 0;
    bool stick = false;
    for (auto& [id, p] : g_ptr) {
        if (p.ctl == STICK) {
            float dx = (p.x - l.stick_x) / l.stick_r, dy = (p.y - l.stick_y) / l.stick_r;
            float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1) dx /= len, dy /= len;
            g_lx = (int)(dx * 32767), g_ly = (int)(dy * 32767);
            stick = true;
        } else {
            b |= hit(l, p.x, p.y);
        }
    }
    if (!stick) g_lx = 0, g_ly = 0;
    g_version++;
    g_stick_on = stick;
    static int logged = 0;
    if (b != g_buttons && logged++ < 100) log_write(LOG_PAD, "touch: buttons %04x", b);
    if ((b & MENU) && !(g_buttons & MENU)) g_menu_tap = true;
    g_buttons = b;
}

// ---- drawing (buffer pixels are RGBX, R in the low byte) ----
inline void blend(u32& d, u32 rgb, float a) {
    if (a <= 0) return;
    if (a > 1) a = 1;
    u32 ia = (u32)(a * 256), na = 256 - ia;
    // premultiplied "over": the colour and the alpha (opaque targets stay opaque)
    u32 rb = ((d & 0x00FF00FF) * na + (rgb & 0x00FF00FF) * ia) >> 8;
    u32 g = ((d & 0x0000FF00) * na + (rgb & 0x0000FF00) * ia) >> 8;
    u32 al = ((d >> 24) * na + 255 * ia) >> 8;
    d = (al << 24) | (rb & 0x00FF00FF) | (g & 0x0000FF00);
}
struct Canvas { u32* px; int w, h, stride; };

// shape by signed distance: coverage with a soft one-pixel edge
template <class F>
void shape(Canvas& c, float x0, float y0, float x1, float y1, u32 rgb, float alpha, F dist) {
    int ix0 = std::max(0, (int)x0 - 1), iy0 = std::max(0, (int)y0 - 1);
    int ix1 = std::min(c.w - 1, (int)x1 + 1), iy1 = std::min(c.h - 1, (int)y1 + 1);
    for (int y = iy0; y <= iy1; y++)
        for (int x = ix0; x <= ix1; x++) {
            float d = dist(x + 0.5f, y + 0.5f);
            if (d < 0.5f) blend(c.px[(size_t)y * c.stride + x], rgb, alpha * std::min(1.0f, 0.5f - d));
        }
}
void disc(Canvas& c, float cx, float cy, float r, u32 rgb, float a) {
    shape(c, cx - r, cy - r, cx + r, cy + r, rgb, a, [=](float x, float y) { return std::hypot(x - cx, y - cy) - r; });
}
void ring(Canvas& c, float cx, float cy, float r, float t, u32 rgb, float a) {
    shape(c, cx - r - t, cy - r - t, cx + r + t, cy + r + t, rgb, a,
          [=](float x, float y) { return std::fabs(std::hypot(x - cx, y - cy) - r) - t / 2; });
}
float seg_dist(float x, float y, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = x - ax, wy = y - ay;
    float t = std::clamp((wx * vx + wy * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
    return std::hypot(wx - t * vx, wy - t * vy);
}
void line(Canvas& c, float ax, float ay, float bx, float by, float t, u32 rgb, float a) {
    shape(c, std::min(ax, bx) - t, std::min(ay, by) - t, std::max(ax, bx) + t, std::max(ay, by) + t, rgb, a,
          [=](float x, float y) { return seg_dist(x, y, ax, ay, bx, by) - t / 2; });
}
void rrect(Canvas& c, float x0, float y0, float x1, float y1, float r, u32 rgb, float a) {
    shape(c, x0, y0, x1, y1, rgb, a, [=](float x, float y) {
        float qx = std::fabs(x - (x0 + x1) / 2) - ((x1 - x0) / 2 - r), qy = std::fabs(y - (y0 + y1) / 2) - ((y1 - y0) / 2 - r);
        return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
    });
}

// 5x7 glyphs for the button labels
const char* glyph(char ch) {
    switch (ch) {
    case 'L': return "10000100001000010000100001111";
    case 'R': return "11110100011000111110101001001";
    case '1': return "00100011000010000100001000111";
    case '2': return "01110100010001000100010001111";
    case 'S': return "01111100001000001110000011110";
    case 'T': return "11111001000010000100001000010";
    case 'A': return "01110100011000111111100011000";
    case 'E': return "11111100001111010000100001111";
    case 'C': return "01111100001000010000100000111";
    default: return nullptr;
    }
}
void text(Canvas& c, const char* s, float cx, float cy, float px, u32 rgb, float a) {
    int n = (int)strlen(s);
    float w = n * 6 * px - px, x = cx - w / 2, y = cy - 3.5f * px;
    for (int i = 0; i < n; i++, x += 6 * px) {
        const char* g = glyph(s[i]);
        if (!g) continue;
        for (int r = 0; r < 7; r++)
            for (int k = 0; k < 5; k++)
                if (g[r * 5 + k] == '1') {
                    int gx0 = (int)(x + k * px), gy0 = (int)(y + r * px), gx1 = (int)(x + (k + 1) * px), gy1 = (int)(y + (r + 1) * px);
                    for (int yy = std::max(0, gy0); yy < std::min(c.h, gy1); yy++)
                        for (int xx = std::max(0, gx0); xx < std::min(c.w, gx1); xx++) blend(c.px[(size_t)yy * c.stride + xx], rgb, a);
                }
    }
}
}  // namespace

void layout(int bw, int bh, int ww, int wh) {
    std::lock_guard<std::mutex> lk(g_m);
    if (bw == g_bw && bh == g_bh && ww == g_ww && wh == g_wh) return;
    g_version++;
    g_bw = bw, g_bh = bh, g_ww = std::max(1, ww), g_wh = std::max(1, wh);
    g_lay = make_layout(bw, bh);
}

void draw(u32* px, int stride) {
    if (!g_visible) return;
    Layout l;
    int bw, bh;
    {
        std::lock_guard<std::mutex> lk(g_m);
        l = g_lay;
        bw = g_bw, bh = g_bh;
    }
    if (bw <= 0) return;
    Canvas c{px, bw, bh, stride};
    u32 pressed = g_buttons;
    const u32 WHITE = 0xFFFFFF, DARK = 0x000000;
    float u = l.u, base = 0.18f, on = 0.42f;
    // analog stick
    ring(c, l.stick_x, l.stick_y, l.stick_r, 3 * u, WHITE, 0.35f);
    disc(c, l.stick_x, l.stick_y, l.stick_r, DARK, 0.12f);
    float kx = l.stick_x + g_lx / 32767.0f * l.stick_r * 0.7f, ky = l.stick_y + g_ly / 32767.0f * l.stick_r * 0.7f;
    disc(c, kx, ky, 30 * u, WHITE, g_stick_on ? on : base + 0.08f);
    // d-pad: four arms
    struct Arm { u32 bit; float dx, dy; };
    for (Arm a : {Arm{UP, 0, -1}, Arm{DOWN, 0, 1}, Arm{LEFT, -1, 0}, Arm{RIGHT, 1, 0}}) {
        float cx = l.dpad_x + a.dx * 36 * u, cy = l.dpad_y + a.dy * 36 * u, h = 18 * u;
        rrect(c, cx - h, cy - h, cx + h, cy + h, 5 * u, WHITE, (pressed & a.bit) ? on : base);
        // arrow
        float tx = cx + a.dx * 7 * u, ty = cy + a.dy * 7 * u, px = -a.dy, py = a.dx;
        line(c, tx, ty, tx - a.dx * 9 * u + px * 7 * u, ty - a.dy * 9 * u + py * 7 * u, 3 * u, WHITE, 0.5f);
        line(c, tx, ty, tx - a.dx * 9 * u - px * 7 * u, ty - a.dy * 9 * u - py * 7 * u, 3 * u, WHITE, 0.5f);
    }
    // face buttons with their symbols in the PlayStation colors
    for (const Btn& k : l.face) {
        float cx = (k.x0 + k.x1) / 2, cy = (k.y0 + k.y1) / 2, r = (k.x1 - k.x0) / 2, s = r * 0.48f, t = 3 * u;
        disc(c, cx, cy, r, WHITE, (pressed & k.bit) ? on : base);
        if (k.bit == TRI) {
            u32 g = 0x86D33A;  // green (RGB with R in the low byte: 0xBBGGRR)
            float ax = cx, ay = cy - s, bx = cx + s * 0.95f, by = cy + s * 0.65f, qx = cx - s * 0.95f;
            line(c, ax, ay, bx, by, t, g, 0.75f);
            line(c, bx, by, qx, by, t, g, 0.75f);
            line(c, qx, by, ax, ay, t, g, 0.75f);
        } else if (k.bit == CIR) {
            ring(c, cx, cy, s * 0.85f, t, 0x4A4AF0, 0.75f);
        } else if (k.bit == CRO) {
            u32 b = 0xF0A060;
            line(c, cx - s * 0.75f, cy - s * 0.75f, cx + s * 0.75f, cy + s * 0.75f, t, b, 0.75f);
            line(c, cx - s * 0.75f, cy + s * 0.75f, cx + s * 0.75f, cy - s * 0.75f, t, b, 0.75f);
        } else {
            u32 p = 0xD08CE8;
            float h = s * 0.7f;
            line(c, cx - h, cy - h, cx + h, cy - h, t, p, 0.75f);
            line(c, cx + h, cy - h, cx + h, cy + h, t, p, 0.75f);
            line(c, cx + h, cy + h, cx - h, cy + h, t, p, 0.75f);
            line(c, cx - h, cy + h, cx - h, cy - h, t, p, 0.75f);
        }
    }
    static const char* SH[4] = {"L2", "L1", "R1", "R2"};
    for (int i = 0; i < 4; i++) {
        const Btn& k = l.shoulder[i];
        rrect(c, k.x0, k.y0, k.x1, k.y1, 10 * u, WHITE, (pressed & k.bit) ? on : base);
        text(c, SH[i], (k.x0 + k.x1) / 2, (k.y0 + k.y1) / 2, 2.2f * u, WHITE, 0.6f);
    }
    {
        // menu button: three bars
        const Btn& k = l.menu;
        rrect(c, k.x0, k.y0, k.x1, k.y1, 10 * u, WHITE, (pressed & MENU) ? on : base);
        float cx = (k.x0 + k.x1) / 2, cy = (k.y0 + k.y1) / 2;
        for (int i = -1; i <= 1; i++) line(c, cx - 11 * u, cy + i * 7 * u, cx + 11 * u, cy + i * 7 * u, 3 * u, WHITE, 0.6f);
    }
    static const char* SM[2] = {"SELECT", "START"};
    for (int i = 0; i < 2; i++) {
        const Btn& k = l.small[i];
        rrect(c, k.x0, k.y0, k.x1, k.y1, 13 * u, WHITE, (pressed & k.bit) ? on : base);
        text(c, SM[i], (k.x0 + k.x1) / 2, (k.y0 + k.y1) / 2, 1.6f * u, WHITE, 0.6f);
    }
}

bool on_motion(const AInputEvent* e) {
    if ((AInputEvent_getSource(e) & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) return false;
    Layout l;
    float sx, sy;
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (g_bw <= 0) return true;
        l = g_lay;
        sx = (float)g_bw / g_ww;
        sy = (float)g_bh / g_wh;
    }
    if (!g_visible.exchange(true)) g_version++;
    int32_t act = AMotionEvent_getAction(e), kind = act & AMOTION_EVENT_ACTION_MASK;
    size_t idx = (size_t)((act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    if (kind == AMOTION_EVENT_ACTION_CANCEL) {
        g_ptr.clear();
    } else if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_POINTER_UP) {
        g_ptr.erase(AMotionEvent_getPointerId(e, kind == AMOTION_EVENT_ACTION_UP ? 0 : idx));
        if (kind == AMOTION_EVENT_ACTION_UP) g_ptr.clear();
    } else {
        size_t n = AMotionEvent_getPointerCount(e);
        for (size_t i = 0; i < n; i++) {
            int32_t id = AMotionEvent_getPointerId(e, i);
            float x = AMotionEvent_getX(e, i) * sx, y = AMotionEvent_getY(e, i) * sy;
            auto it = g_ptr.find(id);
            if (it == g_ptr.end()) {
                // a new finger: on the stick area it drives the stick until it is lifted
                bool on_stick = std::hypot(x - l.stick_x, y - l.stick_y) <= l.stick_r * 1.3f;
                g_ptr[id] = {on_stick ? STICK : BUTTONS, x, y};
            } else {
                it->second.x = x, it->second.y = y;
            }
        }
    }
    update_from_pointers(l);
    return true;
}

void controller_used() {
    if (g_visible.exchange(false)) {
        g_version++;
        g_buttons = 0;
        g_lx = g_ly = 0;
        g_stick_on = false;
    }
}

u32 buttons() { return g_buttons & 0xFFFF; }
bool take_menu_tap() { return g_menu_tap.exchange(false); }
void release_all() {
    std::lock_guard<std::mutex> lk(g_m);
    g_ptr.clear();
    g_buttons = 0;
    g_lx = g_ly = 0;
    g_stick_on = false;
    g_version++;
}
u64 version() { return g_version; }
bool visible() { return g_visible; }
bool stick(int* lx, int* ly) {
    if (!g_stick_on) return false;
    *lx = g_lx, *ly = g_ly;
    return true;
}

}  // namespace touch
