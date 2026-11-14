// Android build: POSIX implementation of the Win32 subset in include/windows.h.
#include <windows.h>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#undef fopen  // g3_compat.h: the real one is used here

std::string g3_android_dir;  // the app's files folder, set by android_main (plays the role of the exe folder)

// a document chosen in the system file picker (content:// URI, e.g. the disc image): opened through
// the activity (jni_android.cpp), read as /proc/self/fd/N
std::string g3_content_path(const std::string& uri);

static std::string fix_path(const char* p) {
    std::string s(p ? p : "");
    if (s.rfind("content://", 0) == 0) return g3_content_path(s);
    for (char& c : s)
        if (c == '\\') c = '/';
    return s;
}

// A picked document is used through the descriptor the activity got for it: opening
// /proc/self/fd/N again would check the real path (e.g. in Download), which the app may not read.
static int doc_fd(const std::string& p) {
    static const char P[] = "/proc/self/fd/";
    if (p.rfind(P, 0) != 0) return -1;
    return atoi(p.c_str() + sizeof(P) - 1);
}

FILE* g3_fopen(const char* path, const char* mode) {
    std::string p = fix_path(path);
    int d = doc_fd(p);
    if (d >= 0) {
        int fd = dup(d);
        if (fd < 0) return nullptr;
        lseek(fd, 0, SEEK_SET);  // (dup shares the position: streams on the document are read one at a time)
        FILE* f = fdopen(fd, mode);
        if (!f) close(fd);
        return f;
    }
    return fopen(p.c_str(), mode);
}

static std::string narrow(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) {
        u_int32_t c = (u_int32_t)*w;
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
        else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    }
    return s;
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------
enum HType { H_FILE = 1, H_EVENT, H_THREAD, H_FIND, H_PSEUDO };
struct HObj { HType type; };
struct HFile : HObj { int fd; };
struct HEvent : HObj {
    std::mutex m;
    std::condition_variable cv;
    bool signaled = false, manual = false;
    int waiters = 0;  // threads in WaitForSingleObject (SetEvent wakes only then: no syscall otherwise)
};
struct HThread : HObj { pthread_t th; };
struct HFind : HObj {
    DIR* d;
    std::string dir, pat;
};
static HObj g_pseudo_thread{H_PSEUDO}, g_pseudo_process{H_PSEUDO};

BOOL CloseHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    HObj* o = (HObj*)h;
    switch (o->type) {
    case H_FILE: close(((HFile*)o)->fd); delete (HFile*)o; break;
    case H_EVENT: delete (HEvent*)o; break;
    case H_THREAD: delete (HThread*)o; break;
    default: break;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
BOOL QueryPerformanceCounter(LARGE_INTEGER* v) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    v->QuadPart = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    return TRUE;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER* v) { v->QuadPart = 1000000000ll; return TRUE; }
void Sleep(DWORD ms) {
    if (ms == 0) { sched_yield(); return; }
    timespec ts{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000l};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}
BOOL SwitchToThread() { sched_yield(); return TRUE; }
UINT timeBeginPeriod(UINT) { return 0; }

