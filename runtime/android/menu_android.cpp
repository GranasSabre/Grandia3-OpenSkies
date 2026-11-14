// In-game settings menu for Android (the PC's F1 menu, overlay.cpp, draws with GDI). The panel is
// drawn into the presentation overlay (platform_android.cpp overlay_update) with the system's text
// rendering (jni_android.cpp text_coverage). Opened with SELECT+START or the controller's home button,
// or the menu button of the touch controls; the game is paused while it is open. Controller: d-pad
// (or stick) moves, left/right change, cross confirms, circle closes. Touch: tap a row to select it,
// tap the selected row to change it (left/right half of the value), tap outside to close; drag to scroll.
// Changes apply at once and are saved when the menu closes.
#include "platform/platform.h"
#include "ee/kernel.h"
#include "savestate.h"
#include "touch_android.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" int g_ee_patch[8];
namespace kernel {
extern std::atomic<bool> g_pause_req;
extern std::atomic<bool> g_pause_represent;
extern std::atomic<int> g_preview_vblanks;
}  // namespace kernel

namespace platform {

bool text_coverage(const std::string& text, int fs, int max_w, int m, std::vector<u8>& t, int& W, int& H);  // jni_android.cpp

namespace {

struct Item {
    const wchar_t* label;  // nullptr: section header given in value()
    std::function<std::wstring()> value;
    std::function<void(int dir)> change;  // dir -1/+1 (left/right), 0 = confirm
    bool header = false;
};

std::vector<Item> g_items;
std::atomic<bool> g_open{false};
int g_sel = 1, g_first = 0;
std::wstring g_note;
std::recursive_mutex g_mu;
std::atomic<u64> g_version{1};
bool g_disc_confirm = false;

// geometry of the last drawing (overlay pixels), for touch
struct Geo { int x0 = 0, y0 = 0, w = 0, h = 0, rows_y = 0, row_h = 1, vis = 0, val_x = 0; } g_geo;

// texts of the Android menu that the PC's table (i18n.cpp) does not have
const wchar_t* tra(const wchar_t* de) {
    static const wchar_t* const T[][2] = {
        {L"Spiel", L"Game"},
        {L"Savestate speichern", L"Save savestate"},
        {L"Savestate laden", L"Load savestate"},
        {L"Disc wechseln (beim nächsten Start)", L"Change disc (at the next start)"},
        {L"Disc-Abbild neu wählen (beim nächsten Start)", L"Choose the disc image again (at the next start)"},
        {L"wählen", L"choose"},
        {L"Beim nächsten Start fragt die App nach dem Abbild.", L"The app asks for the image at the next start."},
        {L"Wirkt beim nächsten Start der App.", L"Takes effect at the next start of the app."},
        {L"Tippen: auswählen, nochmal tippen: ändern    außerhalb tippen: schließen",
         L"Tap: select, tap again: change    tap outside: close"},
        {L"↑↓ Auswahl    ←→ ändern    Kreuz: auswählen    Kreis: schließen",
         L"↑↓ select    ←→ change    cross: choose    circle: close"},
    };
    if (!de || config().language == "de") return de;
    for (auto& t : T)
        if (wcscmp(t[0], de) == 0) return t[1];
    return tr(de);
}
std::wstring tra(const std::wstring& de) { return tra(de.c_str()); }

std::wstring onoff(bool b) { return b ? L"An" : L"Aus"; }
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void build_items() {
    Config& c = config();
    auto toggle = [](bool& b, std::function<void()> after = nullptr) {
        return [&b, after](int) { b = !b; if (after) after(); };
    };
    // options that change the picture: the paused game runs a few frames so the change shows
    auto drawn = [toggle](bool& b, std::function<void()> after = nullptr) {
        return toggle(b, [after] {
            if (after) after();
            kernel::g_preview_vblanks = 8;
        });
    };
    auto pct = [](int& v, int step) { return [&v, step](int dir) { v = clampi(v + (dir ? dir : 1) * step, 0, 100); }; };
    g_items.clear();
    g_items.push_back({nullptr, [] { return std::wstring(L"Grafik"); }, nullptr, true});
    g_items.push_back({L"Widescreen", [&c] { return onoff(c.widescreen_hack); }, drawn(c.widescreen_hack, [] { apply_patches(); })});
    g_items.push_back({L"Menüs/Texte im 4:3-Format", [&c] { return onoff(c.hud_fix); }, drawn(c.hud_fix)});
    g_items.push_back({L"Große Dialogfenster", [&c] {
                           if (c.big_text == 1 && !c.widescreen_hack) return std::wstring(L"braucht Widescreen");
                           return onoff(c.big_text == 1);
                       },
                       [&c](int) {
                           c.big_text = c.big_text == 1 ? 0 : 1;
                           kernel::g_preview_vblanks = 8;
                       }});
    g_items.push_back({L"Kinobalken entfernen (experimentell)", [&c] { return onoff(c.remove_letterbox); },
                       drawn(c.remove_letterbox, [&c] { g_ee_patch[1] = g_ee_patch[2] = c.remove_letterbox; })});
    g_items.push_back({L"Scharfe Schrift", [&c] { return onoff(c.sharp_text); }, drawn(c.sharp_text)});
    g_items.push_back({L"60 fps (experimentell)", [&c] { return onoff(c.fps60); }, toggle(c.fps60, [&c] { kernel::g_force60 = c.fps60; })});
    g_items.push_back({L"60 fps auch in Kämpfen (experimentell)", [&c] { return onoff(c.fps60_battle); }, toggle(c.fps60_battle)});
    g_items.push_back({L"Interne Auflösung (ab Neustart)", [&c] {
                           return c.internal_scale ? std::to_wstring(c.internal_scale) + L"x" : std::wstring(tr(L"Automatisch"));
                       },
                       [&c](int dir) { c.internal_scale = (c.internal_scale + (dir < 0 ? 4 : 1)) % 5; }});
    g_items.push_back({nullptr, [] { return std::wstring(L"Spiel"); }, nullptr, true});
    g_items.push_back({L"Untertitel in Zwischensequenzen", [&c] { return onoff(c.subtitles); }, toggle(c.subtitles)});
    g_items.push_back({L"Zwischensequenzen mit START überspringen", [&c] { return onoff(c.cutscene_skip); }, toggle(c.cutscene_skip)});
    g_items.push_back({L"Filme mit START überspringen", [&c] { return onoff(c.movie_skip); },
                       toggle(c.movie_skip, [&c] { g_ee_patch[3] = c.movie_skip; })});
    g_items.push_back({L"Kämpfe bei Gegnerkontakt", [&c] { return onoff(c.contact_battles); }, toggle(c.contact_battles)});
    g_items.push_back({L"Undub: japanische Stimmen (ab Neustart)", [&c] { return onoff(c.jp_voices); }, toggle(c.jp_voices)});
    g_items.push_back({nullptr, [] { return std::wstring(L"Audio"); }, nullptr, true});
    g_items.push_back({L"Lautstärke gesamt", [&c] { return std::to_wstring(c.volume) + L" %"; }, pct(c.volume, 5)});
    g_items.push_back({L"Musik", [&c] { return std::to_wstring(c.vol_music) + L" %"; }, pct(c.vol_music, 5)});
    g_items.push_back({L"Sprache", [&c] { return std::to_wstring(c.vol_voice) + L" %"; }, pct(c.vol_voice, 5)});
    g_items.push_back({L"Effekte", [&c] { return std::to_wstring(c.vol_sfx) + L" %"; }, pct(c.vol_sfx, 5)});
    g_items.push_back({L"Videos", [&c] { return std::to_wstring(c.vol_movie) + L" %"; }, pct(c.vol_movie, 5)});
    g_items.push_back({nullptr, [] { return std::wstring(L"Steuerung"); }, nullptr, true});
    g_items.push_back({L"Kamera umkehren (rechter Stick links/rechts)", [&c] { return onoff(c.camera_invert_x); }, toggle(c.camera_invert_x)});
    g_items.push_back({nullptr, [] { return std::wstring(L"System"); }, nullptr, true});
    g_items.push_back({L"Sprache (Language)", [&c] { return language_name(c.language); }, [&c](int dir) {
                           std::vector<std::string> l = languages();
                           size_t i = 0;
                           while (i < l.size() && l[i] != c.language) i++;
                           if (i == l.size()) i = 0;
                           c.language = l[(i + (dir < 0 ? l.size() - 1 : 1)) % l.size()];
                           g_note = L"Wirkt beim nächsten Start der App.";
                       }});
    g_items.push_back({L"Disc wechseln (beim nächsten Start)", [&c] {
                           if (g_disc_confirm) return std::wstring(L"Nochmal bestätigen!");
                           return std::wstring(c.disc == 2 ? L"Disc 2 → Disc 1" : L"Disc 1 → Disc 2");
                       },
                       [&c](int dir) {
                           if (dir != 0) return;
                           if (!g_disc_confirm) { g_disc_confirm = true; g_note = L"Nicht gespeicherter Fortschritt geht verloren."; return; }
                           c.disc = c.disc == 2 ? 1 : 2;
                           g_disc_confirm = false;
                           g_note = L"Wirkt beim nächsten Start der App.";
                       }});
    g_items.push_back({L"Disc-Abbild neu wählen (beim nächsten Start)", [] { return std::wstring(L"wählen"); }, [&c](int dir) {
                           if (dir != 0) return;
                           (c.disc == 2 ? c.iso_path2 : c.iso_path).clear();
                           g_note = L"Beim nächsten Start fragt die App nach dem Abbild.";
                       }});
    g_items.push_back({nullptr, [] { return std::wstring(L"Savestates (Spielstand an jeder Stelle)"); }, nullptr, true});
    g_items.push_back({L"Slot", [&c] {
                           std::string when;
                           std::wstring v = ss::slot_label(c.state_slot);
                           if (ss::slot_exists(c.state_slot, &when)) v += L"  (" + std::wstring(when.begin(), when.end()) + L")";
                           else v += tr(L"  (leer)");
                           return v;
                       },
                       // 0 = Auto (written after map changes), 1..9
                       [&c](int dir) { c.state_slot = (c.state_slot + (dir < 0 ? 9 : 1)) % 10; }});
    g_items.push_back({L"Savestate speichern", [] { return std::wstring(L"speichern"); }, [&c](int dir) {
                           if (dir != 0) return;
                           overlay_set_open(false);
                           ss::request_save(c.state_slot);
                       }});
    g_items.push_back({L"Savestate laden", [] { return std::wstring(L"laden"); }, [&c](int dir) {
                           if (dir != 0) return;
                           overlay_set_open(false);
                           ss::request_load(c.state_slot);
                       }});
    g_items.push_back({L"Nach Kartenwechsel automatisch (Slot Auto)", [&c] { return onoff(c.autosave_state); }, toggle(c.autosave_state)});
    g_items.push_back({L"Zurück zum Spiel", [] { return std::wstring(); }, [](int dir) {
                           if (dir == 0) overlay_set_open(false);
                       }});
}

bool is_action(const std::wstring& v) { return v == L"speichern" || v == L"laden" || v == L"wählen"; }

std::string utf8(const std::wstring& w) {
    std::string s;
    for (wchar_t wc : w) {
        u32 c = (u32)wc;
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | c >> 6); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xE0 | c >> 12); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }
    return s;
}

