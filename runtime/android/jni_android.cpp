// Calls into Java (JNI), from any native thread:
//  - text rendering for the subtitles (subtitles.cpp text_coverage; GDI on the PC): Android's own text
//    layout (TextPaint + StaticLayout into an ALPHA_8 bitmap), so umlauts, accents and line breaking
//    come from the system; called once per new line
//  - the activity's helpers (java/.../GrandiaActivity.java): system file picker for the disc image,
//    opening a picked document (content:// URI) as a file descriptor, messages as toasts
#include "common.h"
#include <android/native_activity.h>
#include <jni.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

ANativeActivity* g3_activity = nullptr;  // platform_android.cpp android_main

namespace {

bool draw(JNIEnv* env, const std::string& text, int fs, int max_w, int m, std::vector<u8>& t, int& W, int& H) {
    jclass cPaint = env->FindClass("android/text/TextPaint");
    jclass cType = env->FindClass("android/graphics/Typeface");
    jclass cBuilder = env->FindClass("android/text/StaticLayout$Builder");
    jclass cAlign = env->FindClass("android/text/Layout$Alignment");
    jclass cLayout = env->FindClass("android/text/Layout");
    jclass cBitmap = env->FindClass("android/graphics/Bitmap");
    jclass cConfig = env->FindClass("android/graphics/Bitmap$Config");
    jclass cCanvas = env->FindClass("android/graphics/Canvas");
    if (!cPaint || !cType || !cBuilder || !cAlign || !cLayout || !cBitmap || !cConfig || !cCanvas) return false;

    jobject paint = env->NewObject(cPaint, env->GetMethodID(cPaint, "<init>", "(I)V"), 1 /* ANTI_ALIAS_FLAG */);
    env->CallVoidMethod(paint, env->GetMethodID(cPaint, "setTextSize", "(F)V"), (jfloat)fs);
    env->CallVoidMethod(paint, env->GetMethodID(cPaint, "setColor", "(I)V"), (jint)-1);
    jobject face = env->CallStaticObjectMethod(cType, env->GetStaticMethodID(cType, "create", "(Ljava/lang/String;I)Landroid/graphics/Typeface;"),
                                               env->NewStringUTF("sans-serif-medium"), 0);
    if (face) env->CallObjectMethod(paint, env->GetMethodID(cPaint, "setTypeface", "(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;"), face);

    jstring str = env->NewStringUTF(text.c_str());
    if (!str) return false;
    jobject b = env->CallStaticObjectMethod(
        cBuilder, env->GetStaticMethodID(cBuilder, "obtain", "(Ljava/lang/CharSequence;IILandroid/text/TextPaint;I)Landroid/text/StaticLayout$Builder;"),
        str, 0, env->GetStringLength(str), paint, (jint)max_w);
    jobject center = env->GetStaticObjectField(cAlign, env->GetStaticFieldID(cAlign, "ALIGN_CENTER", "Landroid/text/Layout$Alignment;"));
    env->CallObjectMethod(b, env->GetMethodID(cBuilder, "setAlignment", "(Landroid/text/Layout$Alignment;)Landroid/text/StaticLayout$Builder;"), center);
    jobject layout = env->CallObjectMethod(b, env->GetMethodID(cBuilder, "build", "()Landroid/text/StaticLayout;"));
    if (!layout || env->ExceptionCheck()) return false;

    int lines = env->CallIntMethod(layout, env->GetMethodID(cLayout, "getLineCount", "()I"));
    jmethodID mLeft = env->GetMethodID(cLayout, "getLineLeft", "(I)F"), mRight = env->GetMethodID(cLayout, "getLineRight", "(I)F");
    float x0 = 1e9f, x1 = -1e9f;
    for (int i = 0; i < lines; i++) {
        x0 = std::min(x0, (float)env->CallFloatMethod(layout, mLeft, i));
        x1 = std::max(x1, (float)env->CallFloatMethod(layout, mRight, i));
    }
    int th = env->CallIntMethod(layout, env->GetMethodID(cLayout, "getHeight", "()I"));
    if (lines <= 0 || x1 <= x0 || th <= 0) return false;
    int left = (int)floorf(x0), tw = (int)ceilf(x1) - left;
    W = tw + 2 * m;
    H = th + 2 * m;

    jobject a8 = env->GetStaticObjectField(cConfig, env->GetStaticFieldID(cConfig, "ALPHA_8", "Landroid/graphics/Bitmap$Config;"));
    jobject bm = env->CallStaticObjectMethod(cBitmap, env->GetStaticMethodID(cBitmap, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"),
                                             (jint)W, (jint)H, a8);
    if (!bm || env->ExceptionCheck()) return false;
    jobject canvas = env->NewObject(cCanvas, env->GetMethodID(cCanvas, "<init>", "(Landroid/graphics/Bitmap;)V"), bm);
    env->CallVoidMethod(canvas, env->GetMethodID(cCanvas, "translate", "(FF)V"), (jfloat)(m - left), (jfloat)m);
    env->CallVoidMethod(layout, env->GetMethodID(cLayout, "draw", "(Landroid/graphics/Canvas;)V"), canvas);
    int row = env->CallIntMethod(bm, env->GetMethodID(cBitmap, "getRowBytes", "()I"));
    if (row < W || env->ExceptionCheck()) return false;
    std::vector<u8> buf((size_t)row * H);
    jobject bb = env->NewDirectByteBuffer(buf.data(), (jlong)buf.size());
    env->CallVoidMethod(bm, env->GetMethodID(cBitmap, "copyPixelsToBuffer", "(Ljava/nio/Buffer;)V"), bb);
    env->CallVoidMethod(bm, env->GetMethodID(cBitmap, "recycle", "()V"));
    if (env->ExceptionCheck()) return false;
    t.resize((size_t)W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) t[(size_t)y * W + x] = buf[(size_t)y * row + x];
    return true;
}

}  // namespace

namespace {

// the calling thread attached to the VM for the lifetime of the object
struct Jni {
    JNIEnv* env = nullptr;
    bool attached = false;
    Jni() {
        if (!g3_activity) return;
        JavaVM* vm = g3_activity->vm;
        if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) env = nullptr;
            else attached = true;
        }
        if (env && env->PushLocalFrame(32) != JNI_OK) env = nullptr;
    }
    ~Jni() {
        if (!env) return;
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        env->PopLocalFrame(nullptr);
        if (attached) g3_activity->vm->DetachCurrentThread();
    }
    bool failed() {
        if (!env->ExceptionCheck()) return false;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
    // a method of the activity (GrandiaActivity); nullptr in the plain NativeActivity
    jmethodID method(const char* name, const char* sig) {
        jclass c = env->GetObjectClass(g3_activity->clazz);
        jmethodID m = env->GetMethodID(c, name, sig);
        if (failed()) return nullptr;
        return m;
    }
};

std::mutex g_content_mu;
std::map<std::string, int> g_content_fd;

}  // namespace

