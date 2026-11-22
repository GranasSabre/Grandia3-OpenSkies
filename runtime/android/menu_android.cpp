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
#include "vk_ctx.h"
#include <android/native_activity.h>
#include <dirent.h>
#include <sys/stat.h>
#include <thread>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" int g_ee_patch[8];
extern ANativeActivity* g3_activity;                                       // platform_android.cpp
std::string g3_pick_document();                                            // jni_android.cpp
std::string g3_install_driver(const std::string& uri, const std::string& base);  // jni_android.cpp

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
struct Geo {
    int x0 = 0, y0 = 0, w = 0, h = 0, rows_y = 0, row_h = 1, vis = 0, val_x = 0, list_x0 = 0, list_x1 = 0;
    std::vector<int> tab_x0, tab_x1;
    int tab_y0 = 0, tab_y1 = 0;
} g_geo;
int g_tab = 0;

// texts of the Android menu that the PC's table (i18n.cpp) does not have
const wchar_t* tra(const wchar_t* de) {
    static const wchar_t* const T[][2] = {
        {L"Spiel", L"Game"},
        {L"Bild", L"Picture"},
        {L"Spielablauf", L"Gameplay"},
        {L"Sprache und Text", L"Language and text"},
        {L"Reiter antippen    Eintrag antippen, nochmal: ändern    ‹ ›: Zahl    außerhalb: schließen",
         L"Tap a tab    tap an item, again: change    ‹ ›: number    outside: close"},
        {L"L1/R1: Reiter    ↑↓ Auswahl    ←→ ändern    Kreuz: auswählen    Kreis: schließen",
         L"L1/R1: tabs    ↑↓ select    ←→ change    cross: choose    circle: close"},
        {L"Legt die Bildschärfe fest: „Automatisch“ passt zum Handy; höher kostet Grafikleistung.",
         L"How finely the picture is drawn: “Automatic” suits the phone; higher costs graphics power."},
        {L"Blendet die Touch-Steuerung nach 5 Sekunden ohne Berührung aus; eine Berührung holt sie zurück.",
         L"Hides the touch controls after 5 seconds without a touch; a touch brings them back."},
        {L"Wie schnell das Wischen auf dem Bildschirm die Kamera dreht.", L"How fast a swipe on the screen turns the camera."},
        {L"Zeigt das Steuerkreuz der Touch-Steuerung. Der linke Stick bedient auch die Menüs.",
         L"Shows the d-pad of the touch controls. The left stick works in the menus too."},
        {L"Startet beim nächsten Mal mit der anderen Disc. Nicht gespeicherter Fortschritt geht verloren.",
         L"Starts with the other disc next time. Unsaved progress is lost."},
        {L"Beim nächsten Start fragt die App erneut nach dem Disc-Abbild.", L"At the next start the app asks for the disc image again."},
        {L"Eigener Vulkan-Treiber (z. B. Turnip) statt des Handy-Treibers; wirkt beim nächsten Start.",
         L"A custom Vulkan driver (e.g. Turnip) instead of the phone's own; used from the next start."},
        {L"Wählt eine Treiber-Zip aus (z. B. Turnip für Snapdragon) und installiert sie in der App.",
         L"Picks a driver zip (e.g. Turnip for Snapdragon) and installs it in the app."},
        {L"Schließt das Menü (auch Kreis oder außerhalb tippen).", L"Closes the menu (also circle or tap outside)."},
        {L"Speichert genau diese Stelle im gewählten Slot.", L"Saves exactly this moment in the chosen slot."},
        {L"Savestate speichern", L"Save savestate"},
        {L"Savestate laden", L"Load savestate"},
        {L"Disc wechseln (beim nächsten Start)", L"Change disc (at the next start)"},
        {L"Disc-Abbild neu wählen (beim nächsten Start)", L"Choose the disc image again (at the next start)"},
        {L"wählen", L"choose"},
        {L"GPU-Treiber (ab Neustart)", L"GPU driver (from the next start)"},
        {L"GPU-Treiber hinzufügen (Zip, z. B. Turnip)", L"Add a GPU driver (zip, e.g. Turnip)"},
        {L"System", L"System"},
        {L" (nicht geladen)", L" (not loaded)"},
        {L"Treiber installiert, wirkt beim nächsten Start.", L"Driver installed, used from the next start."},
        {L"Treiber-Zip konnte nicht entpackt werden.", L"The driver zip could not be unpacked."},
        {L"Der gewählte Treiber ließ sich nicht starten; der Systemtreiber läuft.", L"The chosen driver did not start; the system driver runs."},
        {L"Touch-Steuerung nach 5 s ausblenden", L"Hide the touch controls after 5 s"},
        {L"Kamera drehen durch Wischen: Tempo", L"Camera swipe speed"},
        {L"Steuerkreuz anzeigen", L"Show the d-pad"},
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

// one line of help per item, shown under the list (the PC's texts, i18n.cpp has their English; the
// Android-only ones are in tra() above)
const wchar_t* item_help(const wchar_t* label) {
    static const wchar_t* const H[][2] = {
        {L"Slot", L"Wähle einen von 10 Plätzen. „Auto“ wird nach jedem Kartenwechsel von selbst beschrieben."},
        {L"Savestate speichern", L"Speichert genau diese Stelle im gewählten Slot."},
        {L"Savestate laden", L"Lädt den gewählten Slot. Nicht gespeicherter Fortschritt geht verloren."},
        {L"Nach Kartenwechsel automatisch (Slot Auto)", L"Legt ein paar Sekunden nach jedem Kartenwechsel einen Savestate im Slot „Auto“ an."},
        {L"Widescreen", L"Die 3D-Welt wird breiter statt gestreckt. Für echtes 4:3 ausschalten."},
        {L"Menüs/Texte im 4:3-Format", L"Menüs, Textfenster und Anzeigen behalten im Breitbild ihre ursprüngliche Form."},
        {L"Interne Auflösung (ab Neustart)", L"Legt die Bildschärfe fest: „Automatisch“ passt zum Handy; höher kostet Grafikleistung."},
        {L"Scharfe Schrift", L"Glättet die Schrift der Spieltexte bei hoher Auflösung."},
        {L"Kinobalken entfernen (experimentell)", L"Entfernt die schwarzen Balken in Zwischensequenzen und Kämpfen."},
        {L"60 fps (experimentell)", L"Doppelte Bildrate beim Laufen durch die Welt. Zwischensequenzen bleiben bei 30."},
        {L"60 fps auch in Kämpfen (experimentell)", L"60 fps auch in Kämpfen. Braucht mehr Leistung."},
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
        {L"Touch-Steuerung nach 5 s ausblenden", L"Blendet die Touch-Steuerung nach 5 Sekunden ohne Berührung aus; eine Berührung holt sie zurück."},
        {L"Kamera drehen durch Wischen: Tempo", L"Wie schnell das Wischen auf dem Bildschirm die Kamera dreht."},
        {L"Steuerkreuz anzeigen", L"Zeigt das Steuerkreuz der Touch-Steuerung. Der linke Stick bedient auch die Menüs."},
        {L"Kamera umkehren (rechter Stick links/rechts)", L"Dreht die Richtung des rechten Sticks links/rechts um."},
        {L"Disc wechseln (beim nächsten Start)", L"Startet beim nächsten Mal mit der anderen Disc. Nicht gespeicherter Fortschritt geht verloren."},
        {L"Disc-Abbild neu wählen (beim nächsten Start)", L"Beim nächsten Start fragt die App erneut nach dem Disc-Abbild."},
        {L"GPU-Treiber (ab Neustart)", L"Eigener Vulkan-Treiber (z. B. Turnip) statt des Handy-Treibers; wirkt beim nächsten Start."},
        {L"GPU-Treiber hinzufügen (Zip, z. B. Turnip)", L"Wählt eine Treiber-Zip aus (z. B. Turnip für Snapdragon) und installiert sie in der App."},
        {L"Zurück zum Spiel", L"Schließt das Menü (auch Kreis oder außerhalb tippen)."},
    };
    if (!label) return L"";
    for (auto& h : H)
        if (wcscmp(h[0], label) == 0) return h[1];
    return L"";
}

// custom GPU drivers: folders in the app's internal files/gpu_drivers (vk_dyn.cpp)
std::string gpu_driver_base() {
    std::string b = std::string(g3_activity && g3_activity->internalDataPath ? g3_activity->internalDataPath : ".") + "/gpu_drivers";
    mkdir(b.c_str(), 0700);
    return b;
}
std::vector<std::string> gpu_drivers() {
    std::vector<std::string> v;
    if (DIR* d = opendir(gpu_driver_base().c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_type == DT_DIR && e->d_name[0] != '.') v.push_back(e->d_name);
        closedir(d);
    }
    std::sort(v.begin(), v.end());
    return v;
}
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
    g_items.push_back({nullptr, [] { return std::wstring(L"Bild"); }, nullptr, true});
    g_items.push_back({L"Widescreen", [&c] { return onoff(c.widescreen_hack); }, drawn(c.widescreen_hack, [] { apply_patches(); })});
    g_items.push_back({L"Menüs/Texte im 4:3-Format", [&c] { return onoff(c.hud_fix); }, drawn(c.hud_fix)});
    g_items.push_back({L"Interne Auflösung (ab Neustart)", [&c] {
                           return c.internal_scale ? std::to_wstring(c.internal_scale) + L"x" : std::wstring(tr(L"Automatisch"));
                       },
                       [&c](int dir) { c.internal_scale = (c.internal_scale + (dir < 0 ? 4 : 1)) % 5; }});
    g_items.push_back({L"Scharfe Schrift", [&c] { return onoff(c.sharp_text); }, drawn(c.sharp_text)});
    g_items.push_back({L"Kinobalken entfernen (experimentell)", [&c] { return onoff(c.remove_letterbox); },
                       drawn(c.remove_letterbox, [&c] { g_ee_patch[1] = g_ee_patch[2] = c.remove_letterbox; })});
    g_items.push_back({L"60 fps (experimentell)", [&c] { return onoff(c.fps60); }, toggle(c.fps60, [&c] { kernel::g_force60 = c.fps60; })});
    g_items.push_back({L"60 fps auch in Kämpfen (experimentell)", [&c] { return onoff(c.fps60_battle); }, toggle(c.fps60_battle)});
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
                           g_note = L"Wirkt beim nächsten Start der App.";
                       }});
    g_items.push_back({L"Untertitel in Zwischensequenzen", [&c] { return onoff(c.subtitles); }, toggle(c.subtitles)});
    g_items.push_back({L"Undub: japanische Stimmen (ab Neustart)", [&c] { return onoff(c.jp_voices); }, toggle(c.jp_voices)});
    g_items.push_back({L"Große Dialogfenster", [&c] {
                           if (c.big_text == 1 && !c.widescreen_hack) return std::wstring(L"braucht Widescreen");
                           return onoff(c.big_text == 1);
                       },
                       [&c](int) {
                           c.big_text = c.big_text == 1 ? 0 : 1;
                           kernel::g_preview_vblanks = 8;
                       }});
    g_items.push_back({nullptr, [] { return std::wstring(L"Audio"); }, nullptr, true});
    g_items.push_back({L"Lautstärke gesamt", [&c] { return std::to_wstring(c.volume) + L" %"; }, pct(c.volume, 5)});
    g_items.push_back({L"Musik", [&c] { return std::to_wstring(c.vol_music) + L" %"; }, pct(c.vol_music, 5)});
    g_items.push_back({L"Sprache", [&c] { return std::to_wstring(c.vol_voice) + L" %"; }, pct(c.vol_voice, 5)});
    g_items.push_back({L"Effekte", [&c] { return std::to_wstring(c.vol_sfx) + L" %"; }, pct(c.vol_sfx, 5)});
    g_items.push_back({L"Videos", [&c] { return std::to_wstring(c.vol_movie) + L" %"; }, pct(c.vol_movie, 5)});
    g_items.push_back({nullptr, [] { return std::wstring(L"Steuerung"); }, nullptr, true});
    g_items.push_back({L"Touch-Steuerung nach 5 s ausblenden", [&c] { return onoff(c.touch_autohide); },
                       toggle(c.touch_autohide, [&c] { touch::set_autohide(c.touch_autohide); })});
    g_items.push_back({L"Kamera drehen durch Wischen: Tempo", [&c] { return std::to_wstring(c.touch_cam_speed); }, [&c](int dir) {
                           c.touch_cam_speed = clampi(c.touch_cam_speed + (dir ? dir : 1), 1, 15);
                           touch::set_camera_speed(c.touch_cam_speed);
                       }});
    g_items.push_back({L"Steuerkreuz anzeigen", [&c] { return onoff(c.touch_dpad); },
                       toggle(c.touch_dpad, [&c] { touch::set_dpad(c.touch_dpad); })});
    g_items.push_back({L"Kamera umkehren (rechter Stick links/rechts)", [&c] { return onoff(c.camera_invert_x); }, toggle(c.camera_invert_x)});
    g_items.push_back({nullptr, [] { return std::wstring(L"System"); }, nullptr, true});
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
    // custom Vulkan driver (vk_dyn.cpp): the phone's own ("System") or one unpacked into files/gpu_drivers
    g_items.push_back({L"GPU-Treiber (ab Neustart)", [] {
                           std::string d = g3_gpu_driver_setting();
                           if (d.empty()) return std::wstring(tra(L"System"));
                           std::wstring w(d.begin(), d.end());
                           // chosen at the last start but not running (the phone's own driver answered)
                           if (vkc::driver_fallback() && d == g3_gpu_driver_setting()) w += tra(L" (nicht geladen)");
                           return w;
                       },
                       [](int dir) {
                           std::vector<std::string> opts{""};
                           for (auto& n : gpu_drivers()) opts.push_back(n);
                           std::string cur = g3_gpu_driver_setting();
                           int i = 0;
                           for (int k = 0; k < (int)opts.size(); k++)
                               if (opts[k] == cur) i = k;
                           int n = (int)opts.size();
                           i = ((i + (dir < 0 ? -1 : 1)) % n + n) % n;
                           g3_set_gpu_driver_setting(opts[i]);
                           save_config();
                           g_note = L"Wirkt beim nächsten Start der App.";
                       }});
    g_items.push_back({L"GPU-Treiber hinzufügen (Zip, z. B. Turnip)", [] { return std::wstring(L"wählen"); }, [](int dir) {
                           if (dir != 0) return;
                           // the picker blocks until the player chose a file: on a thread of its own
                           std::thread([] {
                               std::string uri = g3_pick_document();
                               if (uri.empty()) return;
                               std::string name = g3_install_driver(uri, gpu_driver_base());
                               std::lock_guard<std::recursive_mutex> lk(g_mu);
                               if (name.empty()) {
                                   g_note = L"Treiber-Zip konnte nicht entpackt werden.";
                               } else {
                                   g3_set_gpu_driver_setting(name);
                                   save_config();
                                   g_note = L"Treiber installiert, wirkt beim nächsten Start.";
                               }
                               g_version++;
                           }).detach();
                       }});
    g_items.push_back({L"Zurück zum Spiel", [] { return std::wstring(); }, [](int dir) {
                           if (dir == 0) overlay_set_open(false);
                       }});
}

