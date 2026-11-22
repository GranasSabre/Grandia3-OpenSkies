// Android platform layer (first test version): NativeActivity through android_native_app_glue,
// the software renderer's frames drawn into the ANativeWindow, AAudio output, game controllers.
// The portable pieces of platform.cpp (config file, game patches, autopress, cutscene skip, pad
// logic) are repeated here for now; they move into a shared file when the platform split goes back
// into the main branch.
#include "platform/platform.h"
#include "platform/audio.h"
#include "platform/subtitles.h"
#include "gs/gs_gpu.h"
#include "hw/gspipe.h"
#include "ee/kernel.h"
#include "savestate.h"
#include "touch_android.h"
#include "memory.h"
#include "vk_ctx.h"
#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <aaudio/AAudio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern std::string g3_android_dir;  // win32_shim.cpp
struct ANativeActivity;
extern ANativeActivity* g3_activity;  // jni_android.cpp (calls into Java)
std::string g3_pick_document();
bool g3_show_message(const std::string& utf8);
extern "C" int g_ee_patch[8];
namespace kernel { extern std::atomic<bool> g_pause_req; }
int g3_main(int argc, char** argv);
void hud_sw_install();  // hud_sw.cpp
void vk_probe();        // vk_probe.cpp  // main.cpp, renamed by the build (-Dmain=g3_main)

#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, "Grandia3", __VA_ARGS__)

static android_app* g_app = nullptr;
static std::mutex g_win_mutex;
static std::condition_variable g_win_cv;
static ANativeWindow* g_window = nullptr;
static std::atomic<bool> g_quit{false};

// ===========================================================================
// platform
// ===========================================================================
namespace platform {

static Config g_cfg;
static std::string g_cfg_path;
static std::string g_gpu_driver;  // custom Vulkan driver folder under files/gpu_drivers ("" = system; vk_dyn.cpp)

bool g_force_steam_deck = false;
bool g_shots_final = false;
double g_window_test_at = 0, g_settings_test_at = 0, g_overlay_test_at = 0, g_overlay_shot_at = 0;
int g_overlay_test_downs = 4;
std::vector<double> g_ram_dump_at;
bool g_log_fps = false;
double g_turbo_from = 0;

Config& config() { return g_cfg; }
bool console_enabled() { return false; }
void init_console(bool) {}
void fatal_message(const char* msg) { ALOG("fatal: %s", msg); }
static std::string utf8(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) {
        u32 c = (u32)*w;
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | c >> 6); s += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { s += (char)(0xE0 | c >> 12); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xF0 | c >> 18); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }
    return s;
}
void message(const wchar_t* text) {
    std::string s = utf8(text);
    log_write(LOG_MAIN, "message: %s", s.c_str());
    ALOG("message: %s", s.c_str());
    g3_show_message(s);
}
// the system file picker (GrandiaActivity); the result is a content:// URI that win32_shim.cpp opens
// through the activity. disc1.iso in the app folder (tests, adb) is found before this is asked.
// option jp_voices (Undub): the player's Japanese disc 1 in the system file picker
std::string choose_jp_image() {
    message(tr(L"Undub: Abbild von Grandia III (Japan) Disc 1 wählen (ISO)"));
    return g3_pick_document();
}
// Cancelled once: the hint again and a second chance; cancelled again: "" (the game ends, the app closes).
std::string choose_disc_image(int disc) {
    std::string s = g3_pick_document();
    if (!s.empty()) return s;
    bool de = g_cfg.language == "de";
    if (disc == 2)
        message(de ? L"Ohne das Abbild (ISO) deiner Disc 2 kann Grandia III nicht weiterlaufen. Bitte auswählen; nochmal Abbrechen schließt die App."
                   : L"Grandia III cannot go on without the image (ISO) of your disc 2. Please choose it; cancelling again closes the app.");
    else
        message(de ? L"Ohne das Abbild (ISO) deiner Disc 1 kann Grandia III nicht starten. Bitte auswählen; nochmal Abbrechen schließt die App."
                   : L"Grandia III cannot start without the image (ISO) of your disc 1. Please choose it; cancelling again closes the app.");
    return g3_pick_document();
}
bool is_steam_deck() { return false; }
std::string steam_deck_info() { return "no (Android)"; }
void default_input(Config& c) {
    for (int i = 0; i < 16; i++) c.keymap[i] = 0, c.padmap[i] = 0;
}

void apply_patches() {
    g_ee_patch[0] = g_cfg.widescreen_hack && g_cfg.aspect != 4;
    g_ee_patch[4] = g_cfg.widescreen_hack && g_cfg.aspect == 3;
}
static std::atomic<float> g_win_aspect{20.0f / 9.0f};
float view_aspect() {
    if (g_cfg.aspect == 3) return 16.0f / 10.0f;
    if (g_cfg.aspect == 4) return std::max(g_win_aspect.load(), 4.0f / 3.0f);
    return 16.0f / 9.0f;
}
float hud_factor() { return 4.0f / 3.0f / view_aspect(); }
bool big_text_active() { return g_cfg.widescreen_hack && g_cfg.big_text == 1; }
float camera_boost() { return touch::camera_boost(); }