static const int64_t FT_EPOCH = 11644473600ll;  // seconds from 1601 to 1970
static void to_ft(int64_t sec, long nsec, FILETIME* ft) {
    uint64_t v = (uint64_t)(sec + FT_EPOCH) * 10000000ull + (uint64_t)(nsec / 100);
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}
static int64_t ft_100ns(const FILETIME* ft) { return (int64_t)(((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime); }
void GetSystemTimeAsFileTime(FILETIME* ft) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    to_ft(ts.tv_sec, ts.tv_nsec, ft);
}
static void tm_to_st(const tm& t, int ms, SYSTEMTIME* st) {
    st->wYear = (WORD)(t.tm_year + 1900);
    st->wMonth = (WORD)(t.tm_mon + 1);
    st->wDayOfWeek = (WORD)t.tm_wday;
    st->wDay = (WORD)t.tm_mday;
    st->wHour = (WORD)t.tm_hour;
    st->wMinute = (WORD)t.tm_min;
    st->wSecond = (WORD)t.tm_sec;
    st->wMilliseconds = (WORD)ms;
}
void GetLocalTime(SYSTEMTIME* st) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tm t;
    localtime_r(&ts.tv_sec, &t);
    tm_to_st(t, (int)(ts.tv_nsec / 1000000), st);
}
BOOL FileTimeToLocalFileTime(const FILETIME* ft, FILETIME* local) {
    time_t sec = (time_t)(ft_100ns(ft) / 10000000ll - FT_EPOCH);
    tm t;
    localtime_r(&sec, &t);
    to_ft(sec + t.tm_gmtoff, (long)(ft_100ns(ft) % 10000000ll) * 100, local);
    return TRUE;
}
BOOL FileTimeToSystemTime(const FILETIME* ft, SYSTEMTIME* st) {
    int64_t v = ft_100ns(ft);
    time_t sec = (time_t)(v / 10000000ll - FT_EPOCH);
    tm t;
    gmtime_r(&sec, &t);
    tm_to_st(t, (int)((v % 10000000ll) / 10000), st);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
static std::mutex g_vm_mutex;
static std::map<void*, size_t> g_vm_sizes;

static int prot_of(DWORD p) { return p == PAGE_NOACCESS ? PROT_NONE : p == PAGE_READONLY ? PROT_READ : PROT_READ | PROT_WRITE; }

void* VirtualAlloc(void* addr, SIZE_T size, DWORD type, DWORD protect) {
    if (addr) return nullptr;  // only fresh allocations are used
    void* p = mmap(nullptr, size, (type & MEM_COMMIT) ? prot_of(protect) : PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    std::lock_guard<std::mutex> lk(g_vm_mutex);
    g_vm_sizes[p] = size;
    return p;
}
BOOL VirtualFree(void* addr, SIZE_T, DWORD) {
    std::lock_guard<std::mutex> lk(g_vm_mutex);
    auto it = g_vm_sizes.find(addr);
    if (it == g_vm_sizes.end()) return FALSE;
    munmap(addr, it->second);
    g_vm_sizes.erase(it);
    return TRUE;
}
BOOL VirtualProtect(void* addr, SIZE_T size, DWORD protect, DWORD* old) {
    if (old) *old = PAGE_READWRITE;
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)4095;
    return mprotect((void*)a, size + ((uintptr_t)addr - a), prot_of(protect)) == 0;
}
void* AddVectoredExceptionHandler(ULONG, PVECTORED_EXCEPTION_HANDLER) { return nullptr; }  // debug tools only
void* _aligned_malloc(size_t size, size_t align) {
    void* p = nullptr;
    return posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, size) == 0 ? p : nullptr;
}
void _aligned_free(void* p) { free(p); }
void ExitProcess(UINT code) { _exit((int)code); }
USHORT RtlCaptureStackBackTrace(DWORD, DWORD, void**, DWORD*) { return 0; }  // debug logs only

// ---------------------------------------------------------------------------
// Threads and events
// ---------------------------------------------------------------------------
struct ThreadStart {
    LPTHREAD_START_ROUTINE fn;
    void* param;
};
static void* thread_entry(void* p) {
    ThreadStart s = *(ThreadStart*)p;
    delete (ThreadStart*)p;
    s.fn(s.param);
    return nullptr;
}
HANDLE CreateThread(void*, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void* param, DWORD, DWORD* id) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack ? (stack < 256 * 1024 ? 256 * 1024 : stack) : 8 * 1024 * 1024);
    HThread* t = new HThread;
    t->type = H_THREAD;
    if (pthread_create(&t->th, &a, thread_entry, new ThreadStart{fn, param}) != 0) {
        pthread_attr_destroy(&a);
        delete t;
        return nullptr;
    }
    pthread_attr_destroy(&a);
    if (id) *id = (DWORD)pthread_gettid_np(t->th);  // the same number GetCurrentThreadId gives inside
    return t;
}
HANDLE GetCurrentThread() { return &g_pseudo_thread; }
HANDLE GetCurrentProcess() { return &g_pseudo_process; }
DWORD GetCurrentThreadId() { return (DWORD)gettid(); }
BOOL DuplicateHandle(HANDLE, HANDLE src, HANDLE, HANDLE* out, DWORD, BOOL, DWORD) {
    if (src != &g_pseudo_thread) return FALSE;
    HThread* t = new HThread;
    t->type = H_THREAD;
    t->th = pthread_self();
    *out = t;
    return TRUE;
}
long SetThreadDescription(HANDLE th, const wchar_t* name) {
    if (!th || ((HObj*)th)->type != H_THREAD) return -1;
    std::string n = narrow(name).substr(0, 15);
    pthread_setname_np(((HThread*)th)->th, n.c_str());
    return 0;
}
// the sampling profiler (--perf) suspends threads: not available here
DWORD SuspendThread(HANDLE) { return (DWORD)-1; }
DWORD ResumeThread(HANDLE) { return (DWORD)-1; }
BOOL GetThreadContext(HANDLE, CONTEXT*) { return FALSE; }

