// Texture replacement on Android (gs_texrep.h; the PC's gs_texrep.cpp uses WIC and Win32 folders).
// Only the images that ship with the port: lang/<code>_tex (translated labels) and lang/all_tex (the
// title logo), copied from the APK into the app folder (platform_android.cpp extract_lang). No HD
// texture packs and no dumping. Keys and the hash are the PC's, so the same files work on both.
#include "gs/gs_texrep.h"
#include "platform/platform.h"
#include <dirent.h>
#include <sys/stat.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

extern std::string g3_android_dir;  // win32_shim.cpp
bool g3_decode_png(const std::string& path, int& w, int& h, std::vector<u32>& px);  // jni_android.cpp

namespace gs {
namespace texrep {

static std::unordered_map<u64, std::string> g_files;  // key -> file (read-only after init)
static std::mutex g_mu;
static std::condition_variable g_cv;
static std::deque<u64> g_jobs;
static std::unordered_map<u64, Image> g_ready;  // loaded (w 0: unreadable)
static std::unordered_map<u64, bool> g_requested;

u64 hash(const void* data, size_t bytes, u64 seed) {
    // the same function as gs_texrep.cpp (the keys are file names)
    const u8* p = (const u8*)data;
    u64 h = seed ^ (bytes * 0x9E3779B97F4A7C15ull);
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        u64 w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < bytes; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

static bool parse_key(const char* name, u64& key) {
    u64 k = 0;
    for (int i = 0; i < 16; i++) {
        char c = name[i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        k = k << 4 | (u64)d;
    }
    key = k;
    return true;
}

static void scan(const std::string& dir, int depth) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string path = dir + "/" + e->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4) scan(path, depth + 1);
            continue;
        }
        size_t n = strlen(e->d_name);
        u64 key;
        if (n > 4 && strcasecmp(e->d_name + n - 4, ".png") == 0 && parse_key(e->d_name, key)) g_files.emplace(key, path);
    }
    closedir(d);
}

static void worker() {
    for (;;) {
        u64 key;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return !g_jobs.empty(); });
            key = g_jobs.front();
            g_jobs.pop_front();
        }
        Image img;
        if (!g3_decode_png(g_files[key], img.w, img.h, img.px)) {
            log_write(LOG_GS, "texrep: cannot read replacement %016llx", (unsigned long long)key);
            img = Image{};
        }
        for (u32& p : img.px) p = (p & 0xFFFFFF) | ((p >> 24) * 128 + 127) / 255 << 24;  // alpha 0..255 -> GS 0..128
        std::lock_guard<std::mutex> lk(g_mu);
        g_ready[key] = std::move(img);
    }
}

void init(bool) {
    // as on the PC: the translated labels first, then the port's own images (emplace keeps the first)
    const std::string lang = platform::config().language, dir = g3_android_dir + "/lang/";
    if (lang != "en") scan(dir + lang + "_tex", 0);
    scan(dir + "all_tex", 0);
    if (!g_files.empty()) std::thread(worker).detach();
    log_write(LOG_MAIN, "texture replacement: %zu images (language %s)", g_files.size(), lang.c_str());
}

bool have_pack() { return !g_files.empty(); }
bool translated(u64 key) { return g_files.count(key) != 0; }
bool dumping() { return false; }

State lookup(u64 key, const Image** img) {
    if (g_files.find(key) == g_files.end()) return NONE;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ready.find(key);
    if (it != g_ready.end()) {
        if (it->second.px.empty()) return NONE;
        *img = &it->second;
        return READY;
    }
    if (!g_requested[key]) {
        g_requested[key] = true;
        g_jobs.push_back(key);
        g_cv.notify_one();
    }
    return LOADING;
}

void release(u64 key) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ready.find(key);
    if (it != g_ready.end() && it->second.w != 0) {
        g_ready.erase(it);
        g_requested.erase(key);
    }
}

bool dumped(u64 key) { return g_files.count(key) != 0; }
void dump(u64, int, int, std::vector<u32>&&, const std::string&, std::vector<u8>&&) {}

}  // namespace texrep
}  // namespace gs