float text_scale() {
    if (!big_text_active()) return 1.0f;
    return std::clamp(g_cfg.text_scale, 1.0f, 1.6f);
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

void load_config(const std::string& path) {
    g_cfg_path = path;
    default_input(g_cfg);
    // Android defaults: GPU renderer (Vulkan, gs_vk.cpp; the software renderer if it cannot start),
    // automatic internal resolution
    g_cfg.renderer = 1;
    g_cfg.internal_scale = 0;
    g_cfg.hd_textures = false;
    g_cfg.widescreen_hack = true;  // widescreen view in the shape of the screen, menus in 4:3
    g_cfg.aspect = 4;
    g_cfg.hud_fix = true;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) { save_config(); return; }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string l = trim(line);
        if (l.empty() || l[0] == ';' || l[0] == '#' || l[0] == '[') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        int iv = atoi(v.c_str());
        if (k == "aspect") g_cfg.aspect = iv;
        else if (k == "widescreen_hack") g_cfg.widescreen_hack = iv != 0;
        else if (k == "remove_letterbox") g_cfg.remove_letterbox = iv != 0;
        else if (k == "fps60") g_cfg.fps60 = iv != 0;
        else if (k == "fps60_battle") g_cfg.fps60_battle = iv != 0;
        else if (k == "movie_skip") g_cfg.movie_skip = iv != 0;
        else if (k == "cutscene_skip") g_cfg.cutscene_skip = iv != 0;
        else if (k == "subtitles") g_cfg.subtitles = iv != 0;
        else if (k == "state_slot") g_cfg.state_slot = std::clamp(iv, 0, 9);
        else if (k == "autosave_state") g_cfg.autosave_state = iv != 0;
        else if (k == "contact_battles") g_cfg.contact_battles = iv != 0;
        else if (k == "exp_mult") g_cfg.exp_mult = std::clamp(iv, 1, 4);
        else if (k == "gold_mult") g_cfg.gold_mult = std::clamp(iv, 1, 4);
        else if (k == "jp_voices") g_cfg.jp_voices = iv != 0;
        else if (k == "iso_path_jp") g_cfg.iso_path_jp = v;
        else if (k == "gs_thread") g_cfg.gs_thread = iv != 0;
        else if (k == "language") { g_cfg.language = v; for (char& ch : g_cfg.language) ch = (char)tolower((unsigned char)ch); }
        else if (k == "show_fps") g_cfg.show_fps = iv != 0;
        else if (k == "frame_limit") g_cfg.frame_limit = iv;
        else if (k == "volume") g_cfg.volume = std::clamp(iv, 0, 100);
        else if (k == "volume_music") g_cfg.vol_music = std::clamp(iv, 0, 100);
        else if (k == "volume_voice") g_cfg.vol_voice = std::clamp(iv, 0, 100);
        else if (k == "volume_sfx") g_cfg.vol_sfx = std::clamp(iv, 0, 100);
        else if (k == "volume_movie") g_cfg.vol_movie = std::clamp(iv, 0, 100);
        else if (k == "enabled" || k == "audio_enabled") g_cfg.audio_enabled = iv != 0;
        else if (k == "iso_path") g_cfg.iso_path = v;
        else if (k == "iso_path2") g_cfg.iso_path2 = v;
        else if (k == "disc") g_cfg.disc = iv == 2 ? 2 : 1;
        else if (k == "camera_invert_x") g_cfg.camera_invert_x = iv != 0;
        else if (k == "touch_autohide") g_cfg.touch_autohide = iv != 0;
        else if (k == "gpu_driver") g_gpu_driver = v;
        else if (k == "touch_cam_speed") g_cfg.touch_cam_speed = std::clamp(iv, 1, 15);
        else if (k == "touch_dpad") g_cfg.touch_dpad = iv != 0;
        else if (k == "hud_fix") g_cfg.hud_fix = iv != 0;
        else if (k == "big_text") g_cfg.big_text = iv < 0 ? -1 : (iv ? 1 : 0);
        else if (k == "text_scale") g_cfg.text_scale = std::clamp((float)atof(v.c_str()), 1.0f, 1.6f);
        else if (k == "renderer") g_cfg.renderer = iv ? 1 : 0;
        else if (k == "internal_scale") g_cfg.internal_scale = std::clamp(iv, 0, 4);
        else if (k == "sharp_text") g_cfg.sharp_text = iv != 0;
    }
    fclose(f);
    touch::set_autohide(g_cfg.touch_autohide);
    touch::set_camera_speed(g_cfg.touch_cam_speed);
    touch::set_dpad(g_cfg.touch_dpad);
}

}  // namespace platform
std::string g3_gpu_driver_setting() { return platform::g_gpu_driver; }
void g3_set_gpu_driver_setting(const std::string& name) { platform::g_gpu_driver = name; }
namespace platform {

void save_config() {
    FILE* f = fopen(g_cfg_path.c_str(), "w");
    if (!f) return;
    fprintf(f, "; Grandia III native port configuration (Android test version)\n[Video]\n");
    fprintf(f, "; renderer: 1 = GPU (Vulkan), 0 = software; internal_scale: 0 = automatic, 1-4 = times 512x448\n");
    fprintf(f, "renderer = %d\ninternal_scale = %d\nsharp_text = %d\n", g_cfg.renderer, g_cfg.internal_scale, g_cfg.sharp_text);
    fprintf(f, "; aspect: 0 = 4:3, 1 = 16:9, 4 = shape of the screen (with widescreen_hack)\naspect = %d\n", g_cfg.aspect);
    fprintf(f, "widescreen_hack = %d\nremove_letterbox = %d\n", g_cfg.widescreen_hack, g_cfg.remove_letterbox);
    fprintf(f, "; widescreen: menus and text keep their 4:3 proportions\nhud_fix = %d\n", g_cfg.hud_fix);
    fprintf(f, "; larger conversation windows (widescreen): 0 = off, 1 = on\nbig_text = %d\ntext_scale = %.2f\n", g_cfg.big_text, g_cfg.text_scale);
    fprintf(f, "gs_thread = %d\nlanguage = %s\n", g_cfg.gs_thread, g_cfg.language.c_str());
    fprintf(f, "fps60 = %d\nfps60_battle = %d\n", g_cfg.fps60, g_cfg.fps60_battle);
    fprintf(f, "movie_skip = %d\ncutscene_skip = %d\nsubtitles = %d\nstate_slot = %d\n", g_cfg.movie_skip, g_cfg.cutscene_skip,
            g_cfg.subtitles, g_cfg.state_slot);
    fprintf(f, "show_fps = %d\nframe_limit = %d\n", g_cfg.show_fps, g_cfg.frame_limit);
    fprintf(f, "\n[Audio]\nenabled = %d\nvolume = %d\nvolume_music = %d\nvolume_voice = %d\nvolume_sfx = %d\nvolume_movie = %d\n",
            g_cfg.audio_enabled, g_cfg.volume, g_cfg.vol_music, g_cfg.vol_voice, g_cfg.vol_sfx, g_cfg.vol_movie);
    fprintf(f, "autosave_state = %d\ncontact_battles = %d\njp_voices = %d\n", g_cfg.autosave_state, g_cfg.contact_battles, g_cfg.jp_voices);
    fprintf(f, "; battle rewards multiplied (1-4)\nexp_mult = %d\ngold_mult = %d\n", g_cfg.exp_mult, g_cfg.gold_mult);
    fprintf(f, "\n[Paths]\niso_path = %s\niso_path2 = %s\niso_path_jp = %s\ndisc = %d\n", g_cfg.iso_path.c_str(), g_cfg.iso_path2.c_str(),
            g_cfg.iso_path_jp.c_str(), g_cfg.disc);
    fprintf(f, "\n[Input]\ncamera_invert_x = %d\n", g_cfg.camera_invert_x);
    fprintf(f, "; 1 = the touch controls fade out after a few seconds without a touch (always hidden with a controller)\n");
    fprintf(f, "touch_autohide = %d\n", g_cfg.touch_autohide);
    fprintf(f, "\n[GPU]\n; custom Vulkan driver (folder in files/gpu_drivers, e.g. Turnip); empty = the phone's own driver\ngpu_driver = %s\n",
            g_gpu_driver.c_str());
    fprintf(f, "; camera swipe speed 1-15; 1 = d-pad in the touch controls\ntouch_cam_speed = %d\ntouch_dpad = %d\n", g_cfg.touch_cam_speed,
            g_cfg.touch_dpad);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Notices and overlay (no on-screen overlay yet: logged)
// ---------------------------------------------------------------------------
static bool narrow_line(const std::wstring& w, char* buf, size_t n) { return wcstombs(buf, w.c_str(), n - 1) != (size_t)-1; }
void show_notice(const std::wstring& line1, const std::wstring& line2, double) {
    char a[256] = "", b[256] = "";
    narrow_line(line1, a, sizeof(a));
    narrow_line(line2, b, sizeof(b));
    log_write(LOG_MAIN, "notice: %s / %s", a, b);
}
bool notice_active() { return false; }
// settings menu (menu_android.cpp)
void menu_nav(int k);
void menu_touch(int act, float x, float y);
bool menu_pixels(int ow, int oh, std::vector<u32>& out, int& w, int& h, int& x, int& y, u64& version);
static std::atomic<float> g_ovl_sx{0.5f}, g_ovl_sy{0.5f};  // window pixels -> overlay pixels
// the window's real size (touch coordinates): ANativeWindow_getWidth reports the buffer size once the
// CPU presentation has set its own buffer geometry
static std::atomic<int> g_real_w{0}, g_real_h{0};

// ---------------------------------------------------------------------------
// Video: the software renderer's frames, scaled into the window buffer
// ---------------------------------------------------------------------------
static std::mutex g_frame_mutex;
static std::condition_variable g_frame_cv;
static std::vector<u32> g_frame;
static int g_frame_w = 0, g_frame_h = 0;
static bool g_frame_new = false;
static int g_fps_count = 0;

static double g_shot_interval = 0, g_shot_last = -1e9, g_shot_from = 0, g_shot_until = 1e30;
static std::string g_shot_dir;
static int g_shot_index = 0;

static void write_bmp(const std::string& path, const u32* px, int w, int h, int stride) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    u32 rowsz = w * 4, size = 54 + rowsz * h, off = 54, dib = 40, planes_bpp = 1 | (32 << 16);
    u8 hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &w, 4);
    s32 hh = -h;
    memcpy(hdr + 22, &hh, 4);
    memcpy(hdr + 26, &planes_bpp, 4);
    fwrite(hdr, 1, 54, f);
    std::vector<u32> row(w);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            u32 c = px[(size_t)y * stride + x];
            row[x] = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
        }
        fwrite(row.data(), 4, w, f);
    }
    fclose(f);
}

