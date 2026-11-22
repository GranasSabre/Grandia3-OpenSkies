// Vulkan loader (vk_dyn.h). System driver: dlopen("libvulkan.so"). Custom driver (e.g. Turnip for
// Adreno): libadrenotools opens the system loader in its own linker namespace with hooks that make it
// load the driver .so from files/gpu_drivers/<name>/ instead of the vendor one. The hook libraries
// (libhook_impl.so, libmain_hook.so, ...) ship in the APK next to libgrandia3.so; adrenotools needs
// them in the app's native library folder (extractNativeLibs = true).
#include "vk_dyn.h"
#include "common.h"
#include <adrenotools/driver.h>
#include <android/native_activity.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>

#define G3_VK_DEF(n) PFN_##n n = nullptr;
G3_VK_GLOBAL_FUNCS(G3_VK_DEF)
G3_VK_INSTANCE_FUNCS(G3_VK_DEF)
#undef G3_VK_DEF

extern ANativeActivity* g3_activity;           // platform_android.cpp
std::string g3_native_lib_dir();               // jni_android.cpp: getApplicationInfo().nativeLibraryDir

namespace vkdyn {
namespace {
void* g_lib = nullptr;
bool g_custom = false;
std::string g_label = "System";
PFN_vkGetInstanceProcAddr g_gipa = nullptr;

// the driver's .so inside its folder: meta.json "libraryName" (the usual driver package), else the first .so
std::string driver_library(const std::string& dir) {
    if (FILE* f = fopen((dir + "/meta.json").c_str(), "rb")) {
        std::string s;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
        fclose(f);
        size_t k = s.find("\"libraryName\"");
        if (k != std::string::npos) {
            size_t a = s.find('"', s.find(':', k) + 1), b = a == std::string::npos ? a : s.find('"', a + 1);
            if (b != std::string::npos) return s.substr(a + 1, b - a - 1);
        }
    }
    std::string lib;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 3 && n.compare(n.size() - 3, 3, ".so") == 0) { lib = n; break; }
        }
        closedir(d);
    }
    return lib;
}

bool load_globals() {
    g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_lib, "vkGetInstanceProcAddr");
    if (!g_gipa) return false;
#define G3_VK_LOAD(n) n = (PFN_##n)g_gipa(VK_NULL_HANDLE, #n);
    G3_VK_GLOBAL_FUNCS(G3_VK_LOAD)
#undef G3_VK_LOAD
    return vkCreateInstance != nullptr;
}
}  // namespace

bool load(const std::string& custom) {
    unload();
    if (!custom.empty() && g3_activity && g3_activity->internalDataPath) {
        std::string base = std::string(g3_activity->internalDataPath) + "/gpu_drivers", dir = base + "/" + custom + "/";
        std::string lib = driver_library(dir), hooks = g3_native_lib_dir(), tmp = std::string(g3_activity->internalDataPath) + "/gpu_tmp/";
        mkdir(tmp.c_str(), 0700);
        if (lib.empty() || hooks.empty()) {
            log_write(LOG_MAIN, "vulkan: custom driver '%s': %s", custom.c_str(), lib.empty() ? "no .so in its folder" : "no native library folder");
        } else {
            g_lib = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmp.c_str(), (hooks + "/").c_str(), dir.c_str(),
                                               lib.c_str(), nullptr, nullptr);
            if (g_lib && load_globals()) {
                g_custom = true;
                g_label = custom;
                log_write(LOG_MAIN, "vulkan: custom driver '%s' (%s) opened", custom.c_str(), lib.c_str());
                return true;
            }
            log_write(LOG_MAIN, "vulkan: custom driver '%s' (%s) could not be opened: %s", custom.c_str(), lib.c_str(), dlerror() ? dlerror() : "?");
            unload();
        }
    }
    g_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_lib || !load_globals()) {
        log_write(LOG_MAIN, "vulkan: no libvulkan.so");
        unload();
        return false;
    }
    return true;
}

void load_instance(VkInstance inst) {
    if (!g_gipa) return;
#define G3_VK_LOAD(n) n = (PFN_##n)g_gipa(inst, #n);
    G3_VK_INSTANCE_FUNCS(G3_VK_LOAD)
#undef G3_VK_LOAD
}

void unload() {
    // (the library stays mapped: Vulkan drivers do not like being unloaded; only the pointers go)
#define G3_VK_CLEAR(n) n = nullptr;
    G3_VK_GLOBAL_FUNCS(G3_VK_CLEAR)
    G3_VK_INSTANCE_FUNCS(G3_VK_CLEAR)
#undef G3_VK_CLEAR
    g_lib = nullptr;
    g_gipa = nullptr;
    g_custom = false;
    g_label = "System";
}

bool custom_active() { return g_custom; }
std::string driver_label() { return g_label; }
}  // namespace vkdyn
