// In-game settings menu: drawn with GDI into a bitmap that the presenter blends over the game
// picture (platform.cpp draw_present). Opened with F1 or Back+Start on the controller; the game is
// paused while it is open. Changes apply at once and are saved when the menu closes.
#define UNICODE
#include "platform.h"
#include "../ee/kernel.h"
#include "../savestate.h"
#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

extern "C" int g_ee_patch[8];
namespace kernel {
extern std::atomic<bool> g_pause_req;
extern std::atomic<bool> g_pause_represent;
extern std::atomic<int> g_preview_vblanks;
}

namespace platform {

void set_fullscreen_public(bool on);
void restart_game();
HWND__* main_hwnd();

namespace {

struct Item {
    const wchar_t* label;   // nullptr: section header given in value()
    std::function<std::wstring()> value;
    std::function<void(int dir)> change;   // dir -1/+1 (left/right), 0 = confirm
    bool header = false;
};

std::vector<Item> g_items;
std::atomic<bool> g_open{false};
int g_sel = 1;
int g_tab = 0;     // shown tab (= section)
int g_first = 0;   // first row shown when the list scrolls
bool g_disc_confirm = false, g_quit_confirm = false;
std::wstring g_note;
std::mutex g_mu;
std::vector<u32> g_px;
int g_w = 0, g_h = 0, g_target_h = 0;
std::atomic<int> g_target_w{0};  // the window width (the menu may not be wider)
u64 g_version = 1, g_drawn = 0;

std::wstring onoff(bool b) { return b ? L"An" : L"Aus"; }

// size factor of the port's panels for a picture height (base: the height at which they have their
// design size). The Steam Deck's small screen gets them about a third larger.
double ui_scale(int target_h, double base) {
    double s = target_h / base;
    if (is_steam_deck()) s *= 1.35;
    return s < 0.6 ? 0.6 : s;
}
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void build_items() {
    Config& c = config();
    auto toggle = [](bool& b, std::function<void()> after = nullptr) {
        return [&b, after](int) { b = !b; if (after) after(); };
    };
    // options that change how the scene is drawn: the paused game runs a few frames (without
    // input) so the picture behind the menu shows the change at once
    auto drawn = [toggle](bool& b, std::function<void()> after = nullptr) {
        return toggle(b, [after] {
            if (after) after();
            kernel::g_preview_vblanks = 8;
        });
    };
    auto pct = [](int& v, int step) {
        return [&v, step](int dir) { v = clampi(v + (dir ? dir : 1) * step, 0, 100); };
    };
    g_items.clear();
    g_items.push_back({nullptr, [] { return std::wstring(L"Savestates"); }, nullptr, true});
    g_items.push_back({L"Slot", [&c] {
                           std::string when;
                           std::wstring v = ss::slot_label(c.state_slot);
                           if (ss::slot_exists(c.state_slot, &when)) v += L"  (" + std::wstring(when.begin(), when.end()) + L")";
                           else v += tr(L"  (leer)");
                           return v;
                       },
                       // 0 = Auto (written after map changes), 1..9
                       [&c](int dir) { c.state_slot = (c.state_slot + (dir < 0 ? 9 : 1)) % 10; }});
    g_items.push_back({L"Savestate speichern (F5, Back+R1)", [] { return std::wstring(L"speichern"); }, [&c](int dir) {
                           if (dir != 0) return;
                           overlay_set_open(false);
                           ss::request_save(c.state_slot);
                       }});
    g_items.push_back({L"Savestate laden (F9, Back+L1)", [] { return std::wstring(L"laden"); }, [&c](int dir) {
                           if (dir != 0) return;
                           overlay_set_open(false);
                           ss::request_load(c.state_slot);
                       }});
    g_items.push_back({L"Nach Kartenwechsel automatisch (Slot Auto)", [&c] { return onoff(c.autosave_state); }, toggle(c.autosave_state)});
    g_items.push_back({nullptr, [] { return std::wstring(L"Bild"); }, nullptr, true});
    g_items.push_back({L"Vollbild", [&c] { return onoff(c.fullscreen); },
                       toggle(c.fullscreen, [&c] { set_fullscreen_public(c.fullscreen); })});
    g_items.push_back({L"Seitenverhältnis", [&c] {
                           static const wchar_t* N[5] = {L"4:3", L"16:9", L"Fenster füllen", L"16:10 (Steam Deck)", L"Bildschirm (Ultrawide)"};
                           return std::wstring(N[clampi(c.aspect, 0, 4)]);
                       },
                       [&c](int dir) {
                           c.aspect = (c.aspect + (dir < 0 ? 4 : 1)) % 5;
                           apply_patches();
                           kernel::g_preview_vblanks = 8;
                       }});
    g_items.push_back({L"Widescreen", [&c] { return onoff(c.widescreen_hack); },
                       drawn(c.widescreen_hack, [] { apply_patches(); })});
    g_items.push_back({L"Menüs/Texte im 4:3-Format", [&c] { return onoff(c.hud_fix); }, drawn(c.hud_fix)});
    g_items.push_back({L"Interne Auflösung (ab Neustart)", [&c] {
                           return c.internal_scale ? std::to_wstring(c.internal_scale) + L"x" : std::wstring(tr(L"Automatisch"));
                       },
                       [&c](int dir) { c.internal_scale = (c.internal_scale + (dir < 0 ? 8 : 1)) % 9; }});
    g_items.push_back({L"Bildfilter", [&c] {
                           static const wchar_t* N[3] = {L"Pixel", L"Weich", L"Scharf-weich"};
                           return std::wstring(N[clampi(c.filter, 0, 2)]);
                       },
                       [&c](int dir) { c.filter = (c.filter + (dir < 0 ? 2 : 1)) % 3; }});
    g_items.push_back({L"Nachschärfen", [&c] { return c.sharpen ? std::to_wstring(c.sharpen) : std::wstring(L"Aus"); },
                       [&c](int dir) { c.sharpen = clampi(c.sharpen + (dir ? dir : 1) * 10, 0, 100); }});
    g_items.push_back({L"HD-Texturen", [&c] { return onoff(c.hd_textures); }, drawn(c.hd_textures)});
    g_items.push_back({L"Scharfe Schrift", [&c] { return onoff(c.sharp_text); }, drawn(c.sharp_text)});
    g_items.push_back({L"Kinobalken entfernen (experimentell)", [&c] { return onoff(c.remove_letterbox); },
                       drawn(c.remove_letterbox, [&c] { g_ee_patch[1] = g_ee_patch[2] = c.remove_letterbox; })});
    g_items.push_back({L"60 fps (experimentell)", [&c] { return onoff(c.fps60); },
                       toggle(c.fps60, [&c] { kernel::g_force60 = c.fps60; })});
    g_items.push_back({L"60 fps auch in Kämpfen (experimentell)", [&c] { return onoff(c.fps60_battle); }, toggle(c.fps60_battle)});
    g_items.push_back({L"VSync", [&c] { return onoff(c.vsync); }, toggle(c.vsync)});
    g_items.push_back({L"Bildrate anzeigen", [&c] { return onoff(c.show_fps); }, toggle(c.show_fps)});
    g_items.push_back({nullptr, [] { return std::wstring(L"Spielablauf"); }, nullptr, true});
    g_items.push_back({L"Zwischensequenzen mit START überspringen", [&c] { return onoff(c.cutscene_skip); }, toggle(c.cutscene_skip)});
    g_items.push_back({L"Filme mit START überspringen", [&c] { return onoff(c.movie_skip); },
                       toggle(c.movie_skip, [&c] { g_ee_patch[3] = c.movie_skip; })});
    g_items.push_back({L"Kämpfe bei Gegnerkontakt", [&c] { return onoff(c.contact_battles); }, toggle(c.contact_battles)});
    g_items.push_back({L"EXP aus Kämpfen", [&c] { return L"x" + std::to_wstring(c.exp_mult); },
                       [&c](int dir) { c.exp_mult = (c.exp_mult - 1 + (dir < 0 ? 3 : 1)) % 4 + 1; }});
    g_items.push_back({L"Gold aus Kämpfen", [&c] { return L"x" + std::to_wstring(c.gold_mult); },
                       [&c](int dir) { c.gold_mult = (c.gold_mult - 1 + (dir < 0 ? 3 : 1)) % 4 + 1; }});
    g_items.push_back({nullptr, [] { return std::wstring(L"Sprache und Text"); }, nullptr, true});
    g_items.push_back({L"Sprache (Language)", [&c] { return language_name(c.language); }, [&c](int dir) {
                           std::vector<std::string> l = languages();
                           size_t i = 0;
                           while (i < l.size() && l[i] != c.language) i++;
                           if (i == l.size()) i = 0;
                           c.language = l[(i + (dir < 0 ? l.size() - 1 : 1)) % l.size()];
                           g_note = L"Die Spieltexte wechseln beim nächsten Start.";
                       }});
    g_items.push_back({L"Untertitel in Zwischensequenzen", [&c] { return onoff(c.subtitles); }, toggle(c.subtitles)});
    g_items.push_back({L"Undub: japanische Stimmen (ab Neustart)", [&c] { return onoff(c.jp_voices); }, toggle(c.jp_voices)});
    g_items.push_back({L"Große Dialogfenster", [&c] {
                           bool wanted = c.big_text == 1 || (c.big_text < 0 && is_steam_deck());
                           if (wanted && !c.widescreen_hack) return std::wstring(L"braucht Widescreen");
                           if (c.big_text >= 0) return onoff(c.big_text == 1);
                           return std::wstring(wanted ? L"Auto (an: Steam Deck)" : L"Auto (aus)");
                       },
                       [&c](int dir) {
                           // automatic -> on -> off
                           static const int ORDER[3] = {-1, 1, 0};
                           int i = c.big_text < 0 ? 0 : (c.big_text ? 1 : 2);
                           c.big_text = ORDER[(i + (dir < 0 ? 2 : 1)) % 3];
                           kernel::g_preview_vblanks = 8;
                       }});
    g_items.push_back({nullptr, [] { return std::wstring(L"Audio"); }, nullptr, true});
    g_items.push_back({L"Lautstärke gesamt", [&c] { return std::to_wstring(c.volume) + L" %"; }, pct(c.volume, 5)});
    g_items.push_back({L"Musik", [&c] { return std::to_wstring(c.vol_music) + L" %"; }, pct(c.vol_music, 5)});
    g_items.push_back({L"Sprache", [&c] { return std::to_wstring(c.vol_voice) + L" %"; }, pct(c.vol_voice, 5)});
    g_items.push_back({L"Effekte", [&c] { return std::to_wstring(c.vol_sfx) + L" %"; }, pct(c.vol_sfx, 5)});
    g_items.push_back({L"Videos", [&c] { return std::to_wstring(c.vol_movie) + L" %"; }, pct(c.vol_movie, 5)});
    g_items.push_back({nullptr, [] { return std::wstring(L"Steuerung"); }, nullptr, true});
    g_items.push_back({L"Kamera-Drehtempo", [&c] { return std::to_wstring(clampi(c.cam_speed, 100, 300)) + L" %"; },
                       [&c](int dir) { c.cam_speed = clampi(c.cam_speed, 100, 300) + (dir < 0 ? -25 : 25); if (c.cam_speed > 300) c.cam_speed = 100; if (c.cam_speed < 100) c.cam_speed = 300; }});
    g_items.push_back({L"Kamera umkehren (rechter Stick links/rechts)", [&c] { return onoff(c.camera_invert_x); }, toggle(c.camera_invert_x)});
    g_items.push_back({L"Vibration", [&c] { return onoff(c.vibration); }, toggle(c.vibration, [] { set_vibration(0, 0); })});
    g_items.push_back({L"Tasten-/Controllerbelegung …", [] { return std::wstring(L"öffnen"); }, [](int dir) {
                           if (dir != 0) return;
                           overlay_set_open(false);
                           open_settings(main_hwnd());
                       }});
    g_items.push_back({nullptr, [] { return std::wstring(L"System"); }, nullptr, true});
    g_items.push_back({L"Beim Start nach Updates suchen", [&c] { return onoff(c.update_check == 1); },
                       [&c](int) { c.update_check = c.update_check == 1 ? 0 : 1; }});
    g_items.push_back({L"Größe dieses Menüs", [&c] { return std::to_wstring(clampi(c.menu_size, 100, 200)) + L" %"; },
                       [&c](int dir) { c.menu_size = clampi(c.menu_size, 100, 200) + (dir < 0 ? -25 : 25); if (c.menu_size > 200) c.menu_size = 100; if (c.menu_size < 100) c.menu_size = 200; }});
    g_items.push_back({L"Disc wechseln (Neustart)", [&c] {
                           if (g_disc_confirm) return std::wstring(L"Nochmal bestätigen!");
                           return std::wstring(c.disc == 2 ? L"Disc 2 → Disc 1" : L"Disc 1 → Disc 2");
                       },
                       [&c](int dir) {
                           if (dir != 0) return;
                           if (!g_disc_confirm) { g_disc_confirm = true; g_note = L"Nicht gespeicherter Fortschritt geht verloren."; return; }
                           c.disc = c.disc == 2 ? 1 : 2;
                           save_config();
                           restart_game();
                       }});
    g_items.push_back({L"Zurück zum Spiel", [] { return std::wstring(); }, [](int dir) {
                           if (dir == 0) overlay_set_open(false);
                       }});
    g_items.push_back({L"Spiel beenden", [] {
                           return std::wstring(g_quit_confirm ? L"Nochmal bestätigen!" : L"beenden");
                       },
                       [](int dir) {
                           if (dir != 0) return;
                           if (!g_quit_confirm) { g_quit_confirm = true; g_note = L"Nicht gespeicherter Fortschritt geht verloren."; return; }
                           save_config();
                           // the same way out as closing the window
                           PostMessageW((HWND)main_hwnd(), WM_CLOSE, 0, 0);
                       }});
}

// one line of help per item, shown under the list
const wchar_t* item_help(const wchar_t* label) {
    static const wchar_t* const H[][2] = {
        {L"Slot", L"Wähle einen von 10 Plätzen. „Auto“ wird nach jedem Kartenwechsel von selbst beschrieben."},
        {L"Savestate speichern (F5, Back+R1)", L"Speichert genau diese Stelle im gewählten Slot."},
        {L"Savestate laden (F9, Back+L1)", L"Lädt den gewählten Slot. Nicht gespeicherter Fortschritt geht verloren."},
        {L"Nach Kartenwechsel automatisch (Slot Auto)", L"Legt ein paar Sekunden nach jedem Kartenwechsel einen Savestate im Slot „Auto“ an."},
        {L"Vollbild", L"Wechselt zwischen Fenster und Vollbild (auch F11)."},
        {L"Seitenverhältnis", L"Form des Bildes: 4:3 wie früher, 16:9, 16:10 (Steam Deck) oder die Form des Bildschirms (Ultrawide)."},
        {L"Widescreen", L"Die 3D-Welt wird breiter statt gestreckt. Für echtes 4:3 ausschalten."},
        {L"Menüs/Texte im 4:3-Format", L"Menüs, Textfenster und Anzeigen behalten im Breitbild ihre ursprüngliche Form."},
        {L"Interne Auflösung (ab Neustart)", L"Wie fein das Bild berechnet wird. „Automatisch“ passt zum Bildschirm; höher kostet Grafikleistung."},
        {L"Bildfilter", L"Pixel: hart. Weich: geglättet. Scharf-weich: glatt, aber knackig."},
        {L"Nachschärfen", L"Macht Kanten und Texturen schärfer. Aus = wie berechnet."},
        {L"HD-Texturen", L"Nutzt das HD-Texturpaket im Ordner „textures“, falls vorhanden."},
        {L"Scharfe Schrift", L"Glättet die Schrift der Spieltexte bei hoher Auflösung."},
        {L"Kinobalken entfernen (experimentell)", L"Entfernt die schwarzen Balken in Zwischensequenzen und Kämpfen."},
        {L"60 fps (experimentell)", L"Doppelte Bildrate beim Laufen durch die Welt. Zwischensequenzen bleiben bei 30."},
        {L"60 fps auch in Kämpfen (experimentell)", L"60 fps auch in Kämpfen. Braucht mehr Leistung."},
        {L"VSync", L"Gleicht die Bildausgabe mit dem Monitor ab und verhindert Bildrisse."},
        {L"Bildrate anzeigen", L"Zeigt die Bilder pro Sekunde in der Ecke an."},
        {L"Zwischensequenzen mit START überspringen", L"START spult eine Zwischensequenz unsichtbar bis zu ihrem Ende vor."},
        {L"Filme mit START überspringen", L"Jeder Film lässt sich mit START oder SELECT abbrechen."},
        {L"Kämpfe bei Gegnerkontakt", L"Aus: Monster im Feld starten keinen Kampf. Story- und Bosskämpfe bleiben."},
        {L"EXP aus Kämpfen", L"Vervielfacht die Erfahrungspunkte nach jedem Kampf (x1 = Original)."},
        {L"Gold aus Kämpfen", L"Vervielfacht das Gold nach jedem Kampf (x1 = Original)."},
        {L"Sprache (Language)", L"Sprache der Spieltexte und Menüs. Wirkt beim nächsten Start."},
        {L"Untertitel in Zwischensequenzen", L"Untertitel zu allen gesprochenen Zwischensequenzen und Filmen."},
        {L"Undub: japanische Stimmen (ab Neustart)", L"Japanische Originalstimmen. Braucht deine japanische Disc 1 als ISO."},
        {L"Große Dialogfenster", L"Größere Textfenster für kleine Bildschirme. „Auto“: nur auf dem Steam Deck."},
        {L"Lautstärke gesamt", L"Lautstärke des ganzen Spiels."},
        {L"Musik", L"Lautstärke der Musik."},
        {L"Sprache", L"Lautstärke der Stimmen in Zwischensequenzen und Kämpfen."},
        {L"Effekte", L"Lautstärke der Geräusche und Effekte."},
        {L"Videos", L"Lautstärke der Filme."},
        {L"Kamera-Drehtempo", L"Wie schnell der rechte Stick die Kamera dreht (100 % = Original)."},
        {L"Kamera umkehren (rechter Stick links/rechts)", L"Dreht die Richtung des rechten Sticks links/rechts um."},
        {L"Vibration", L"Rüttelfunktion des Controllers."},
        {L"Tasten-/Controllerbelegung …", L"Öffnet das Fenster für die Tastatur- und Controllerbelegung."},
        {L"Beim Start nach Updates suchen", L"Fragt beim Start GitHub nach einer neuen Version. Über dich wird nichts gesendet."},
        {L"Größe dieses Menüs", L"Macht dieses Menü größer oder kleiner."},
        {L"Disc wechseln (Neustart)", L"Startet das Spiel mit der anderen Disc neu."},
        {L"Zurück zum Spiel", L"Schließt das Menü (auch Esc, F1 oder Kreis)."},
        {L"Spiel beenden", L"Beendet das Spiel. Vorher speichern!"},
    };
    if (!label) return L"";
    for (auto& h : H) if (wcscmp(h[0], label) == 0) return h[1];
    return L"";
}

// tabs: every section header starts one; its items follow up to the next header
void tab_range(int t, int* first, int* end) {
    int n = (int)g_items.size(), k = -1;
    *first = 0; *end = n;
    for (int i = 0; i < n; i++) {
        if (!g_items[i].header) continue;
        if (++k == t) *first = i + 1;
        else if (k == t + 1) { *end = i; return; }
    }
}
int tab_count() {
    int k = 0;
    for (auto& it : g_items) k += it.header;
    return k;
}
int tab_of(int sel) {
    int k = -1;
    for (int i = 0; i <= sel && i < (int)g_items.size(); i++) k += g_items[i].header;
    return k < 0 ? 0 : k;
}

// ---------------------------------------------------------------------------------------------------
// Drawing in the look of the game's own menus: light brushed panels in an olive-gold frame with
// rivets in the corners, dark blue text with a light glow, captions as small plaques
// ---------------------------------------------------------------------------------------------------
struct Canvas {
    int W = 0, H = 0;
    std::vector<u32> p;  // BGRA, straight alpha
};

inline void over(u32& d, u32 rgb, float a) {
    if (a <= 0) return;
    if (a > 1) a = 1;
    float da = (d >> 24) / 255.0f;
    float oa = a + da * (1 - a);
    if (oa <= 0) return;
    auto ch = [&](int sh) {
        float c = ((rgb >> sh) & 255) * a + ((d >> sh) & 255) * da * (1 - a);
        return (u32)std::min(255.0f, c / oa + 0.5f);
    };
    d = ((u32)(oa * 255 + 0.5f) << 24) | ch(16) << 16 | ch(8) << 8 | ch(0);
}

// coverage of a rounded rectangle at pixel (px, py), anti-aliased
inline float rr_cover(float px, float py, float x, float y, float w, float h, float r) {
    float cx = x + w / 2, cy = y + h / 2, hx = w / 2 - r, hy = h / 2 - r;
    float dx = fabsf(px + 0.5f - cx) - hx, dy = fabsf(py + 0.5f - cy) - hy;
    float ox = std::max(dx, 0.0f), oy = std::max(dy, 0.0f);
    float dist = sqrtf(ox * ox + oy * oy) + std::min(std::max(dx, dy), 0.0f) - r;
    return std::min(1.0f, std::max(0.0f, 0.5f - dist));
}

template <class F>
void rr_fill(Canvas& cv, float x, float y, float w, float h, float r, float alpha, F color) {
    int x0 = std::max(0, (int)x - 1), x1 = std::min(cv.W, (int)(x + w) + 2);
    int y0 = std::max(0, (int)y - 1), y1 = std::min(cv.H, (int)(y + h) + 2);
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float c = rr_cover((float)px, (float)py, x, y, w, h, r);
            if (c > 0) over(cv.p[(size_t)py * cv.W + px], color(px, py), alpha * c);
        }
}
void rr_solid(Canvas& cv, float x, float y, float w, float h, float r, u32 rgb, float alpha) {
    rr_fill(cv, x, y, w, h, r, alpha, [rgb](int, int) { return rgb; });
}
inline u32 mix(u32 a, u32 b, float t) {
    auto ch = [&](int sh) { return (u32)(((a >> sh) & 255) * (1 - t) + ((b >> sh) & 255) * t + 0.5f); };
    return ch(16) << 16 | ch(8) << 8 | ch(0);
}
inline u32 shade(u32 c, float k) {
    auto ch = [&](int sh) { return (u32)std::min(255.0f, std::max(0.0f, ((c >> sh) & 255) * k)); };
    return ch(16) << 16 | ch(8) << 8 | ch(0);
}
inline float hash1(int a, int b) {
    u32 h = (u32)a * 374761393u + (u32)b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

// a panel: olive-gold frame with bevel, brushed light metal inside, a rivet in each corner
void panel(Canvas& cv, float x, float y, float w, float h, float s, float alpha = 0.97f) {
    const u32 OUTLINE = 0x3C3A26, FRAME = 0xB4AE72, FRAME_HI = 0xDCD69C, FRAME_LO = 0x7C7848;
    float r = 13 * s, b = 8 * s;
    rr_solid(cv, x + 3 * s, y + 4 * s, w, h, r, 0x000000, 0.35f * alpha);        // drop shadow
    rr_solid(cv, x, y, w, h, r, OUTLINE, alpha);
    rr_fill(cv, x + 1.5f * s, y + 1.5f * s, w - 3 * s, h - 3 * s, r - 1.5f * s, alpha, [&](int px, int py) {
        float t = ((py - y) / h + (px - x) / w * 0.35f);                         // light from the top left
        return t < 0.5f ? mix(FRAME_HI, FRAME, t * 2) : mix(FRAME, FRAME_LO, (t - 0.5f) / 0.85f);
    });
    rr_solid(cv, x + b, y + b, w - 2 * b, h - 2 * b, r - b * 0.6f, 0x4A4830, alpha);  // inner edge
    float ix = x + b + 1.5f * s, iy = y + b + 1.5f * s, iw = w - 2 * b - 3 * s, ih = h - 2 * b - 3 * s;
    rr_fill(cv, ix, iy, iw, ih, r - b * 0.7f, alpha, [&](int px, int py) {
        float t = (py - iy) / ih;
        float dg = ((px - ix) / iw + t) * 0.5f;  // light top left -> dark bottom right, as in the game
        // colour stops measured on the game's own menu windows ("Magie nutzen", Yuki's status):
        // light beige-grey top left -> dark grey-brown bottom right
        static const u32 STOP[9] = {0xCCBEAE, 0xBEB0A2, 0xAA9E94, 0x8C817E, 0x726A6E, 0x60595A, 0x544E4E, 0x494441, 0x3E3A36};
        float q = std::min(1.0f, std::max(0.0f, dg)) * 8;
        int qi = std::min(7, (int)q);
        u32 base = mix(STOP[qi], STOP[qi + 1], q - qi);
        // brushed streaks: per row noise, smoothed along x
        int row = (int)((py - iy) / std::max(1.0f, s));
        float u = (px - ix) / (60 * s);
        int ui = (int)u;
        float f = u - ui;
        float n = hash1(row, ui) * (1 - f) + hash1(row, ui + 1) * f;
        float n2 = hash1(row / 3, 7);
        return shade(base, 0.96f + 0.06f * n + 0.03f * n2);
    });
    // rivets
    float rr = 4.2f * s, off = b * 0.5f + 0.75f * s;
    for (int k = 0; k < 4; k++) {
        float cx = (k & 1) ? x + w - off - 1.5f * s : x + off + 1.5f * s;
        float cy = (k & 2) ? y + h - off - 1.5f * s : y + off + 1.5f * s;
        rr_solid(cv, cx - rr, cy - rr, 2 * rr, 2 * rr, rr, 0x3C3A26, alpha);
        rr_fill(cv, cx - rr + s, cy - rr + s, 2 * rr - 2 * s, 2 * rr - 2 * s, rr - s, alpha, [&](int px, int py) {
            float t = ((px - cx) + (py - cy)) / (2 * rr) + 0.5f;
            u32 c = mix(0xE6E2BE, 0x8A8660, std::min(1.0f, std::max(0.0f, t)));
            // the slot of the screw, diagonal
            float d = fabsf((px + 0.5f - cx) - (py + 0.5f - cy)) / 1.414f;
            return d < 0.7f * s ? 0x4A4830u : c;
        });
    }
}

// a small plaque for captions (title, tabs): dark with a frame; gold when active
void plaque(Canvas& cv, float x, float y, float w, float h, float s, bool active) {
    float r = h / 2;
    rr_solid(cv, x + 2 * s, y + 3 * s, w, h, r, 0x000000, 0.35f);
    rr_solid(cv, x, y, w, h, r, 0x2E2A1A, 0.97f);
    rr_fill(cv, x + 1.5f * s, y + 1.5f * s, w - 3 * s, h - 3 * s, r - 1.5f * s, 0.97f, [&](int, int py) {
        float t = (py - y) / h;
        return active ? mix(0xE8D890, 0x9C8A48, t) : mix(0xB8B48C, 0x6E6C50, t);
    });
    rr_fill(cv, x + 4.5f * s, y + 4.5f * s, w - 9 * s, h - 9 * s, r - 4.5f * s, 0.97f, [&](int, int py) {
        float t = (py - y) / h;
        return active ? mix(0x4A3A1C, 0x2A1E0C, t) : mix(0x34364A, 0x20222E, t);
    });
}

// text with a soft glow around it (the game's dark text on light panels, light text on dark)
void glow_text(HDC dc, const std::wstring& t, RECT r, UINT fmt, COLORREF fg, COLORREF glow, int rad) {
    SetTextColor(dc, glow);
    for (int dy = -rad; dy <= rad; dy++)
        for (int dx = -rad; dx <= rad; dx++) {
            if (dx * dx + dy * dy > rad * rad + 1 || (dx == 0 && dy == 0)) continue;
            RECT q{r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
            DrawTextW(dc, t.c_str(), -1, &q, fmt);
        }
    SetTextColor(dc, fg);
    DrawTextW(dc, t.c_str(), -1, &r, fmt);
}

// GDI rendering of the menu at the requested height (BGRA, straight alpha): a title plaque, a row of
// tab plaques (one per section), a panel with the items of the shown tab, a panel with help for the
// selected item, the key hints
void render_locked() {
    if (g_items.empty()) build_items();
    g_tab = tab_of(g_sel);
    int ntab = tab_count();
    int first, end;
    tab_range(g_tab, &first, &end);
    int most = 1;  // the panel keeps one size for all tabs: the longest tab decides
    for (int t = 0; t < ntab; t++) { int f, e; tab_range(t, &f, &e); most = std::max(most, e - f); }
    const int BASE_W = 1120, ROW = 42, TOP = 152, BOTTOM = 168;
    int base_h = TOP + most * ROW + BOTTOM;
    // about nine tenths of the picture height, never wider than the window
    double s = g_target_h > 0 ? g_target_h * 0.90 / base_h : 1.0;
    if (g_target_w > 0) s = std::min(s, g_target_w * 0.82 / BASE_W);
    if (s < 0.55) s = 0.55;
    s *= clampi(config().menu_size, 100, 200) / 100.0;           // option "Größe dieses Menüs"
    if (g_target_w > 0) s = std::min(s, (double)g_target_w / BASE_W);
    if (g_target_h > 0) s = std::min(s, (double)g_target_h / (TOP + 4 * ROW + BOTTOM));
    int rows = end - first;
    int vis = most;
    if (g_target_h > 0 && base_h * s > g_target_h) vis = std::max(4, (int)((g_target_h / s - TOP - BOTTOM) / ROW));
    vis = std::min(vis, most);
    int rel = g_sel - first;  // a tab longer than the visible rows scrolls with the selection
    if (rel < g_first) g_first = rel;
    if (rel >= g_first + vis) g_first = rel + 1 - vis;
    g_first = std::max(0, std::min(g_first, std::max(0, rows - vis)));
    base_h = TOP + vis * ROW + BOTTOM;
    int W = (int)(BASE_W * s), H = (int)(base_h * s);
    float fs = (float)s;
    auto S = [s](double v) { return (int)(v * s + 0.5); };

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldbm = SelectObject(dc, bm);
    const wchar_t* FACE = L"Bahnschrift";  // close to the game's condensed menu font (Windows 10+)
    HFONT ftitle = CreateFontW(-S(32), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, FACE);
    HFONT ftab = CreateFontW(-S(23), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, FACE);
    HFONT frow = CreateFontW(-S(26), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, FACE);
    HFONT fhelp = CreateFontW(-S(23), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, FACE);
    HFONT fsmall = CreateFontW(-S(18), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, FACE);

    // layout
    std::wstring title = platform::tr(L"Einstellungen");
    SelectObject(dc, ftitle);
    SIZE tsz{};
    GetTextExtentPoint32W(dc, title.c_str(), (int)title.size(), &tsz);
    RECT title_r{S(16), S(6), S(16) + tsz.cx + S(56), S(58)};
    std::vector<std::wstring> names;
    for (auto& it : g_items) if (it.header) names.push_back(platform::tr(it.value()));
    SelectObject(dc, ftab);
    std::vector<int> wd(names.size());
    int total = 0;
    for (size_t i = 0; i < names.size(); i++) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, names[i].c_str(), (int)names[i].size(), &sz);
        wd[i] = sz.cx + S(34);
        total += wd[i];
    }
    int tab_gap = names.size() > 1 ? std::max(S(4), std::min(S(14), (W - S(32) - total) / (int)(names.size() - 1))) : 0;
    int tabs_w = total + tab_gap * (int)(names.size() > 0 ? names.size() - 1 : 0);
    int tx = std::max(S(16), (W - tabs_w) / 2), ty0 = S(70), ty1 = S(112);
    int py0 = S(124), py1 = py0 + S(28 + vis * ROW);                // item panel
    int hy0 = py1 + S(12), hy1 = hy0 + S(100);                      // help panel
    int ky0 = hy1 + S(10), ky1 = H - S(4);                          // key hints

    // shapes
    Canvas cv;
    cv.W = W;
    cv.H = H;
    cv.p.assign((size_t)W * H, 0);
    plaque(cv, (float)title_r.left, (float)title_r.top, (float)(title_r.right - title_r.left), (float)(title_r.bottom - title_r.top), fs, true);
    std::vector<RECT> tab_r(names.size());
    for (size_t i = 0; i < names.size(); i++) {
        tab_r[i] = RECT{tx, ty0, tx + wd[i], ty1};
        plaque(cv, (float)tx, (float)ty0, (float)wd[i], (float)(ty1 - ty0), fs, (int)i == g_tab);
        tx += wd[i] + tab_gap;
    }
    panel(cv, (float)S(12), (float)py0, (float)(W - S(24)), (float)(py1 - py0), fs);
    if (g_sel >= first + g_first && g_sel < first + g_first + vis) {
        int y = py0 + S(14 + (g_sel - first - g_first) * ROW);
        rr_solid(cv, (float)S(30), (float)y + fs, (float)(W - S(60)), (float)S(ROW) - 2 * fs, 8 * fs, 0xB09A58, 0.85f);
        rr_fill(cv, (float)S(31.5), (float)y + 2.5f * fs, (float)(W - S(63)), (float)S(ROW) - 5 * fs, 7 * fs, 0.80f, [&](int, int py) {
            float t = (py - y) / (float)S(ROW);
            return mix(0xE6E6E8, 0xB8B8BE, t);
        });
    }
    panel(cv, (float)S(12), (float)hy0, (float)(W - S(24)), (float)(hy1 - hy0), fs);
    rr_solid(cv, (float)S(60), (float)ky0, (float)(W - S(120)), (float)(ky1 - ky0), (ky1 - ky0) / 2.0f, 0x10121C, 0.62f);

    // text, drawn by GDI over the shapes; GDI clears the alpha of the pixels it touches, so the
    // shapes' alpha is put back afterwards
    u32* px = (u32*)bits;
    memcpy(px, cv.p.data(), cv.p.size() * 4);
    SetBkMode(dc, TRANSPARENT);
    const COLORREF NAVY = RGB(20, 26, 98), GLOW = RGB(206, 208, 214), VALUE = RGB(12, 84, 118);
    SelectObject(dc, ftitle);
    glow_text(dc, title, title_r, DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(255, 222, 120), RGB(40, 26, 6), S(2));
    SelectObject(dc, ftab);
    for (size_t i = 0; i < names.size(); i++) {
        bool on = (int)i == g_tab;
        glow_text(dc, names[i], tab_r[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE, on ? RGB(255, 226, 130) : RGB(190, 214, 236),
                  RGB(10, 14, 30), S(1.5));
    }
    if (vis < rows) {
        SelectObject(dc, fsmall);
        std::wstring m = std::wstring(g_first > 0 ? L"▲" : L"  ") + L" " + (g_first + vis < rows ? L"▼" : L"  ");
        RECT mr{S(30), py0 + S(4), W - S(28), py0 + S(16)};
        glow_text(dc, m, mr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, NAVY, GLOW, S(1));
    }
    for (int i = first + g_first; i < std::min(end, first + g_first + vis); i++) {
        const Item& it = g_items[i];
        int y = py0 + S(14 + (i - first - g_first) * ROW);
        bool sel = i == g_sel;
        SelectObject(dc, frow);
        if (sel) {
            RECT cr{S(36), y, S(64), y + S(ROW)};
            glow_text(dc, L"►", cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(170, 110, 10), GLOW, S(1.5));
        }
        RECT lr{S(66), y, W - S(360), y + S(ROW)};
        glow_text(dc, platform::tr(it.label), lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, NAVY, GLOW, S(1.2));
        std::wstring v = it.value();
        if (!v.empty()) {
            bool action = v == L"öffnen" || v == L"speichern" || v == L"laden" || v == L"beenden";
            v = platform::tr(v);
            if (sel && it.change && !action) v = L"‹  " + v + L"  ›";
            RECT vr{W - S(360), y, W - S(52), y + S(ROW)};
            glow_text(dc, v, vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, sel ? RGB(150, 40, 10) : VALUE, GLOW, S(1.2));
        }
    }
    SelectObject(dc, fhelp);
    std::wstring help = g_note.empty() ? platform::tr(item_help(g_items[g_sel].label)) : platform::tr(g_note);
    RECT hr{S(42), hy0 + S(20), W - S(42), hy1 - S(16)};
    glow_text(dc, help, hr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS, g_note.empty() ? NAVY : RGB(150, 40, 10), GLOW, S(1.2));
    SelectObject(dc, fsmall);
    RECT kr{S(70), ky0, W - S(70), ky1};
    SetTextColor(dc, RGB(220, 224, 236));
    DrawTextW(dc, platform::tr(L"L1/R1 oder Q/E: Reiter    ↑↓ Auswahl    ←→ ändern    Kreuz/Enter: auswählen    Kreis/Esc/F1: schließen"),
              -1, &kr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();
    g_px.resize((size_t)W * H);
    for (size_t k = 0; k < g_px.size(); k++) g_px[k] = (px[k] & 0xFFFFFF) | (cv.p[k] & 0xFF000000u);
    g_w = W;
    g_h = H;
    SelectObject(dc, oldbm);
    DeleteObject(bm);
    DeleteDC(dc);
    for (HFONT f : {ftitle, ftab, frow, fhelp, fsmall}) DeleteObject(f);
    g_version++;
}

void clear_confirm() {
    if (g_disc_confirm || g_quit_confirm) { g_disc_confirm = g_quit_confirm = false; g_note.clear(); }
}

void move_sel(int d) {
    int first, end;
    tab_range(tab_of(g_sel), &first, &end);
    if (end <= first) return;
    g_sel = first + ((g_sel - first + d) % (end - first) + (end - first)) % (end - first);
    clear_confirm();
}

void switch_tab(int d) {
    int n = tab_count();
    if (n <= 0) return;
    int t = ((tab_of(g_sel) + d) % n + n) % n;
    int first, end;
    tab_range(t, &first, &end);
    g_sel = first < end ? first : g_sel;
    g_first = 0;
    clear_confirm();
}

void invoke(int dir) {
    Item& it = g_items[g_sel];
    if (!it.change) return;
    it.change(dir);
}

}  // namespace

bool overlay_open() { return g_open.load(std::memory_order_acquire); }

void overlay_set_open(bool on) {
    if (on == g_open.load()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (on) {
        build_items();
        if (g_sel <= 0 || g_sel >= (int)g_items.size() || g_items[g_sel].header) g_sel = 1;
        g_disc_confirm = g_quit_confirm = false;
        g_note.clear();
        g_drawn = 0;
        g_open = true;
        kernel::g_pause_represent = true;
        kernel::g_pause_req = true;
    } else {
        g_open = false;
        kernel::g_pause_represent = false;
        kernel::g_pause_req = false;
        save_config();
    }
    g_version++;
}

bool overlay_handle_key(int vk) {
    if (!overlay_open()) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    switch (vk) {
    case VK_UP: move_sel(-1); break;
    case VK_DOWN: move_sel(1); break;
    case 'Q': case VK_PRIOR: switch_tab(-1); break;
    case 'E': case VK_NEXT: switch_tab(1); break;
    case VK_LEFT: invoke(-1); break;
    case VK_RIGHT: invoke(1); break;
    case VK_RETURN: case VK_SPACE: {
        // actions run without the lock held (they may close the menu or open windows)
        Item it = g_items[g_sel];
        g_mu.unlock();
        if (it.change) it.change(0);
        g_mu.lock();
        break;
    }
    case VK_ESCAPE: case VK_F1: case VK_BACK:
        g_mu.unlock();
        overlay_set_open(false);
        g_mu.lock();
        return true;
    default: return true;
    }
    g_version++;
    return true;
}

// controller: Back+Start opens; D-pad/stick, A (cross) and B (circle) operate the menu
void overlay_poll_controller() {
    static WORD prev = 0;
    static double next_repeat = 0;
    XINPUT_STATE xs;
    if (!pad_state(&xs)) { prev = 0; return; }
    WORD b = xs.Gamepad.wButtons;
    if (xs.Gamepad.sThumbLY > 16000) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (xs.Gamepad.sThumbLY < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (xs.Gamepad.sThumbLX < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (xs.Gamepad.sThumbLX > 16000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    WORD fresh = b & ~prev;
    double t = host_time_sec();
    const WORD dirs = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
    if (fresh & dirs) next_repeat = t + 0.35;
    else if ((b & dirs) && t >= next_repeat) { fresh |= b & dirs; next_repeat = t + 0.09; }
    prev = b;
    bool combo = ((b & XINPUT_GAMEPAD_BACK) && (b & XINPUT_GAMEPAD_START) && (fresh & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START))) ||
                 (fresh & PAD_GUIDE);  // PS button
    if (!overlay_open()) {
        if (combo) overlay_set_open(true);
        return;
    }
    if (combo || (fresh & XINPUT_GAMEPAD_B)) { overlay_set_open(false); return; }
    if (fresh & XINPUT_GAMEPAD_DPAD_UP) overlay_handle_key(VK_UP);
    if (fresh & XINPUT_GAMEPAD_DPAD_DOWN) overlay_handle_key(VK_DOWN);
    if (fresh & XINPUT_GAMEPAD_DPAD_LEFT) overlay_handle_key(VK_LEFT);
    if (fresh & XINPUT_GAMEPAD_DPAD_RIGHT) overlay_handle_key(VK_RIGHT);
    if (fresh & XINPUT_GAMEPAD_A) overlay_handle_key(VK_RETURN);
    if (fresh & XINPUT_GAMEPAD_LEFT_SHOULDER) overlay_handle_key('Q');
    if (fresh & XINPUT_GAMEPAD_RIGHT_SHOULDER) overlay_handle_key('E');
}

// note shown while a cutscene is skipped (GDI, BGRA)
static bool skip_banner(int target_h, std::vector<u32>& out, int& w, int& h, u64& version) {
    static std::vector<u32> px;
    static int bw = 0, bh = 0, for_h = 0;
    if (for_h != target_h || px.empty()) {
        double s = ui_scale(target_h, 860.0);
        int W = (int)(560 * s), H = (int)(110 * s);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ oldbm = SelectObject(dc, bm);
        RECT all{0, 0, W, H};
        HBRUSH bg = CreateSolidBrush(RGB(18, 22, 34));
        FillRect(dc, &all, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        HFONT f1 = CreateFontW(-(int)(30 * s), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT f2 = CreateFontW(-(int)(18 * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(dc, f1);
        SetTextColor(dc, RGB(235, 215, 150));
        RECT r1{0, (int)(14 * s), W, (int)(62 * s)};
        DrawTextW(dc, platform::tr(L"Zwischensequenz wird übersprungen …"), -1, &r1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, f2);
        SetTextColor(dc, RGB(160, 165, 180));
        RECT r2{0, (int)(62 * s), W, (int)(96 * s)};
        DrawTextW(dc, platform::tr(L"START: abbrechen"), -1, &r2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        GdiFlush();
        px.resize((size_t)W * H);
        const u32* src = (const u32*)bits;
        for (size_t k = 0; k < px.size(); k++) px[k] = (src[k] & 0xFFFFFF) | 0xFF000000u;
        SelectObject(dc, oldbm);
        DeleteObject(bm);
        DeleteDC(dc);
        DeleteObject(f1);
        DeleteObject(f2);
        bw = W;
        bh = H;
        for_h = target_h;
    }
    const u64 BANNER_VERSION = 0x5C1900000000ull + (u64)for_h;
    if (version != BANNER_VERSION || out.size() != px.size()) out = px;
    w = bw;
    h = bh;
    version = BANNER_VERSION;
    return true;
}

// short note at the top of the picture (savestates)
static std::mutex g_nmu;
static std::wstring g_n1, g_n2;
static double g_nuntil = 0;
static u64 g_nver = 1;
void show_notice(const std::wstring& line1, const std::wstring& line2, double seconds) {
    std::lock_guard<std::mutex> lk(g_nmu);
    g_n1 = line1;
    g_n2 = line2;
    // the Steam Deck: a little longer, a glance at the small screen is easily missed
    g_nuntil = host_time_sec() + seconds + (is_steam_deck() ? 1.5 : 0.0);
    g_nver++;
}
bool notice_active() { return host_time_sec() < g_nuntil; }

static bool notice_banner(int target_h, std::vector<u32>& out, int& w, int& h, u64& version) {
    static std::vector<u32> px;
    static int bw = 0, bh = 0, for_h = 0;
    static u64 for_ver = 0;
    std::wstring l1, l2;
    u64 ver;
    {
        std::lock_guard<std::mutex> lk(g_nmu);
        l1 = g_n1;
        l2 = g_n2;
        ver = g_nver;
    }
    if (for_h != target_h || for_ver != ver || px.empty()) {
        double s = ui_scale(target_h, 860.0);
        int W = (int)(520 * s), H = (int)((l2.empty() ? 60 : 96) * s);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ oldbm = SelectObject(dc, bm);
        RECT all{0, 0, W, H};
        HBRUSH bg = CreateSolidBrush(RGB(18, 22, 34));
        FillRect(dc, &all, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        HFONT f1 = CreateFontW(-(int)(26 * s), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT f2 = CreateFontW(-(int)(18 * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(dc, f1);
        SetTextColor(dc, RGB(235, 215, 150));
        RECT r1{0, (int)(10 * s), W, (int)(50 * s)};
        DrawTextW(dc, l1.c_str(), -1, &r1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (!l2.empty()) {
            SelectObject(dc, f2);
            SetTextColor(dc, RGB(170, 175, 190));
            RECT r2{0, (int)(50 * s), W, (int)(84 * s)};
            DrawTextW(dc, l2.c_str(), -1, &r2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        GdiFlush();
        px.resize((size_t)W * H);
        const u32* src = (const u32*)bits;
        for (size_t k = 0; k < px.size(); k++) px[k] = (src[k] & 0xFFFFFF) | 0xE8000000u;
        SelectObject(dc, oldbm);
        DeleteObject(bm);
        DeleteDC(dc);
        DeleteObject(f1);
        DeleteObject(f2);
        bw = W;
        bh = H;
        for_h = target_h;
        for_ver = ver;
    }
    const u64 V = 0x707100000000ull + (for_ver << 16) + (u64)for_h;
    if (version != V || out.size() != px.size()) out = px;
    w = bw;
    h = bh;
    version = V;
    return true;
}

// frame rate label (option show_fps), drawn again when the number changes
bool fps_pixels(int target_h, int fps, std::vector<u32>& out, int& w, int& h, u64& version) {
    static std::vector<u32> px;
    static int bw = 0, bh = 0, for_h = 0, for_fps = -1;
    if (for_h != target_h || for_fps != fps || px.empty()) {
        double s = ui_scale(target_h, 860.0);
        int W = (int)(118 * s), H = (int)(40 * s);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ oldbm = SelectObject(dc, bm);
        RECT all{0, 0, W, H};
        HBRUSH bg = CreateSolidBrush(RGB(18, 22, 34));
        FillRect(dc, &all, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        HFONT f = CreateFontW(-(int)(24 * s), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(dc, f);
        // green at the target rate, yellow a bit below, red when clearly slower; fps < 0: fast forward
        SetTextColor(dc, fps < 0 ? RGB(120, 200, 255) : fps >= 57 || (fps >= 28 && fps <= 31) ? RGB(130, 230, 140) : fps >= 45 ? RGB(240, 210, 110) : RGB(240, 120, 110));
        wchar_t t[32];
        if (fps < 0) swprintf(t, 32, L"»» 4x");
        else swprintf(t, 32, L"%d FPS", fps);
        RECT r{0, 0, W, H};
        DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        GdiFlush();
        px.resize((size_t)W * H);
        const u32* src = (const u32*)bits;
        for (size_t k = 0; k < px.size(); k++) px[k] = (src[k] & 0xFFFFFF) | 0xC0000000u;
        SelectObject(dc, oldbm);
        DeleteObject(bm);
        DeleteDC(dc);
        DeleteObject(f);
        bw = W;
        bh = H;
        for_h = target_h;
        for_fps = fps;
    }
    const u64 V = 0xF95000000000ull + ((u64)(u32)for_fps << 16) + (u64)for_h;
    if (version != V || out.size() != px.size()) out = px;
    w = bw;
    h = bh;
    version = V;
    return true;
}

void overlay_set_target_w(int w) { g_target_w = w; }

bool overlay_pixels(int target_h, std::vector<u32>& out, int& w, int& h, u64& version) {
    if (!overlay_open()) {
        if (skip_active()) return skip_banner(target_h, out, w, h, version);
        return notice_active() && notice_banner(target_h, out, w, h, version);
    }
    std::lock_guard<std::mutex> lk(g_mu);
    static int drawn_w = 0;
    if (target_h != g_target_h || g_drawn != g_version || g_target_w != drawn_w) {
        g_target_h = target_h;
        drawn_w = g_target_w;
        render_locked();
        g_drawn = g_version;
    }
    if (version != g_version || out.size() != g_px.size()) out = g_px;  // the caller passes its last version
    w = g_w;
    h = g_h;
    version = g_version;
    return true;
}

}  // namespace platform