void set_screenshot_interval(double sec, const std::string& dir) {
    g_shot_interval = sec;
    g_shot_dir = dir;
    mkdir(dir.c_str(), 0777);
}
void set_screenshot_window(double from, double until) { g_shot_from = from, g_shot_until = until; }
void set_gpu_present(bool) {}
bool screenshot_due() { return false; }
void request_final_shot() {}
bool shots_enabled() { return g_shot_interval > 0; }
void save_screenshot(const u32* pixels, int width, int height, int stride) {
    write_bmp(g_shot_dir + "/shot_final.bmp", pixels, width, height, stride);
}

bool init_video() {
    std::unique_lock<std::mutex> lk(g_win_mutex);
    g_win_cv.wait(lk, [] { return g_window != nullptr || g_quit.load(); });
    return !g_quit;
}

void run_message_loop() {
    // the activity's looper runs in android_main; this thread only waits for the end
    while (!g_quit) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
bool window_closed() { return g_quit; }

// --shots=SEC: is an interval screenshot due now (then the caller passes the picture to write_shot)
bool interval_shot_due() {
    if (g_shot_interval <= 0) return false;
    double t = host_time_sec();
    if (t >= g_shot_from && t < g_shot_until && t - g_shot_last >= g_shot_interval) {
        g_shot_last = t;
        return true;
    }
    return false;
}
void write_shot(const u32* pixels, int width, int height, int stride) {
    char name[64];
    snprintf(name, sizeof(name), "/shot_%04d.bmp", g_shot_index++);
    write_bmp(g_shot_dir + name, pixels, width, height, stride);
}

void submit_frame(const u32* pixels, int width, int height, int stride) {
    if (interval_shot_due()) write_shot(pixels, width, height, stride);
    {
        std::lock_guard<std::mutex> lk(g_frame_mutex);
        g_frame.resize((size_t)width * height);
        for (int y = 0; y < height; y++) memcpy(&g_frame[(size_t)y * width], pixels + (size_t)y * stride, width * 4);
        g_frame_w = width;
        g_frame_h = height;
        g_frame_new = true;
        g_fps_count++;
    }
    g_frame_cv.notify_one();
}

// Present thread: the window buffer has the PS2 frame height and the screen's shape; the frame is
// scaled horizontally to its display aspect (4:3, or the widescreen view) and centered. The system
// compositor scales that buffer to the screen.
// letterbox bars of a movie frame: black rows at the top and the bottom; over a movie the smallest
// counts win (fades and dark scenes only make them larger), fully black frames are ignored
static void movie_bars(const u32* px, int w, int h, int* top, int* bottom) {
    auto black_row = [&](int y) {
        const u32* r = px + (size_t)y * w;
        for (int x = 0; x < w; x += 2)
            if ((r[x] & 0xFF) > 24 || ((r[x] >> 8) & 0xFF) > 24 || ((r[x] >> 16) & 0xFF) > 24) return false;
        return true;
    };
    int t = 0, b = 0;
    while (t < h && black_row(t)) t++;
    if (t >= h - 16) return;
    while (b < h - t && black_row(h - 1 - b)) b++;
    *top = *top < 0 ? t : std::min(*top, t);
    *bottom = *bottom < 0 ? b : std::min(*bottom, b);
}

// The overlay for the GPU presentation (premultiplied RGBA, ow x oh = half the window): the touch
// controls and the subtitle line (subtitles.cpp; drawn for a picture of pic_w x oh at pic_x0).
// Rebuilt only when one of them changes; true when ovl is new.
struct Overlay {
    std::vector<u32> px;
    u64 touch_v = ~0ull, sub_v = 0;
    int w = 0, h = 0, pic_x0 = -1;
    bool sub_shown = false;
    std::vector<u32> sub;
    bool menu_shown = false;
    u64 menu_v = 0;
    std::vector<u32> menu;
};
static bool overlay_update(Overlay& o, int ow, int oh, int pic_x0, int pic_w) {
    u64 tv = touch::version();
    u64 sv = o.sub_v;
    int sw = 0, sh = 0;
    bool sub = g_cfg.subtitles && !skip_active() && subs::pixels(pic_w, oh, o.sub, sw, sh, sv) && sw > 0 && sh > 0 &&
               sw <= ow && sh <= oh;
    u64 mv = o.menu_v;
    int mw = 0, mh = 0, mx = 0, my = 0;
    bool menu = menu_pixels(ow, oh, o.menu, mw, mh, mx, my, mv) && mw > 0 && mh > 0 && mw <= ow && mh <= oh;
    if (tv == o.touch_v && ow == o.w && oh == o.h && pic_x0 == o.pic_x0 && sub == o.sub_shown && (!sub || sv == o.sub_v) &&
        menu == o.menu_shown && (!menu || mv == o.menu_v))
        return false;
    o.menu_shown = menu;
    o.menu_v = mv;
    o.touch_v = tv;
    o.sub_v = sv;
    o.w = ow;
    o.h = oh;
    o.pic_x0 = pic_x0;
    o.sub_shown = sub;
    o.px.assign((size_t)ow * oh, 0);
    if (!menu) touch::draw(o.px.data(), ow);
    if (sub) {
        // centred near the bottom of the game picture, under the touch controls (as on the PC)
        int x0 = std::clamp(pic_x0 + (pic_w - sw) / 2, 0, ow - sw), y0 = std::clamp((int)(oh * (touch::visible() ? 0.905f : 0.955f)) - sh, 0, oh - sh);  // (above SELECT/START)
        for (int y = 0; y < sh; y++) {
            u32* d = o.px.data() + (size_t)(y0 + y) * ow + x0;
            const u32* s = o.sub.data() + (size_t)y * sw;
            for (int x = 0; x < sw; x++) {
                u32 a = s[x] >> 24;
                if (!a) continue;
                u32 g = (s[x] & 0xFF) * a / 255;  // (gray: the channel order does not matter)
                u32 dd = d[x], inv = 255 - a;
                u32 r = g + ((dd & 0xFF) * inv) / 255, gg = g + (((dd >> 8) & 0xFF) * inv) / 255;
                u32 bb = g + (((dd >> 16) & 0xFF) * inv) / 255, aa = a + ((dd >> 24) * inv) / 255;
                d[x] = r | gg << 8 | bb << 16 | aa << 24;
            }
        }
    }
    if (menu) {
        // the menu panel on top (premultiplied already)
        mx = std::clamp(mx, 0, ow - mw), my = std::clamp(my, 0, oh - mh);
        for (int y = 0; y < mh; y++) {
            u32* d = o.px.data() + (size_t)(my + y) * ow + mx;
            const u32* s = o.menu.data() + (size_t)y * mw;
            for (int x = 0; x < mw; x++) {
                u32 a = s[x] >> 24, inv = 255 - a, dd = d[x];
                u32 r = (s[x] & 0xFF) + ((dd & 0xFF) * inv) / 255, g = ((s[x] >> 8) & 0xFF) + (((dd >> 8) & 0xFF) * inv) / 255;
                u32 b = ((s[x] >> 16) & 0xFF) + (((dd >> 16) & 0xFF) * inv) / 255, al = a + ((dd >> 24) * inv) / 255;
                d[x] = std::min(r, 255u) | std::min(g, 255u) << 8 | std::min(b, 255u) << 16 | std::min(al, 255u) << 24;
            }
        }
    }
    return true;
}

// GPU renderer picture (gs_vk.cpp, on the renderer thread): fw x fh RGBA8 image; px: a CPU copy of a
// recent picture of the same size for the movie bar detection (or null)
static int g_gpu_movie_top = -1, g_gpu_movie_bottom = -1;
bool present_gpu(VkImageView view, int fw, int fh, const u32* px) {
    static Overlay ovl;
    if (g_overlay_test_at > 0 && host_time_sec() >= g_overlay_test_at) {
        // --overlay-test=SEC[,N]: open the menu, N rows down, change that row to the left (tests)
        g_overlay_test_at = 0;
        overlay_set_open(true);
        for (int k = 0; k < g_overlay_test_downs; k++) menu_nav(1);
        if (g_overlay_test_downs > 0) menu_nav(2);  // (N = 0: only open it)
    }
    {
        std::lock_guard<std::mutex> lk(g_frame_mutex);
        g_fps_count++;
    }
    std::lock_guard<std::mutex> wl(g_win_mutex);
    if (!g_window || !vkc::ready() || fw <= 0 || fh <= 0) return false;
    int ww = ANativeWindow_getWidth(g_window), wh = ANativeWindow_getHeight(g_window);
    if (ww <= 0 || wh <= 0) return false;
    g_win_aspect = (float)ww / wh;
    bool movie = kernel::movie_running();
    int sy0 = 0, sh = fh;
    float aspect = g_cfg.widescreen_hack ? view_aspect() : 4.0f / 3.0f;
    if (movie) {
        if (px) movie_bars(px, fw, fh, &g_gpu_movie_top, &g_gpu_movie_bottom);
        if (g_gpu_movie_top >= 0) {
            sy0 = g_gpu_movie_top;
            sh = std::max(fh / 2, fh - g_gpu_movie_top - g_gpu_movie_bottom);
        }
        aspect = 4.0f / 3.0f * fh / sh;
    } else {
        g_gpu_movie_top = g_gpu_movie_bottom = -1;
    }
    int dw = std::min(ww, (int)lroundf(wh * aspect));
    int ow = ww / 2, oh = wh / 2;
    touch::layout(ow, oh, ww, wh);
    g_ovl_sx = (float)ow / ww;
    g_ovl_sy = (float)oh / wh;
    bool ovl_new = overlay_update(ovl, ow, oh, (ww - dw) / 4, dw / 2);
    return vkc::present_image(view, fw, fh, sy0, sh, (ww - dw) / 2, 0, (ww - dw) / 2 + dw, wh, ovl.px.data(), ow, oh, ovl_new);
}

static void present_thread() {
    std::vector<u32> frame;
    int movie_top = -1, movie_bottom = -1;
    Overlay ovl;  // touch controls + subtitles for the GPU presentation
    int fw = 0, fh = 0, buf_w = 0, buf_h = 0;
    double fps_t0 = -1;  // host_time_sec() works once the game has started its clock
    while (!g_quit) {
        {
            std::unique_lock<std::mutex> lk(g_frame_mutex);
            // (the open menu pauses the game: no new frames, but the CPU presentation must still redraw it)
            bool menu = overlay_open() && !vkc::ready();
            g_frame_cv.wait_for(lk, std::chrono::milliseconds(menu ? 33 : 200), [] { return g_frame_new || g_quit.load(); });
            double now = host_time_sec();
            if (fps_t0 < 0 || !std::isfinite(fps_t0) || !std::isfinite(now)) fps_t0 = now;
            else if (now - fps_t0 >= 5.0) {
                log_write(LOG_MAIN, "fps: %.1f", g_fps_count / (now - fps_t0));
                g_fps_count = 0;
                fps_t0 = now;
            }
            if (g_frame_new) {
                g_frame_new = false;
                frame.swap(g_frame);
                fw = g_frame_w;
                fh = g_frame_h;
            } else if (!menu || frame.empty()) {
                continue;
            }  // else: the last picture again with the menu on top
        }
        std::lock_guard<std::mutex> wl(g_win_mutex);
        if (!g_window || fw <= 0 || fh <= 0) continue;
        int ww = ANativeWindow_getWidth(g_window), wh = ANativeWindow_getHeight(g_window);
        if (ww <= 0 || wh <= 0) continue;
        g_win_aspect = (float)ww / wh;
        if (vkc::ready()) {
            // GPU presentation: the picture scaled in both directions, the overlay at half the window size
            bool movie = kernel::movie_running();
            int sy0 = 0, sh = fh;
            float aspect = g_cfg.widescreen_hack ? view_aspect() : 4.0f / 3.0f;
            if (movie) {
                movie_bars(frame.data(), fw, fh, &movie_top, &movie_bottom);
                if (movie_top >= 0) {
                    sy0 = movie_top;
                    sh = std::max(fh / 2, fh - movie_top - movie_bottom);
                }
                aspect = 4.0f / 3.0f * fh / sh;
            } else {
                movie_top = movie_bottom = -1;
            }
            int dw = std::min(ww, (int)lroundf(wh * aspect));
            int ow = ww / 2, oh = wh / 2;
            touch::layout(ow, oh, ww, wh);
            g_ovl_sx = (float)ow / ww;
            g_ovl_sy = (float)oh / wh;
            bool ovl_new = overlay_update(ovl, ow, oh, (ww - dw) / 4, dw / 2);
            vkc::present(frame.data(), fw, fh, fw, sy0, sh, (ww - dw) / 2, 0, (ww - dw) / 2 + dw, wh, ovl.px.data(), ow, oh, ovl_new);
            continue;
        }
        int bh = fh, bw = (int)lroundf(fh * (float)ww / wh);
        if (bw != buf_w || bh != buf_h) {
            ANativeWindow_setBuffersGeometry(g_window, bw, bh, WINDOW_FORMAT_RGBX_8888);
            buf_w = bw;
            buf_h = bh;
            log_write(LOG_MAIN, "window %dx%d, buffer %dx%d", ww, wh, bw, bh);
        }
        ANativeWindow_Buffer b;
        if (ANativeWindow_lock(g_window, &b, nullptr) != 0) continue;
        // movies (FMV): their own black bars are cut off and the picture fills the screen height in its
        // proportions (the frame is 4:3); the widescreen view is for the 3D scenes
        bool movie = kernel::movie_running();
        int src_y0 = 0, src_h = fh;
        float aspect = g_cfg.widescreen_hack ? view_aspect() : 4.0f / 3.0f;
        if (movie) {
            movie_bars(frame.data(), fw, fh, &movie_top, &movie_bottom);
            if (movie_top >= 0) {
                src_y0 = movie_top;
                src_h = std::max(fh / 2, fh - movie_top - movie_bottom);
            }
            aspect = 4.0f / 3.0f * fh / src_h;
        } else {
            movie_top = movie_bottom = -1;
        }
        int dw = std::min(b.width, (int)lroundf(b.height * aspect));
        int x0 = (b.width - dw) / 2;
        u32* dst = (u32*)b.bits;
        for (int y = 0; y < b.height && y < fh; y++) {
            u32* d = dst + (size_t)y * b.stride;
            const u32* s = frame.data() + (size_t)(src_y0 + std::min(src_h - 1, (int)((y + 0.5f) * src_h / b.height))) * fw;
            std::fill(d, d + x0, 0xFF000000u);
            // linear filter between neighbouring source pixels (fixed point 16.16)
            u32 step = (u32)(((u64)fw << 16) / (u32)dw), pos = step / 2 > 0x8000 ? step / 2 - 0x8000 : 0;
            for (int x = 0; x < dw; x++, pos += step) {
                u32 i = pos >> 16, fr = (pos >> 8) & 0xFF;
                u32 a = s[std::min<u32>(i, fw - 1)], c = s[std::min<u32>(i + 1, fw - 1)];
                u32 rb = ((a & 0x00FF00FF) * (256 - fr) + (c & 0x00FF00FF) * fr) >> 8;
                u32 g = ((a & 0x0000FF00) * (256 - fr) + (c & 0x0000FF00) * fr) >> 8;
                d[x0 + x] = 0xFF000000u | (rb & 0x00FF00FF) | (g & 0x0000FF00);
            }
            std::fill(d + x0 + dw, d + b.width, 0xFF000000u);
        }
        if (g_overlay_test_at > 0 && host_time_sec() >= g_overlay_test_at) {
            // --overlay-test=SEC[,N] in the CPU presentation: open the menu, N rows down (no change)
            g_overlay_test_at = 0;
            overlay_set_open(true);
            for (int k = 0; k < g_overlay_test_downs; k++) menu_nav(1);
        }
        // CPU presentation (no Vulkan): touch controls, menu and subtitles at buffer size, blended on top
        // (touch coordinates are window pixels: the real window size, not the buffer's)
        int rw = g_real_w > 0 ? g_real_w.load() : ww, rh = g_real_h > 0 ? g_real_h.load() : wh;
        touch::layout(b.width, b.height, rw, rh);
        g_ovl_sx = (float)b.width / rw;
        g_ovl_sy = (float)b.height / rh;
        overlay_update(ovl, b.width, b.height, x0, dw);
        if ((int)ovl.px.size() >= b.width * b.height)
            for (int y = 0; y < b.height; y++) {
                u32* d = dst + (size_t)y * b.stride;
                const u32* o = ovl.px.data() + (size_t)y * b.width;
                for (int x = 0; x < b.width; x++) {
                    u32 s = o[x], a = s >> 24;
                    if (!a) continue;
                    u32 na = 255 - a, dd = d[x];  // premultiplied "over"
                    u32 r = (s & 0xFF) + ((dd & 0xFF) * na) / 255, g = ((s >> 8) & 0xFF) + (((dd >> 8) & 0xFF) * na) / 255;
                    u32 bl = ((s >> 16) & 0xFF) + (((dd >> 16) & 0xFF) * na) / 255;
                    d[x] = 0xFF000000u | std::min(255u, bl) << 16 | std::min(255u, g) << 8 | std::min(255u, r);
                }
            }
        ANativeWindow_unlockAndPost(g_window);
    }
}

// ---------------------------------------------------------------------------
// Input: game controller state from the activity's input queue, autopress (tests), cutscene skip
// ---------------------------------------------------------------------------
static std::atomic<u32> g_btn{0};          // libpad bit order
static std::atomic<int> g_axis[6];         // lx ly rx ry l2 r2, -32768..32767 (triggers 0..32767)
static std::atomic<int> g_hat_x{0}, g_hat_y{0};

struct AutoPress { double t; u32 mask; double dur; };
static std::vector<AutoPress> g_autopress;
void add_autopress(const std::string& spec) {
    static const char* NAMES[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                    "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            std::string n = item.substr(0, colon);
            double t = atof(item.c_str() + colon + 1);
            size_t colon2 = item.find(':', colon + 1);
            double dur = colon2 != std::string::npos ? atof(item.c_str() + colon2 + 1) : 0.25;
            for (int i = 0; i < 16; i++)
                if (n == NAMES[i]) g_autopress.push_back({t, 1u << i, dur});
            static const char* STICK[6] = {"lup", "lright", "ldown", "lleft", "rright", "rleft"};
            for (int i = 0; i < 6; i++)
                if (n == STICK[i]) g_autopress.push_back({t, 1u << (16 + i), dur});
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

void set_vibration(float, float) {}

static std::atomic<bool> g_skip{false};
static double g_skip_since = 0;
static int g_skip_session = 0;
bool skip_active() { return g_skip.load(std::memory_order_relaxed); }
int skip_session() { return g_skip_session; }
void skip_stop(const char* why) {
    if (!g_skip.exchange(false)) return;
    log_write(LOG_MAIN, "skip: end (%s) after %.1f s", why, host_time_sec() - g_skip_since);
}
static void skip_start() {
    g_skip_since = host_time_sec();
    g_skip_session++;
    g_skip = true;
    log_write(LOG_MAIN, "skip: start");
}
}  // namespace platform
extern "C" int g_cinema;
extern "C" int game_flag(int n);
namespace platform {

static std::atomic<bool> g_ff{false};
bool fast_forward() { return g_ff.load(std::memory_order_relaxed); }

PadState read_pad(int port) {
    PadState s{};
    s.lx = s.ly = s.rx = s.ry = 0x80;
    if (port != 0) return s;
    if (!g_ram_dump_at.empty() && host_time_sec() >= g_ram_dump_at.front()) {
        // --ramdump=SEC[,SEC...]: EE main memory snapshots next to the ini (analysis, as on the PC)
        char name[64];
        snprintf(name, sizeof name, "/ram_%03d.bin", (int)g_ram_dump_at.front());
        std::string d = g_cfg_path.substr(0, g_cfg_path.find_last_of("/\\"));
        if (FILE* f = fopen((d + name).c_str(), "wb")) {
            fwrite(mem::ram, 1, 32u << 20, f);
            fclose(f);
            log_write(LOG_MAIN, "ram dump %s", name);
        }
        g_ram_dump_at.erase(g_ram_dump_at.begin());
    }
    u16 b = (u16)(g_btn.load(std::memory_order_relaxed) | touch::buttons());
    if (g_hat_x < 0) b |= 0x0080;
    if (g_hat_x > 0) b |= 0x0020;
    if (g_hat_y < 0) b |= 0x0010;
    if (g_hat_y > 0) b |= 0x0040;
    if (g_axis[4] > 8000) b |= 0x0100;
    if (g_axis[5] > 8000) b |= 0x0200;
    // SELECT+R2 held: fast forward (as Back+RT on the PC); the combination is not passed to the game
    g_ff = (b & 0x0201) == 0x0201;
    if (g_ff) b &= ~0x0201;
    auto ax = [](int v) { return (u8)std::clamp(128 + v / 256, 0, 255); };
    s.lx = ax(g_axis[0]);
    s.ly = ax(g_axis[1]);
    int tx, ty;
    if (touch::stick(&tx, &ty)) s.lx = ax(tx), s.ly = ax(ty);
    s.rx = ax(g_axis[2]);
    s.ry = ax(g_axis[3]);
    int crx;
    if (touch::camera(&crx) && crx) s.rx = ax(crx);  // swipe on the screen: camera
    u32 auto_stick = 0;
    double now = host_time_sec();
    for (auto& a : g_autopress)
        if (now >= a.t && now < a.t + a.dur) {
            b |= (u16)a.mask;
            auto_stick |= a.mask >> 16;
        }
    if (auto_stick & 15) {
        s.lx = (auto_stick & 2) ? 0xFF : (auto_stick & 8) ? 0x00 : 0x80;
        s.ly = (auto_stick & 4) ? 0xFF : (auto_stick & 1) ? 0x00 : 0x80;
    }
    if (auto_stick & 48) s.rx = (auto_stick & 16) ? 0xFF : 0x00;
    if (g_cfg.camera_invert_x) s.rx = (u8)std::min(255, 256 - s.rx);
    {
        // START during a cutscene skips it (same logic as on the PC, platform.cpp)
        static bool start_was = true;
        static double select_until = 0;
        bool start_now = (b & 0x0008) != 0, edge = start_now && !start_was;
        start_was = start_now;
        if (now < select_until) {
            b = (b & ~0x0008) | 0x0001;
        } else if (skip_active()) {
            if (edge) skip_stop("cancelled");
            double t = now - g_skip_since;
            b = fmod(t, 0.15) < 0.05 ? 0x4000 : 0;
            s.lx = s.ly = s.rx = s.ry = 0x80;
            if (!skip_active()) b = 0;
        } else if (edge && g_cfg.cutscene_skip && (g_cinema || kernel::flight_running()) && !kernel::battle_running() &&
                   !kernel::movie_running()) {
            b &= ~0x0008;
            if (!game_flag(445)) {
                select_until = now + 0.12;
                b |= 0x0001;
                log_write(LOG_MAIN, "skip: scene skip (SELECT)");
            } else {
                skip_start();
            }
        }
    }
    s.buttons = b;
    static const u16 pbits[12] = {0x0020, 0x0080, 0x0010, 0x0040, 0x1000, 0x2000, 0x4000, 0x8000, 0x0400, 0x0800, 0x0100, 0x0200};
    for (int i = 0; i < 12; i++) s.pressure[i] = (b & pbits[i]) ? 0xFF : 0;
    return s;
}

}  // namespace platform

// Android key codes -> libpad bits (SELECT L3 R3 START UP RIGHT DOWN LEFT L2 R2 L1 R1 TRI CIRCLE CROSS SQUARE)
static u32 pad_bit(int32_t key) {
    switch (key) {
    case AKEYCODE_BUTTON_SELECT: return 0x0001;
    case AKEYCODE_BUTTON_THUMBL: return 0x0002;
    case AKEYCODE_BUTTON_THUMBR: return 0x0004;
    case AKEYCODE_BUTTON_START: return 0x0008;
    case AKEYCODE_DPAD_UP: return 0x0010;
    case AKEYCODE_DPAD_RIGHT: return 0x0020;
    case AKEYCODE_DPAD_DOWN: return 0x0040;
    case AKEYCODE_DPAD_LEFT: return 0x0080;
    case AKEYCODE_BUTTON_L2: return 0x0100;
    case AKEYCODE_BUTTON_R2: return 0x0200;
    case AKEYCODE_BUTTON_L1: return 0x0400;
    case AKEYCODE_BUTTON_R1: return 0x0800;
    case AKEYCODE_BUTTON_Y: return 0x1000;  // top: triangle
    case AKEYCODE_BUTTON_B: return 0x2000;  // right: circle
    case AKEYCODE_BUTTON_A: return 0x4000;  // bottom: cross
    case AKEYCODE_BUTTON_X: return 0x8000;  // left: square
    default: return 0;
    }
}

// the settings menu: opened by SELECT+START, the home/mode button or the touch menu button; while it
// is open the controller and the touch screen operate it and the game sees no input
static void open_menu() {
    platform::g_btn = 0;
    touch::release_all();
    platform::overlay_set_open(true);
}
static int32_t menu_input(AInputEvent* e, int32_t type) {
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t key = AKeyEvent_getKeyCode(e), act = AKeyEvent_getAction(e);
        if (act != AKEY_EVENT_ACTION_DOWN) return 1;  // (key repeats arrive as further downs)
        switch (key) {
        case AKEYCODE_DPAD_UP: platform::menu_nav(0); break;
        case AKEYCODE_DPAD_DOWN: platform::menu_nav(1); break;
        case AKEYCODE_DPAD_LEFT: platform::menu_nav(2); break;
        case AKEYCODE_DPAD_RIGHT: platform::menu_nav(3); break;
        case AKEYCODE_BUTTON_L1: platform::menu_nav(6); break;   // previous tab
        case AKEYCODE_BUTTON_R1: platform::menu_nav(7); break;   // next tab
        case AKEYCODE_BUTTON_A: case AKEYCODE_DPAD_CENTER: case AKEYCODE_ENTER: platform::menu_nav(4); break;
        case AKEYCODE_BUTTON_B: case AKEYCODE_BACK: case AKEYCODE_BUTTON_START: case AKEYCODE_BUTTON_MODE:
        case AKEYCODE_BUTTON_SELECT: case AKEYCODE_ESCAPE: platform::menu_nav(5); break;
        }
        return 1;
    }
    int32_t src = AInputEvent_getSource(e);
    if ((src & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) {
        int32_t a = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
        float x = AMotionEvent_getX(e, 0) * platform::g_ovl_sx, y = AMotionEvent_getY(e, 0) * platform::g_ovl_sy;
        if (a == AMOTION_EVENT_ACTION_DOWN) platform::menu_touch(0, x, y);
        else if (a == AMOTION_EVENT_ACTION_MOVE) platform::menu_touch(1, x, y);
        else if (a == AMOTION_EVENT_ACTION_UP) platform::menu_touch(2, x, y);
        else if (a == AMOTION_EVENT_ACTION_CANCEL) platform::menu_touch(3, x, y);
        return 1;
    }
    if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        // hat (d-pad on many controllers) and left stick: one step per push
        static int last_x = 0, last_y = 0;
        auto v = [e](int32_t axis) { return AMotionEvent_getAxisValue(e, axis, 0); };
        float hx = v(AMOTION_EVENT_AXIS_HAT_X), hy = v(AMOTION_EVENT_AXIS_HAT_Y), sx = v(AMOTION_EVENT_AXIS_X), sy = v(AMOTION_EVENT_AXIS_Y);
        int dx = (hx < -0.5f || sx < -0.6f) ? -1 : (hx > 0.5f || sx > 0.6f) ? 1 : 0;
        int dy = (hy < -0.5f || sy < -0.6f) ? -1 : (hy > 0.5f || sy > 0.6f) ? 1 : 0;
        if (dy != last_y && dy) platform::menu_nav(dy < 0 ? 0 : 1);
        if (dx != last_x && dx) platform::menu_nav(dx < 0 ? 2 : 3);
        last_x = dx, last_y = dy;
        return 1;
    }
    return 0;
}

static int32_t on_input(android_app*, AInputEvent* e) {
    int32_t type = AInputEvent_getType(e);
    if (platform::overlay_open()) return menu_input(e, type);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t key = AKeyEvent_getKeyCode(e), act = AKeyEvent_getAction(e);
        if (key == AKEYCODE_BUTTON_MODE) {
            if (act == AKEY_EVENT_ACTION_DOWN) open_menu();
            return 1;
        }
        u32 bit = pad_bit(key);
        if (!bit) return key == AKEYCODE_BACK ? 1 : 0;  // BACK (B on some controllers) must not close the game
        touch::controller_used();
        if (act == AKEY_EVENT_ACTION_DOWN) {
            u32 before = platform::g_btn.fetch_or(bit);
            // SELECT+START together: the settings menu
            if ((bit == 0x0001 && (before & 0x0008)) || (bit == 0x0008 && (before & 0x0001))) open_menu();
        } else if (act == AKEY_EVENT_ACTION_UP) platform::g_btn.fetch_and(~bit);
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && touch::on_motion(e)) {
        if (touch::take_menu_tap()) open_menu();
        // SELECT+START on the touch pad: the settings menu (as on a controller)
        static bool both_was = false;
        bool both = (touch::buttons() & 0x0009) == 0x0009;
        if (both && !both_was) open_menu();
        both_was = both;
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (AInputEvent_getSource(e) & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        touch::controller_used();
        auto v = [e](int32_t axis) { return AMotionEvent_getAxisValue(e, axis, 0); };
        auto st = [](float f) { return (int)std::clamp(f * 32767.0f, -32768.0f, 32767.0f); };
        platform::g_axis[0] = st(v(AMOTION_EVENT_AXIS_X));
        platform::g_axis[1] = st(v(AMOTION_EVENT_AXIS_Y));
        platform::g_axis[2] = st(v(AMOTION_EVENT_AXIS_Z));
        platform::g_axis[3] = st(v(AMOTION_EVENT_AXIS_RZ));
        platform::g_axis[4] = st(std::max(v(AMOTION_EVENT_AXIS_LTRIGGER), v(AMOTION_EVENT_AXIS_BRAKE)));
        platform::g_axis[5] = st(std::max(v(AMOTION_EVENT_AXIS_RTRIGGER), v(AMOTION_EVENT_AXIS_GAS)));
        float hx = v(AMOTION_EVENT_AXIS_HAT_X), hy = v(AMOTION_EVENT_AXIS_HAT_Y);
        platform::g_hat_x = hx < -0.5f ? -1 : hx > 0.5f ? 1 : 0;
        platform::g_hat_y = hy < -0.5f ? -1 : hy > 0.5f ? 1 : 0;
        return 1;
    }
    return 0;
}

// ===========================================================================
// audio: the same ring buffer and rate correction as platform/audio.cpp, AAudio as the device
// ===========================================================================
namespace audio {

static constexpr u32 SRC_RATE = 48000;
static constexpr u32 RING_FRAMES = 1u << 15;
static constexpr u32 RING_MASK = RING_FRAMES - 1;
static constexpr u32 TARGET_FRAMES = SRC_RATE * 60 / 1000;  // a little more than on the PC
static constexpr u32 MAX_FRAMES = TARGET_FRAMES * 3;

static s16 g_ring[RING_FRAMES * 2];
static std::atomic<u32> g_w{0}, g_r{0};
static AAudioStream* g_stream = nullptr;
static u32 g_dev_rate = SRC_RATE;

void set_wav_path(const std::string&) {}

void push(const s16* frames, int n) {
    u32 w = g_w.load(std::memory_order_relaxed);
    u32 r = g_r.load(std::memory_order_acquire);
    u32 space = RING_FRAMES - (w - r);
    if ((u32)n > space) n = (int)space;
    for (int i = 0; i < n; i++) {
        u32 idx = (w + (u32)i) & RING_MASK;
        g_ring[idx * 2] = frames[i * 2];
        g_ring[idx * 2 + 1] = frames[i * 2 + 1];
    }
    g_w.store(w + (u32)n, std::memory_order_release);
}

struct Consumer {
    bool playing = false;
    double frac = 0, fill_avg = TARGET_FRAMES;
    float last_l = 0, last_r = 0;
};
static Consumer g_cons;

static void consume(float* out, u32 n, u32 dev_rate, float volume) {
    Consumer& c = g_cons;
    u32 r = g_r.load(std::memory_order_relaxed);
    u32 w = g_w.load(std::memory_order_acquire);
    u32 avail = w - r;
    if (!c.playing) {
        if (avail < TARGET_FRAMES) {
            memset(out, 0, n * 2 * sizeof(float));
            return;
        }
        c.playing = true;
        c.fill_avg = avail;
        c.frac = 0;
    }
    if (avail > MAX_FRAMES) {
        r += avail - TARGET_FRAMES;
        avail = TARGET_FRAMES;
        c.fill_avg = avail;
    }
    c.fill_avg += (avail - c.fill_avg) * 0.02;
    double err = (c.fill_avg - TARGET_FRAMES) / TARGET_FRAMES;
    double adj = std::max(-0.005, std::min(0.005, err * 0.01));
    double ratio = (double)SRC_RATE / dev_rate * (1.0 + adj);
    const float scale = volume / 32768.0f;
    u32 i = 0;
    for (; i < n; i++) {
        if (avail < 2) break;
        u32 a = r & RING_MASK, b = (r + 1) & RING_MASK;
        float t = (float)c.frac;
        out[i * 2] = (g_ring[a * 2] + (g_ring[b * 2] - g_ring[a * 2]) * t) * scale;
        out[i * 2 + 1] = (g_ring[a * 2 + 1] + (g_ring[b * 2 + 1] - g_ring[a * 2 + 1]) * t) * scale;
        c.frac += ratio;
        while (c.frac >= 1.0) {
            c.frac -= 1.0;
            r++;
            avail--;
        }
    }
    if (i < n) {
        float l = i ? out[(i - 1) * 2] : c.last_l, rr = i ? out[(i - 1) * 2 + 1] : c.last_r;
        for (; i < n; i++) {
            l *= 0.95f;
            rr *= 0.95f;
            out[i * 2] = l;
            out[i * 2 + 1] = rr;
        }
        c.playing = false;
    }
    c.last_l = out[(n - 1) * 2];
    c.last_r = out[(n - 1) * 2 + 1];
    g_r.store(r, std::memory_order_release);
}

static aaudio_data_callback_result_t data_cb(AAudioStream*, void*, void* data, int32_t n) {
    const platform::Config& cfg = platform::config();
    float vol = cfg.audio_enabled ? cfg.volume / 100.0f : 0.0f;
    if (kernel::g_pause_req.load(std::memory_order_relaxed)) vol = 0;
    consume((float*)data, (u32)n, g_dev_rate, vol);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void init() {
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, SRC_RATE);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(b, data_cb, nullptr);
    aaudio_result_t res = AAudioStreamBuilder_openStream(b, &g_stream);
    AAudioStreamBuilder_delete(b);
    if (res != AAUDIO_OK) {
        log_write(LOG_MAIN, "audio: AAudio open failed (%d)", res);
        g_stream = nullptr;
        return;
    }
    g_dev_rate = (u32)AAudioStream_getSampleRate(g_stream);
    AAudioStream_requestStart(g_stream);
    log_write(LOG_MAIN, "audio: AAudio %u Hz, burst %d frames", g_dev_rate, AAudioStream_getFramesPerBurst(g_stream));
}

}  // namespace audio



// ===========================================================================
// NativeActivity entry
// ===========================================================================
static void on_cmd(android_app* app, int32_t cmd) {
    static const char* NAMES[] = {"input changed", "init window", "term window", "window resized", "window redraw",
                                  "content rect", "gained focus", "lost focus", "config changed", "low memory",
                                  "start", "resume", "save state", "pause", "stop", "destroy"};
    ALOG("activity: %s", cmd >= 0 && cmd < 16 ? NAMES[cmd] : "?");
    log_write(LOG_MAIN, "activity: %s", cmd >= 0 && cmd < 16 ? NAMES[cmd] : "?");
    switch (cmd) {
    case APP_CMD_INIT_WINDOW: {
        std::lock_guard<std::mutex> lk(g_win_mutex);
        g_window = app->window;
        platform::g_real_w = ANativeWindow_getWidth(g_window);
        platform::g_real_h = ANativeWindow_getHeight(g_window);
        log_write(LOG_MAIN, "window %dx%d", platform::g_real_w.load(), platform::g_real_h.load());
        if (getenv("GRANDIA_NO_VULKAN")) log_write(LOG_MAIN, "vulkan: off (test switch), CPU presentation");  // test: phones without Vulkan
        else if (vkc::init() && !vkc::set_window(g_window)) log_write(LOG_MAIN, "vulkan: no swapchain, CPU presentation");
        g_win_cv.notify_all();
        break;
    }
    case APP_CMD_TERM_WINDOW: {
        std::lock_guard<std::mutex> lk(g_win_mutex);
        vkc::set_window(nullptr);
        g_window = nullptr;
        break;
    }
    case APP_CMD_PAUSE: kernel::g_pause_req = true; break;   // in the background the game waits
    case APP_CMD_RESUME: kernel::g_pause_req = platform::overlay_open(); break;  // (the menu keeps the game paused)
    default: break;
    }
}

static std::vector<std::string> read_args(const std::string& path) {
    // one command line option per line (tests): e.g. --press=start:20
    std::vector<std::string> a;
    if (FILE* f = fopen(path.c_str(), "r")) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            std::string s = platform::trim(line);
            if (!s.empty() && s[0] != '#') a.push_back(s);
        }
        fclose(f);
    }
    return a;
}