bool is_action(const std::wstring& v) { return v == L"speichern" || v == L"laden" || v == L"wählen"; }
// a number setting (volume, speed, x2 ...): shown with arrows, a tap on an arrow changes it right away
bool is_number(const std::wstring& v) {
    size_t i = (!v.empty() && v[0] == L'x') ? 1 : 0;
    return v.size() > i && v[i] >= L'0' && v[i] <= L'9';
}

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

// tabs: every section header starts one; its items follow up to the next header
void tab_range(int t, int* first, int* end) {
    int n = (int)g_items.size(), k = -1;
    *first = 0;
    *end = n;
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
// Drawing in the look of the game's own menus (as the PC's F1 menu, overlay.cpp): light brushed panels
// in an olive-gold frame with rivets in the corners, dark blue text with a light glow, captions on
// small plaques. Canvas: straight alpha, 0xAARRGGBB; turned into premultiplied 0xAABBGGRR at the end.
// ---------------------------------------------------------------------------------------------------
struct Canvas {
    int W = 0, H = 0;
    std::vector<u32> p;
};
inline void over(u32& d, u32 rgb, float a) {
    if (a <= 0) return;
    if (a > 1) a = 1;
    float da = (d >> 24) / 255.0f, oa = a + da * (1 - a);
    if (oa <= 0) return;
    auto ch = [&](int sh) {
        float c = ((rgb >> sh) & 255) * a + ((d >> sh) & 255) * da * (1 - a);
        return (u32)std::min(255.0f, c / oa + 0.5f);
    };
    d = ((u32)(oa * 255 + 0.5f) << 24) | ch(16) << 16 | ch(8) << 8 | ch(0);
}
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
void panel(Canvas& cv, float x, float y, float w, float h, float s, float alpha = 0.97f) {
    const u32 OUTLINE = 0x3C3A26, FRAME = 0xB4AE72, FRAME_HI = 0xDCD69C, FRAME_LO = 0x7C7848;
    float r = 13 * s, b = 8 * s;
    rr_solid(cv, x + 3 * s, y + 4 * s, w, h, r, 0x000000, 0.35f * alpha);
    rr_solid(cv, x, y, w, h, r, OUTLINE, alpha);
    rr_fill(cv, x + 1.5f * s, y + 1.5f * s, w - 3 * s, h - 3 * s, r - 1.5f * s, alpha, [&](int px, int py) {
        float t = ((py - y) / h + (px - x) / w * 0.35f);
        return t < 0.5f ? mix(FRAME_HI, FRAME, t * 2) : mix(FRAME, FRAME_LO, (t - 0.5f) / 0.85f);
    });
    rr_solid(cv, x + b, y + b, w - 2 * b, h - 2 * b, r - b * 0.6f, 0x4A4830, alpha);
    float ix = x + b + 1.5f * s, iy = y + b + 1.5f * s, iw = w - 2 * b - 3 * s, ih = h - 2 * b - 3 * s;
    rr_fill(cv, ix, iy, iw, ih, r - b * 0.7f, alpha, [&](int px, int py) {
        float t = (py - iy) / ih, dg = ((px - ix) / iw + t) * 0.5f;
        static const u32 STOP[9] = {0xCCBEAE, 0xBEB0A2, 0xAA9E94, 0x8C817E, 0x726A6E, 0x60595A, 0x544E4E, 0x494441, 0x3E3A36};
        float q = std::min(1.0f, std::max(0.0f, dg)) * 8;
        int qi = std::min(7, (int)q);
        u32 base = mix(STOP[qi], STOP[qi + 1], q - qi);
        int row = (int)((py - iy) / std::max(1.0f, s));
        float u = (px - ix) / (60 * s);
        int ui = (int)u;
        float f = u - ui, n = hash1(row, ui) * (1 - f) + hash1(row, ui + 1) * f, n2 = hash1(row / 3, 7);
        return shade(base, 0.96f + 0.06f * n + 0.03f * n2);
    });
    float rr = 4.2f * s, off = b * 0.5f + 0.75f * s;
    for (int k = 0; k < 4; k++) {
        float cx = (k & 1) ? x + w - off - 1.5f * s : x + off + 1.5f * s;
        float cy = (k & 2) ? y + h - off - 1.5f * s : y + off + 1.5f * s;
        rr_solid(cv, cx - rr, cy - rr, 2 * rr, 2 * rr, rr, 0x3C3A26, alpha);
        rr_fill(cv, cx - rr + s, cy - rr + s, 2 * rr - 2 * s, 2 * rr - 2 * s, rr - s, alpha, [&](int px, int py) {
            float t = ((px - cx) + (py - cy)) / (2 * rr) + 0.5f;
            u32 c = mix(0xE6E2BE, 0x8A8660, std::min(1.0f, std::max(0.0f, t)));
            float d = fabsf((px + 0.5f - cx) - (py + 0.5f - cy)) / 1.414f;
            return d < 0.7f * s ? 0x4A4830u : c;
        });
    }
}
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
// text (system text layout via JNI) with a soft glow; x = left edge (right edge when right), centred on cy;
// align 1 = centred on x; clipped to [clip0, clip1)
void text(Canvas& cv, const std::wstring& t, int fs, int x, int cy, u32 fg, u32 glow, float rad, int align = 0, int clip0 = 0,
          int clip1 = 1 << 30) {
    const Cov& c = cov(t, fs);
    if (c.w <= 0) return;
    int x0 = align == 2 ? x - c.w : (align == 1 ? x - c.w / 2 : x), y0 = cy - c.h / 2;
    int ri = (int)ceilf(rad);
    auto put = [&](int ox, int oy, u32 rgb, float k) {
        for (int y = 0; y < c.h; y++) {
            int py = y0 + y + oy;
            if (py < 0 || py >= cv.H) continue;
            for (int q = 0; q < c.w; q++) {
                int px = x0 + q + ox;
                if (px < std::max(0, clip0) || px >= std::min(cv.W, clip1)) continue;
                u8 a = c.t[(size_t)y * c.w + q];
                if (a) over(cv.p[(size_t)py * cv.W + px], rgb, a / 255.0f * k);
            }
        }
    };
    if (ri > 0)
        for (int dy = -ri; dy <= ri; dy++)
            for (int dx = -ri; dx <= ri; dx++) {
                if (dx == 0 && dy == 0) continue;
                float d = sqrtf((float)(dx * dx + dy * dy));
                if (d > rad + 0.5f) continue;
                put(dx, dy, glow, 0.30f);
            }
    put(0, 0, fg, 1.0f);
}
int text_w(const std::wstring& t, int fs) { return cov(t, fs).w; }

struct Panel {
    std::vector<u32> px;  // premultiplied RGBA (R in the low byte)
    int w = 0, h = 0;
};
Panel g_panel;
u64 g_drawn = 0;
int g_for_w = 0, g_for_h = 0;

void render(int ow, int oh) {
    if (g_items.empty()) build_items();
    g_tab = tab_of(g_sel);
    int ntab = tab_count(), first, end;
    tab_range(g_tab, &first, &end);
    int most = 1;  // one panel size for all tabs: the longest tab decides
    for (int t = 0; t < ntab; t++) { int f, e; tab_range(t, &f, &e); most = std::max(most, e - f); }
    const int BASE_W = 1120, ROW = 42, TOP = 152, BOTTOM = 168;
    int base_h = TOP + most * ROW + BOTTOM;
    double s = std::min(oh * 0.94 / base_h, ow * 0.86 / BASE_W);
    s = std::max(s, 0.3);
    int rows = end - first, vis = most;
    if (base_h * s > oh) vis = std::max(4, (int)((oh / s - TOP - BOTTOM) / ROW));
    vis = std::min(vis, most);
    int rel = g_sel - first;
    if (rel < g_first) g_first = rel;
    if (rel >= g_first + vis) g_first = rel + 1 - vis;
    g_first = std::max(0, std::min(g_first, std::max(0, rows - vis)));
    base_h = TOP + vis * ROW + BOTTOM;
    int W = (int)(BASE_W * s), H = (int)(base_h * s);
    float fs = (float)s;
    auto S = [s](double v) { return (int)(v * s + 0.5); };
    int f_title = S(32), f_tab = S(23), f_row = S(26), f_help = S(23), f_small = S(18);

    std::wstring title = tra(L"Einstellungen");
    int title_w = text_w(title, f_title) + S(56);
    std::vector<std::wstring> names;
    for (auto& it : g_items) if (it.header) names.push_back(tra(it.value()));
    std::vector<int> wd(names.size());
    int total = 0;
    for (size_t i = 0; i < names.size(); i++) { wd[i] = text_w(names[i], f_tab) + S(34); total += wd[i]; }
    int gap = names.size() > 1 ? std::max(S(4), std::min(S(14), (W - S(32) - total) / (int)(names.size() - 1))) : 0;
    int tabs_w = total + gap * (int)(names.size() > 0 ? names.size() - 1 : 0);
    int tx = std::max(S(16), (W - tabs_w) / 2), ty0 = S(70), ty1 = S(112);
    int py0 = S(124), py1 = py0 + S(28 + vis * ROW);
    int hy0 = py1 + S(12), hy1 = hy0 + S(100);
    int ky0 = hy1 + S(10), ky1 = H - S(4);

    Canvas cv;
    cv.W = W;
    cv.H = H;
    cv.p.assign((size_t)W * H, 0);
    plaque(cv, (float)S(16), (float)S(6), (float)title_w, (float)S(52), fs, true);
    g_geo.tab_x0.assign(names.size(), 0);
    g_geo.tab_x1.assign(names.size(), 0);
    for (size_t i = 0; i < names.size(); i++) {
        plaque(cv, (float)tx, (float)ty0, (float)wd[i], (float)(ty1 - ty0), fs, (int)i == g_tab);
        g_geo.tab_x0[i] = tx;
        g_geo.tab_x1[i] = tx + wd[i];
        tx += wd[i] + gap;
    }
    panel(cv, (float)S(12), (float)py0, (float)(W - S(24)), (float)(py1 - py0), fs);
    if (g_sel >= first + g_first && g_sel < first + g_first + vis) {
        int y = py0 + S(14 + (g_sel - first - g_first) * ROW);
        rr_solid(cv, (float)S(30), (float)y + fs, (float)(W - S(60)), (float)S(ROW) - 2 * fs, 8 * fs, 0xB09A58, 0.85f);
        rr_fill(cv, (float)S(31.5), (float)y + 2.5f * fs, (float)(W - S(63)), (float)S(ROW) - 5 * fs, 7 * fs, 0.80f,
                [&](int, int py) { return mix(0xE6E6E8, 0xB8B8BE, (py - y) / (float)S(ROW)); });
    }
    panel(cv, (float)S(12), (float)hy0, (float)(W - S(24)), (float)(hy1 - hy0), fs);
    rr_solid(cv, (float)S(60), (float)ky0, (float)(W - S(120)), (float)(ky1 - ky0), (ky1 - ky0) / 2.0f, 0x10121C, 0.62f);

    const u32 NAVY = 0x141A62, GLOW = 0xCED0D6, VALUE = 0x0C5476, VALSEL = 0x96280A;
    text(cv, title, f_title, S(16) + title_w / 2, S(32), 0xFFDE78, 0x281A06, 2 * fs, 1);
    for (size_t i = 0; i < names.size(); i++)
        text(cv, names[i], f_tab, (g_geo.tab_x0[i] + g_geo.tab_x1[i]) / 2, (ty0 + ty1) / 2, (int)i == g_tab ? 0xFFE282 : 0xBED6EC,
             0x0A0E1E, 1.5f * fs, 1);
    if (vis < rows) {
        std::wstring m = std::wstring(g_first > 0 ? L"▲" : L"  ") + L" " + (g_first + vis < rows ? L"▼" : L"  ");
        text(cv, m, f_small, W - S(28), py0 + S(10), NAVY, GLOW, fs, 2);
    }
    int val_x = W - S(360);
    for (int i = first + g_first; i < std::min(end, first + g_first + vis); i++) {
        const Item& it = g_items[i];
        int y = py0 + S(14 + (i - first - g_first) * ROW), cy = y + S(ROW) / 2;
        bool sel = i == g_sel;
        if (sel) text(cv, L"►", f_row, S(50), cy, 0xAA6E0A, GLOW, 1.5f * fs, 1);
        text(cv, tra(it.label), f_row, S(66), cy, NAVY, GLOW, 1.2f * fs, 0, 0, val_x - S(10));
        std::wstring v = it.value();
        if (!v.empty()) {
            bool action = is_action(v);
            v = tra(v);
            if (it.change && !action && (sel || is_number(it.value()))) v = L"‹  " + v + L"  ›";
            text(cv, v, f_row, W - S(52), cy, sel ? VALSEL : VALUE, GLOW, 1.2f * fs, 2, val_x - S(40));
        }
    }
    // help for the selected item (or a note), word-wrapped by the system text layout into the panel
    std::wstring help = g_note.empty() ? tra(item_help(g_items[g_sel].label)) : tra(g_note);
    if (!help.empty()) {
        Cov hc;
        if (text_coverage(utf8(help), f_help, W - S(84), 0, hc.t, hc.w, hc.h) && hc.w > 0) {
            int hx = S(42), hy = (hy0 + hy1) / 2 - hc.h / 2;
            u32 fg = g_note.empty() ? NAVY : VALSEL;
            int ri = (int)ceilf(1.2f * fs);
            for (int pass = 0; pass < 2; pass++)
                for (int dy = pass ? 0 : -ri; dy <= (pass ? 0 : ri); dy++)
                    for (int dx = pass ? 0 : -ri; dx <= (pass ? 0 : ri); dx++) {
                        if (!pass && dx == 0 && dy == 0) continue;
                        for (int y = 0; y < hc.h; y++) {
                            int py = hy + y + dy;
                            if (py < hy0 || py >= hy1) continue;
                            for (int q = 0; q < hc.w; q++) {
                                int px = hx + q + dx;
                                u8 a = hc.t[(size_t)y * hc.w + q];
                                if (a && px >= 0 && px < W) over(cv.p[(size_t)py * W + px], pass ? fg : GLOW, a / 255.0f * (pass ? 1 : 0.30f));
                            }
                        }
                    }
        }
    }
    bool pad_ui = touch::pad_active();
    text(cv, tra(pad_ui ? L"L1/R1: Reiter    ↑↓ Auswahl    ←→ ändern    Kreuz: auswählen    Kreis: schließen"
                        : L"Reiter antippen    Eintrag antippen, nochmal: ändern    ‹ ›: Zahl    außerhalb: schließen"),
         f_small, W / 2, (ky0 + ky1) / 2, 0xDCE0EC, 0, 0, 1);

    // to premultiplied RGBA with R in the low byte
    Panel& p = g_panel;
    p.w = W;
    p.h = H;
    p.px.resize((size_t)W * H);
    for (size_t k = 0; k < p.px.size(); k++) {
        u32 c = cv.p[k], a = c >> 24;
        u32 r = ((c >> 16) & 255) * a / 255, g = ((c >> 8) & 255) * a / 255, bl = (c & 255) * a / 255;
        p.px[k] = a << 24 | bl << 16 | g << 8 | r;
    }
    g_geo.w = W;
    g_geo.h = H;
    g_geo.rows_y = py0 + S(14);
    g_geo.row_h = std::max(1, S(ROW));
    g_geo.vis = vis;
    g_geo.val_x = val_x;
    g_geo.tab_y0 = ty0;
    g_geo.tab_y1 = ty1;
    g_geo.list_x0 = S(12);
    g_geo.list_x1 = W - S(12);
}

// up/down stays inside the tab (wrapping); L1/R1 switch the tab
void move_sel(int d) {
    int first, end;
    tab_range(tab_of(g_sel), &first, &end);
    if (end <= first) return;
    g_sel = first + ((g_sel - first + d) % (end - first) + (end - first)) % (end - first);
    if (g_disc_confirm) { g_disc_confirm = false; g_note.clear(); }
}
void switch_tab(int to) {
    int n = tab_count();
    if (n <= 0) return;
    to = (to % n + n) % n;
    int first, end;
    tab_range(to, &first, &end);
    if (end > first) g_sel = first;
    g_first = 0;
    if (g_disc_confirm) { g_disc_confirm = false; }
    g_note.clear();
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

// controller / keys: 0 up, 1 down, 2 left, 3 right, 4 confirm, 5 close, 6/7 previous/next tab
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
    case 6: switch_tab(tab_of(g_sel) - 1); break;
    case 7: switch_tab(tab_of(g_sel) + 1); break;
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
        // dragging scrolls the tab's list (the selection follows into view)
        if (std::fabs(dy) > g.row_h * 0.6f) g_tmoved = true;
        if (g_tmoved) {
            int first, end;
            tab_range(tab_of(g_sel), &first, &end);
            int rows = end - first;
            int f = std::clamp(g_tfirst - (int)std::lround(dy / g.row_h), 0, std::max(0, rows - g.vis));
            if (f != g_first) {
                g_first = f;
                if (g_sel - first < f) g_sel = first + f;
                if (g_sel - first > f + g.vis - 1) g_sel = first + f + g.vis - 1;
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
    // a tab plaque
    if (py >= g.tab_y0 && py < g.tab_y1) {
        for (int t = 0; t < (int)g.tab_x0.size(); t++)
            if (px >= g.tab_x0[t] && px < g.tab_x1[t]) {
                if (t != tab_of(g_sel)) switch_tab(t);
                g_version++;
                return;
            }
        return;
    }
    if (px < g.list_x0 || px >= g.list_x1) return;
    int r = (int)std::floor((py - g.rows_y) / g.row_h);
    if (r < 0 || r >= g.vis) return;
    int first, end;
    tab_range(tab_of(g_sel), &first, &end);
    int i = first + g_first + r;
    if (i >= end) return;
    std::wstring nv = g_items[i].value ? g_items[i].value() : L"";
    if (is_number(nv) && px >= g.val_x) {
        // number: the left half of the value lowers it, the right half raises it (also unselected)
        g_sel = i;
        invoke(px < (g.val_x + g.w) / 2 ? -1 : 1);
    } else if (i != g_sel) {
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