HANDLE CreateEventW(void*, BOOL manual_reset, BOOL initial, const wchar_t*) {
    HEvent* e = new HEvent;
    e->type = H_EVENT;
    e->manual = manual_reset != 0;
    e->signaled = initial != 0;
    return e;
}
BOOL SetEvent(HANDLE h) {
    HEvent* e = (HEvent*)h;
    {
        std::lock_guard<std::mutex> lk(e->m);
        e->signaled = true;
        if (!e->waiters) return TRUE;
    }
    if (e->manual) e->cv.notify_all();
    else e->cv.notify_one();
    return TRUE;
}
BOOL ResetEvent(HANDLE h) {
    HEvent* e = (HEvent*)h;
    std::lock_guard<std::mutex> lk(e->m);
    e->signaled = false;
    return TRUE;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    HObj* o = (HObj*)h;
    if (o->type == H_THREAD) {
        if (ms != INFINITE) return WAIT_TIMEOUT;
        pthread_join(((HThread*)o)->th, nullptr);
        return WAIT_OBJECT_0;
    }
    if (o->type != H_EVENT) return WAIT_FAILED;
    HEvent* e = (HEvent*)o;
    std::unique_lock<std::mutex> lk(e->m);
    e->waiters++;
    bool ok = true;
    if (ms == INFINITE) e->cv.wait(lk, [e] { return e->signaled; });
    else ok = e->cv.wait_for(lk, std::chrono::milliseconds(ms), [e] { return e->signaled; });
    e->waiters--;
    if (!ok) return WAIT_TIMEOUT;
    if (!e->manual) e->signaled = false;
    return WAIT_OBJECT_0;
}

// ---------------------------------------------------------------------------
// Fibers (fiber_arm64.S switches the registers)
// ---------------------------------------------------------------------------
extern "C" void g3_ctx_switch(void** save_sp, void* load_sp);
extern "C" void g3_fiber_trampoline();

struct Fiber {
    void* sp = nullptr;
    LPFIBER_START_ROUTINE fn = nullptr;
    void* param = nullptr;
    void* stack = nullptr;
    size_t stack_size = 0;
};
static thread_local Fiber* t_current = nullptr;

extern "C" void g3_fiber_main(Fiber* f) {
    f->fn(f->param);
    // a Win32 fiber function that returns ends the thread; the kernel's never do
    abort();
}