// lang/ (translations, subtitles) ships in the APK (tools/build_android.py): copied into the app folder
// when the manifest's hash differs from the copy there (a new APK); files added by hand stay
static std::string asset_text(AAssetManager* am, const char* name) {
    AAsset* a = AAssetManager_open(am, name, AASSET_MODE_BUFFER);
    if (!a) return "";
    std::string s((const char*)AAsset_getBuffer(a), (size_t)AAsset_getLength(a));
    AAsset_close(a);
    return s;
}
static void extract_lang(AAssetManager* am) {
    std::string man = asset_text(am, "lang_manifest.txt");
    if (man.empty()) return;
    std::vector<std::string> lines;
    for (size_t p = 0; p < man.size();) {
        size_t e = man.find('\n', p);
        if (e == std::string::npos) e = man.size();
        if (e > p) lines.push_back(man.substr(p, e - p));
        p = e + 1;
    }
    std::string dir = g3_android_dir + "/lang", stamp = dir + "/.apk_lang";
    std::string old;
    if (FILE* f = fopen(stamp.c_str(), "rb")) {
        char b[128] = {};
        size_t n = fread(b, 1, sizeof b - 1, f);
        old.assign(b, n);
        fclose(f);
    }
    if (lines.empty() || old == lines[0]) return;
    mkdir(dir.c_str(), 0777);
    int n = 0;
    for (size_t i = 1; i < lines.size(); i++) {
        const std::string& rel = lines[i];
        for (size_t k = rel.find('/'); k != std::string::npos; k = rel.find('/', k + 1)) mkdir((dir + "/" + rel.substr(0, k)).c_str(), 0777);
        AAsset* a = AAssetManager_open(am, ("lang/" + rel).c_str(), AASSET_MODE_STREAMING);
        if (!a) continue;
        std::string tmp = dir + "/" + rel + ".tmp";
        if (FILE* f = fopen(tmp.c_str(), "wb")) {
            char buf[65536];
            int r;
            bool ok = true;
            while ((r = AAsset_read(a, buf, sizeof buf)) > 0) ok &= fwrite(buf, 1, (size_t)r, f) == (size_t)r;
            fclose(f);
            if (ok && r == 0 && rename(tmp.c_str(), (dir + "/" + rel).c_str()) == 0) n++;
        }
        AAsset_close(a);
    }
    if (FILE* f = fopen(stamp.c_str(), "wb")) {
        fwrite(lines[0].data(), 1, lines[0].size(), f);
        fclose(f);
    }
    ALOG("lang: %d of %zu files copied from the APK", n, lines.size() - 1);
}