// text coverage per (text, size), made by the system's text layout
struct Cov { std::vector<u8> t; int w = 0, h = 0; };
const Cov& cov(const std::wstring& text, int fs) {
    static std::unordered_map<std::string, Cov> cache;
    std::string u = utf8(text), key = u + "\x1f" + std::to_string(fs);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 400) cache.clear();
    Cov c;
    if (!text.empty() && !text_coverage(u, fs, 100000, 0, c.t, c.w, c.h)) c = Cov{};
    return cache.emplace(key, std::move(c)).first->second;
}

// the panel: premultiplied RGBA (R in the low byte)
struct Panel {
    std::vector<u32> px;
    int w = 0, h = 0;
    void fill(int x0, int y0, int x1, int y1, u32 rgb, float a) {
        x0 = std::max(0, x0), y0 = std::max(0, y0), x1 = std::min(w, x1), y1 = std::min(h, y1);
        u32 ia = (u32)(a * 255 + 0.5f);
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) over(px[(size_t)y * w + x], rgb, ia);
    }
    static void over(u32& d, u32 rgb, u32 a) {
        if (!a) return;
        u32 na = 255 - a;
        u32 r = ((rgb & 0xFF) * a + (d & 0xFF) * na) / 255, g = (((rgb >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * na) / 255;
        u32 b = (((rgb >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * na) / 255, al = a + (d >> 24) * na / 255;
        d = r | g << 8 | b << 16 | al << 24;
    }
    // text with its left edge at x (right edge when right) and vertically centred on cy, clipped to [clip0, clip1)
    void text(const std::wstring& s, int fs, int x, int cy, u32 rgb, bool right = false, int clip0 = 0, int clip1 = 1 << 30) {
        const Cov& c = cov(s, fs);
        if (c.w <= 0) return;
        int x0 = right ? x - c.w : x, y0 = cy - c.h / 2;
        for (int y = 0; y < c.h; y++) {
            int py = y0 + y;
            if (py < 0 || py >= h) continue;
            for (int k = 0; k < c.w; k++) {
                int pxx = x0 + k;
                if (pxx < std::max(0, clip0) || pxx >= std::min(w, clip1)) continue;
                over(px[(size_t)py * w + pxx], rgb, c.t[(size_t)y * c.w + k]);
            }
        }
    }
};
Panel g_panel;
u64 g_drawn = 0;
int g_for_w = 0, g_for_h = 0;

// colors as 0xBBGGRR
constexpr u32 BG = 0x22160F, FRAME = 0x4682A0, TITLE = 0x96D7EB, HEAD = 0xD2AA96, SELBG = 0x6E463C, TXT = 0xDCD2CD, TXT_SEL = 0xFFFFFF,
              VAL = 0x8CC8DC, VAL_SEL = 0x8CE1FF, NOTE = 0xB4A5A0;

void render(int ow, int oh) {
    if (g_items.empty()) build_items();
    int rows = (int)g_items.size();
    int row_h = clampi(oh / 17, 18, 64);
    int fs = std::max(10, row_h * 58 / 100), fs_title = std::max(12, row_h * 85 / 100), fs_head = std::max(9, row_h * 50 / 100),
        fs_small = std::max(9, row_h * 48 / 100);
    int top = row_h * 2, bottom = row_h * 2 + row_h / 2;
    int W = std::min(ow - 8, row_h * 28);
    int vis = std::max(4, std::min(rows, (oh - 12 - top - bottom) / row_h));
    if (g_sel - 1 < g_first) g_first = std::max(0, g_sel - 1);
    if (g_sel + 1 >= g_first + vis) g_first = g_sel + 2 - vis;
    g_first = std::max(0, std::min(g_first, rows - vis));
    int H = top + vis * row_h + bottom;
    Panel& p = g_panel;
    p.w = W;
    p.h = H;
    p.px.assign((size_t)W * H, 0);
    p.fill(0, 0, W, H, BG, 0.93f);
    int fr = std::max(1, row_h / 14), m = row_h / 5;
    p.fill(m, m, W - m, m + fr, FRAME, 1);
    p.fill(m, H - m - fr, W - m, H - m, FRAME, 1);
    p.fill(m, m, m + fr, H - m, FRAME, 1);
    p.fill(W - m - fr, m, W - m, H - m, FRAME, 1);
    int pad = row_h;
    p.text(tr(L"Einstellungen"), fs_title, pad, top / 2 + m / 2, TITLE);
    if (vis < rows) {
        std::wstring s = std::wstring(g_first > 0 ? L"▲" : L"  ") + L"  " + (g_first + vis < rows ? L"▼" : L"  ");
        p.text(s, fs_head, W - pad, top / 2 + m / 2, HEAD, true);
    }
    int val_x = W * 62 / 100;
    for (int i = g_first; i < g_first + vis; i++) {
        const Item& it = g_items[i];
        int y = top + (i - g_first) * row_h, cy = y + row_h / 2;
        if (it.header) {
            p.text(tra(it.value()), fs_head, pad, y + row_h * 62 / 100, HEAD);
            continue;
        }
        bool sel = i == g_sel;
        if (sel) p.fill(pad / 2, y, W - pad / 2, y + row_h, SELBG, 1);
        p.text(tra(it.label), fs, pad + row_h / 2, cy, sel ? TXT_SEL : TXT, false, 0, val_x - row_h / 3);
        std::wstring v = it.value();
        if (!v.empty()) {
            bool action = is_action(v);
            v = tra(v);
            if (sel && it.change && !action) v = L"‹  " + v + L"  ›";
            p.text(v, fs, W - pad, cy, sel ? VAL_SEL : VAL, true, val_x - row_h / 3);
        }
    }
    if (!g_note.empty()) p.text(tra(g_note), fs_small, pad, H - bottom + row_h * 3 / 4, NOTE);
    bool pad_ui = !touch::visible();
    p.text(tra(pad_ui ? L"↑↓ Auswahl    ←→ ändern    Kreuz: auswählen    Kreis: schließen"
                      : L"Tippen: auswählen, nochmal tippen: ändern    außerhalb tippen: schließen"),
           fs_small, pad, H - row_h * 3 / 4 - m, NOTE);
    g_geo.w = W;
    g_geo.h = H;
    g_geo.rows_y = top;
    g_geo.row_h = row_h;
    g_geo.vis = vis;
    g_geo.val_x = val_x;
}

void move_sel(int d) {
    int n = (int)g_items.size();
    do { g_sel = (g_sel + d + n) % n; } while (g_items[g_sel].header);
    if (g_disc_confirm) { g_disc_confirm = false; g_note.clear(); }
}

void invoke(int dir) {
    if (g_sel < 0 || g_sel >= (int)g_items.size()) return;
    Item it = g_items[g_sel];  // a copy: the action may close the menu
    if (it.change) it.change(dir);
}

// touch: where a finger went down (overlay pixels) and whether it moved (scrolling)
float g_tx = 0, g_ty = 0;
int g_tfirst = 0;
bool g_tdown = false, g_tmoved = false;

}  // namespace

bool overlay_open() { return g_open.load(std::memory_order_acquire); }

void overlay_set_open(bool on) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (on == g_open.load()) return;
    if (on) {
        build_items();
        if (g_sel <= 0 || g_sel >= (int)g_items.size() || g_items[g_sel].header) g_sel = 1;
        g_disc_confirm = false;
        g_note.clear();
        g_tdown = false;
        g_open = true;
        kernel::g_pause_represent = true;
        kernel::g_pause_req = true;
        log_write(LOG_MAIN, "menu: open");
    } else {
        g_open = false;
        kernel::g_pause_represent = false;
        kernel::g_pause_req = false;
        save_config();
        log_write(LOG_MAIN, "menu: closed");
    }
    g_version++;
}

// controller / keys: 0 up, 1 down, 2 left, 3 right, 4 confirm, 5 close
void menu_nav(int k) {
    if (!overlay_open()) return;
    if (k == 5) { overlay_set_open(false); return; }
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    switch (k) {
    case 0: move_sel(-1); break;
    case 1: move_sel(1); break;
    case 2: invoke(-1); break;
    case 3: invoke(1); break;
    case 4: invoke(0); break;
    }
    g_version++;
}

// touch in overlay pixels: act 0 down, 1 move, 2 up, 3 cancel
void menu_touch(int act, float x, float y) {
    if (!overlay_open()) return;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    const Geo& g = g_geo;
    if (act == 0) {
        g_tdown = true;
        g_tmoved = false;
        g_tx = x, g_ty = y;
        g_tfirst = g_first;
        return;
    }
    if (!g_tdown) return;
    if (act == 3) { g_tdown = false; return; }
    float dy = y - g_ty;
    if (act == 1) {
        // dragging scrolls the list (the selection follows into view)
        if (std::fabs(dy) > g.row_h * 0.6f) g_tmoved = true;
        if (g_tmoved) {
            int rows = (int)g_items.size();
            int f = std::clamp(g_tfirst - (int)std::lround(dy / g.row_h), 0, std::max(0, rows - g.vis));
            if (f != g_first) {
                g_first = f;
                if (g_sel < f + 1) { g_sel = f; while (g_sel < rows - 1 && g_items[g_sel].header) g_sel++; }
                if (g_sel > f + g.vis - 2) { g_sel = f + g.vis - 2; while (g_sel > 0 && g_items[g_sel].header) g_sel--; }
                g_version++;
            }
        }
        return;
    }
    g_tdown = false;
    if (g_tmoved) return;
    float px = x - g.x0, py = y - g.y0;
    if (px < 0 || py < 0 || px >= g.w || py >= g.h) {
        overlay_set_open(false);
        return;
    }
    int r = (int)std::floor((py - g.rows_y) / g.row_h);
    if (r < 0 || r >= g.vis) return;
    int i = g_first + r;
    if (i >= (int)g_items.size() || g_items[i].header) return;
    if (i != g_sel) {
        g_sel = i;
        if (g_disc_confirm) { g_disc_confirm = false; g_note.clear(); }
    } else {
        std::wstring v = g_items[i].value ? g_items[i].value() : L"";
        if (v.empty() || is_action(v) || px < g.val_x) invoke(px < g.val_x && !v.empty() && !is_action(v) ? 1 : 0);
        else invoke(px < (g.val_x + g.w) / 2 ? -1 : 1);
    }
    g_version++;
}

// the panel for an overlay of ow x oh (premultiplied RGBA) and its position; false when closed
bool menu_pixels(int ow, int oh, std::vector<u32>& out, int& w, int& h, int& x, int& y, u64& version) {
    if (!overlay_open()) return false;
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    u64 v = g_version;
    if (g_drawn != v || ow != g_for_w || oh != g_for_h) {
        render(ow, oh);
        g_drawn = v;
        g_for_w = ow;
        g_for_h = oh;
    }
    g_geo.x0 = (ow - g_panel.w) / 2;
    g_geo.y0 = (oh - g_panel.h) / 2;
    if (version != v || out.size() != g_panel.px.size()) out = g_panel.px;
    w = g_panel.w;
    h = g_panel.h;
    x = g_geo.x0;
    y = g_geo.y0;
    version = v;
    return true;
}

}  // namespace platform