void* CreateFiberEx(SIZE_T, SIZE_T reserve, DWORD, LPFIBER_START_ROUTINE fn, void* param) {
    Fiber* f = new Fiber;
    f->fn = fn;
    f->param = param;
    f->stack_size = (reserve < 256 * 1024 ? 256 * 1024 : reserve) + 4096;
    void* s = mmap(nullptr, f->stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (s == MAP_FAILED) { delete f; return nullptr; }
    mprotect(s, 4096, PROT_NONE);  // guard page against overflow
    f->stack = s;
    // initial frame as g3_ctx_switch leaves it: x19 = fiber, x30 = trampoline, x29 = 0
    uintptr_t top = ((uintptr_t)s + f->stack_size) & ~(uintptr_t)15;
    u_int64_t* frame = (u_int64_t*)(top - 160);
    memset(frame, 0, 160);
    frame[0] = (u_int64_t)(uintptr_t)f;
    frame[11] = (u_int64_t)(uintptr_t)&g3_fiber_trampoline;
    f->sp = frame;
    return f;
}
void* ConvertThreadToFiberEx(void* param, DWORD) {
    Fiber* f = new Fiber;
    f->param = param;
    t_current = f;
    return f;
}
void SwitchToFiber(void* fiber) {
    Fiber* to = (Fiber*)fiber;
    Fiber* from = t_current;
    if (to == from) return;
    t_current = to;
    g3_ctx_switch(&from->sp, to->sp);
}
void DeleteFiber(void* fiber) {
    Fiber* f = (Fiber*)fiber;
    if (!f || f == t_current) return;
    if (f->stack) munmap(f->stack, f->stack_size);
    delete f;
}
void* GetCurrentFiber() { return t_current; }

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------
DWORD GetModuleFileNameA(HMODULE, char* buf, DWORD size) {
    std::string p = g3_android_dir + "/Grandia3.exe";
    snprintf(buf, size, "%s", p.c_str());
    return (DWORD)strlen(buf);
}
HMODULE GetModuleHandleW(const wchar_t*) { return nullptr; }
HMODULE LoadLibraryW(const wchar_t*) { return nullptr; }
FARPROC GetProcAddress(HMODULE, const char*) { return nullptr; }

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
HANDLE CreateFileA(const char* path, DWORD access, DWORD, void*, DWORD disp, DWORD, HANDLE) {
    int fl = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (disp == CREATE_ALWAYS) fl |= O_CREAT | O_TRUNC;
    else if (disp == OPEN_ALWAYS) fl |= O_CREAT;
    std::string p = fix_path(path);
    int d = doc_fd(p);
    int fd = d >= 0 ? dup(d) : open(p.c_str(), fl | O_CLOEXEC, 0666);  // (the image is read with pread)
    if (fd < 0) return INVALID_HANDLE_VALUE;
    HFile* f = new HFile;
    f->type = H_FILE;
    f->fd = fd;
    return f;
}
HANDLE CreateFileW(const wchar_t* path, DWORD access, DWORD share, void* sec, DWORD disp, DWORD flags, HANDLE t) {
    return CreateFileA(narrow(path).c_str(), access, share, sec, disp, flags, t);
}
BOOL ReadFile(HANDLE h, void* buf, DWORD n, DWORD* got, OVERLAPPED* ov) {
    int fd = ((HFile*)h)->fd;
    ssize_t r;
    if (ov) r = pread(fd, buf, n, (off_t)(((u_int64_t)ov->OffsetHigh << 32) | ov->Offset));
    else r = read(fd, buf, n);
    if (got) *got = r > 0 ? (DWORD)r : 0;
    return r >= 0;
}
BOOL WriteFile(HANDLE h, const void* buf, DWORD n, DWORD* put, OVERLAPPED* ov) {
    int fd = ((HFile*)h)->fd;
    ssize_t r;
    if (ov) r = pwrite(fd, buf, n, (off_t)(((u_int64_t)ov->OffsetHigh << 32) | ov->Offset));
    else r = write(fd, buf, n);
    if (put) *put = r > 0 ? (DWORD)r : 0;
    return r >= 0;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size) {
    struct stat st;
    if (fstat(((HFile*)h)->fd, &st) != 0) return FALSE;
    size->QuadPart = st.st_size;
    return TRUE;
}
DWORD GetFileAttributesA(const char* path) {
    struct stat st;
    std::string p = fix_path(path);
    int d = doc_fd(p);
    if ((d >= 0 ? fstat(d, &st) : stat(p.c_str(), &st)) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
DWORD GetFileAttributesW(const wchar_t* path) { return GetFileAttributesA(narrow(path).c_str()); }
BOOL GetFileAttributesExA(const char* path, GET_FILEEX_INFO_LEVELS, void* info) {
    struct stat st;
    std::string p = fix_path(path);
    int d = doc_fd(p);
    if ((d >= 0 ? fstat(d, &st) : stat(p.c_str(), &st)) != 0) return FALSE;
    WIN32_FILE_ATTRIBUTE_DATA* fa = (WIN32_FILE_ATTRIBUTE_DATA*)info;
    memset(fa, 0, sizeof *fa);
    fa->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    fa->nFileSizeLow = (DWORD)st.st_size;
    fa->nFileSizeHigh = (DWORD)((u_int64_t)st.st_size >> 32);
    return TRUE;
}
BOOL CreateDirectoryA(const char* path, void*) { return mkdir(fix_path(path).c_str(), 0777) == 0; }
BOOL RemoveDirectoryA(const char* path) { return rmdir(fix_path(path).c_str()) == 0; }
BOOL DeleteFileA(const char* path) { return unlink(fix_path(path).c_str()) == 0; }
BOOL MoveFileExA(const char* from, const char* to, DWORD) { return rename(fix_path(from).c_str(), fix_path(to).c_str()) == 0; }

static bool wild(const char* p, const char* s) {  // Windows matching: case-insensitive
    if (!*p) return !*s;
    if (*p == '*') return wild(p + 1, s) || (*s && wild(p, s + 1));
    if (!*s) return false;
    if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) return wild(p + 1, s + 1);
    return false;
}
static bool find_fill(HFind* f, WIN32_FIND_DATAA* fd) {
    while (dirent* e = readdir(f->d)) {
        if (!wild(f->pat.c_str(), e->d_name)) continue;
        memset(fd, 0, sizeof(*fd));
        snprintf(fd->cFileName, MAX_PATH, "%s", e->d_name);
        struct stat st;
        if (stat((f->dir + "/" + e->d_name).c_str(), &st) == 0) {
            fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            fd->nFileSizeLow = (DWORD)st.st_size;
            fd->nFileSizeHigh = (DWORD)((u_int64_t)st.st_size >> 32);
            to_ft(st.st_mtim.tv_sec, st.st_mtim.tv_nsec, &fd->ftLastWriteTime);
            to_ft(st.st_ctim.tv_sec, st.st_ctim.tv_nsec, &fd->ftCreationTime);
            fd->ftLastAccessTime = fd->ftLastWriteTime;
        }
        return true;
    }
    return false;
}
HANDLE FindFirstFileA(const char* pattern, WIN32_FIND_DATAA* fd) {
    std::string p = fix_path(pattern);
    size_t slash = p.find_last_of('/');
    HFind* f = new HFind;
    f->type = H_FIND;
    f->dir = slash == std::string::npos ? "." : p.substr(0, slash);
    f->pat = slash == std::string::npos ? p : p.substr(slash + 1);
    f->d = opendir(f->dir.c_str());
    if (!f->d || !find_fill(f, fd)) {
        if (f->d) closedir(f->d);
        delete f;
        return INVALID_HANDLE_VALUE;
    }
    return f;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA* fd) { return find_fill((HFind*)h, fd); }
BOOL FindClose(HANDLE h) {
    HFind* f = (HFind*)h;
    if (f->d) closedir(f->d);
    delete f;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
int MultiByteToWideChar(UINT, DWORD, const char* s, int n, wchar_t* out, int out_n) {
    size_t len = n < 0 ? strlen(s) + 1 : (size_t)n;
    std::wstring w;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        u_int32_t cp;
        int k;
        if (c < 0x80) { cp = c; k = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; k = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; k = 3; }
        else { cp = c & 0x07; k = 4; }
        for (int j = 1; j < k && i + j < len; j++) cp = (cp << 6) | ((unsigned char)s[i + j] & 0x3F);
        w += (wchar_t)cp;
        i += (size_t)k;
    }
    if (out_n == 0) return (int)w.size();
    int m = (int)w.size() < out_n ? (int)w.size() : out_n;
    memcpy(out, w.data(), (size_t)m * sizeof(wchar_t));
    return m;
}
int WideCharToMultiByte(UINT, DWORD, const wchar_t* s, int n, char* out, int out_n, const char*, BOOL*) {
    std::wstring w = n < 0 ? std::wstring(s) : std::wstring(s, (size_t)n);
    std::string u = narrow(w.c_str());
    if (n < 0) u += '\0';
    if (out_n == 0) return (int)u.size();
    int m = (int)u.size() < out_n ? (int)u.size() : out_n;
    memcpy(out, u.data(), (size_t)m);
    return m;
}