void android_main(android_app* app) {
    g_app = app;
    g3_activity = app->activity;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    hud_sw_install();
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);  // a game: no screen timeout
    g3_android_dir = app->activity->externalDataPath ? app->activity->externalDataPath : app->activity->internalDataPath;
    mkdir(g3_android_dir.c_str(), 0777);
    ALOG("data folder: %s", g3_android_dir.c_str());
    extract_lang(app->activity->assetManager);

    std::thread game([] {
        std::vector<std::string> args = read_args(g3_android_dir + "/args.txt");
        // --env=NAME=VALUE: environment switches for tests (e.g. GRANDIA_VK_NO_DRAW_BARRIER=1)
        for (size_t i = 0; i < args.size();) {
            if (args[i].rfind("--env=", 0) == 0) {
                std::string kv = args[i].substr(6);
                size_t eq = kv.find('=');
                if (eq != std::string::npos) setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
                args.erase(args.begin() + i);
            } else i++;
        }
        args.insert(args.begin(), g3_android_dir + "/Grandia3.exe");
        std::vector<char*> argv;
        for (auto& s : args) argv.push_back(s.data());
        argv.push_back(nullptr);
        // the GS front end (GIF, primitive setup) on a thread of its own: phones have the cores, and
        // VIF1/VU1 alone fill one (GRANDIA_NO_GS_FE=1 keeps both on the pipeline thread)
        gsp::enable_front_end(!getenv("GRANDIA_NO_GS_FE"));
        int r = g3_main((int)args.size(), argv.data());
        ALOG("game ended (%d)", r);
        g_quit = true;
        g_win_cv.notify_all();
        if (g_app) ANativeActivity_finish(g_app->activity);
        // the runtime keeps its state in globals: a new start must get a fresh process, not run
        // g3_main a second time in this one (the activity may be created again in the same process)
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        _exit(0);
    });
    game.detach();
    std::thread(platform::present_thread).detach();

    while (true) {
        int events;
        android_poll_source* src = nullptr;
        if (ALooper_pollOnce(-1, nullptr, &events, (void**)&src) >= 0 && src) src->process(app, src);
        if (app->destroyRequested) {
            g_quit = true;
            g_win_cv.notify_all();
            _exit(0);  // the emulation has no clean shutdown; the next start begins fresh
        }
    }
}