std::string g3_content_path(const std::string& uri) {
    std::lock_guard<std::mutex> lk(g_content_mu);
    auto it = g_content_fd.find(uri);
    if (it == g_content_fd.end()) {
        Jni j;
        int fd = -1;
        if (j.env)
            if (jmethodID m = j.method("openUri", "(Ljava/lang/String;)I")) {
                fd = j.env->CallIntMethod(g3_activity->clazz, m, j.env->NewStringUTF(uri.c_str()));
                if (j.failed()) fd = -1;
            }
        if (fd < 0) {
            log_write(LOG_MAIN, "document %s: no access (moved, deleted, or the permission was taken back)", uri.c_str());
            return "/nonexistent/document";
        }
        it = g_content_fd.emplace(uri, fd).first;
    }
    return "/proc/self/fd/" + std::to_string(it->second);
}

// system file picker (blocks until the player chose a document or cancelled); "" when cancelled
std::string g3_pick_document() {
    {
        Jni j;
        jmethodID m = j.env ? j.method("pickDocument", "()V") : nullptr;
        if (!m) return "";
        j.env->CallVoidMethod(g3_activity->clazz, m);
        if (j.failed()) return "";
    }
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Jni j;
        if (!j.env) return "";
        jmethodID done = j.method("pickDone", "()Z");
        if (!done) return "";
        if (!j.env->CallBooleanMethod(g3_activity->clazz, done)) continue;
        jstring r = (jstring)j.env->CallObjectMethod(g3_activity->clazz, j.method("pickResult", "()Ljava/lang/String;"));
        if (j.failed() || !r) return "";
        const char* c = j.env->GetStringUTFChars(r, nullptr);
        std::string s = c ? c : "";
        if (c) j.env->ReleaseStringUTFChars(r, c);
        return s;
    }
}

// a message for the player (a long toast); false without the activity's helper
bool g3_show_message(const std::string& utf8) {
    Jni j;
    jmethodID m = j.env ? j.method("showMessage", "(Ljava/lang/String;)V") : nullptr;
    if (!m) return false;
    j.env->CallVoidMethod(g3_activity->clazz, m, j.env->NewStringUTF(utf8.c_str()));
    return !j.failed();
}

namespace platform {

bool text_coverage(const std::string& text, int fs, int max_w, int m, std::vector<u8>& t, int& W, int& H) {
    if (!g3_activity || text.empty()) return false;
    JavaVM* vm = g3_activity->vm;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    }
    bool ok = false;
    if (env->PushLocalFrame(64) == JNI_OK) {
        ok = draw(env, text, fs, max_w, m, t, W, H);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            ok = false;
        }
        env->PopLocalFrame(nullptr);
    }
    if (attached) vm->DetachCurrentThread();
    if (!ok) log_write(LOG_MAIN, "subtitles: text rendering failed");
    return ok;
}

}  // namespace platform
