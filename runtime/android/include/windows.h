// Android build: the subset of the Win32 API the shared runtime uses, implemented on POSIX
// (android/win32_shim.cpp). Only the Android build has this include directory, so the shared
// sources keep their #include <windows.h> unchanged.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cwchar>

typedef int BOOL;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef unsigned int UINT;
typedef int16_t SHORT;
typedef uint64_t DWORD64;
typedef uintptr_t ULONG_PTR;
typedef size_t SIZE_T;
typedef void* PVOID;
typedef void* LPVOID;
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* FARPROC;
typedef wchar_t WCHAR;
typedef const char* LPCSTR;
typedef const wchar_t* LPCWSTR;
typedef DWORD* LPDWORD;
#define VOID void
#define WINAPI
#define CALLBACK
#define TRUE 1
#define FALSE 0

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu

#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define MOVEFILE_REPLACE_EXISTING 1

#define FIBER_FLAG_FLOAT_SWITCH 1
#define CP_UTF8 65001
#define DUPLICATE_SAME_ACCESS 2

#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_SINGLE_STEP 0x80000004u
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_CONTINUE_SEARCH 0
#define CONTEXT_FULL 0x10000Bu

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    int64_t QuadPart;
} LARGE_INTEGER;

typedef struct _FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct _SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;

typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    char cFileName[MAX_PATH];
} WIN32_FIND_DATAA;

typedef struct _OVERLAPPED {
    ULONG_PTR Internal, InternalHigh;
    union {
        struct { DWORD Offset, OffsetHigh; };
        PVOID Pointer;
    };
    HANDLE hEvent;
} OVERLAPPED;

// exception handlers and thread contexts: only debug tools use them (never called on Android)
typedef struct _CONTEXT { DWORD ContextFlags; DWORD EFlags; DWORD64 Rip, Rsp; } CONTEXT;
typedef struct _EXCEPTION_RECORD { DWORD ExceptionCode; ULONG_PTR ExceptionInformation[15]; } EXCEPTION_RECORD;
typedef struct _EXCEPTION_POINTERS { EXCEPTION_RECORD* ExceptionRecord; CONTEXT* ContextRecord; } EXCEPTION_POINTERS;
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS*);

typedef DWORD (*LPTHREAD_START_ROUTINE)(void*);
typedef void (*LPFIBER_START_ROUTINE)(void*);

// time
BOOL QueryPerformanceCounter(LARGE_INTEGER* v);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* v);
void Sleep(DWORD ms);
BOOL SwitchToThread();
UINT timeBeginPeriod(UINT ms);
void GetSystemTimeAsFileTime(FILETIME* ft);
void GetLocalTime(SYSTEMTIME* st);
BOOL FileTimeToLocalFileTime(const FILETIME* ft, FILETIME* local);
BOOL FileTimeToSystemTime(const FILETIME* ft, SYSTEMTIME* st);

// memory
void* VirtualAlloc(void* addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(void* addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(void* addr, SIZE_T size, DWORD protect, DWORD* old);
void* AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h);

// threads, events, fibers
HANDLE CreateThread(void* attr, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void* param, DWORD flags, DWORD* id);
HANDLE GetCurrentThread();
HANDLE GetCurrentProcess();
DWORD GetCurrentThreadId();
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE* out, DWORD access, BOOL inherit, DWORD options);
long SetThreadDescription(HANDLE th, const wchar_t* name);
DWORD SuspendThread(HANDLE th);
DWORD ResumeThread(HANDLE th);
BOOL GetThreadContext(HANDLE th, CONTEXT* ctx);
HANDLE CreateEventW(void* attr, BOOL manual_reset, BOOL initial, const wchar_t* name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
BOOL CloseHandle(HANDLE h);
void* CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, void* param);
void* ConvertThreadToFiberEx(void* param, DWORD flags);
void SwitchToFiber(void* fiber);
void DeleteFiber(void* fiber);
void* GetCurrentFiber();

// modules
DWORD GetModuleFileNameA(HMODULE m, char* buf, DWORD size);
HMODULE GetModuleHandleW(const wchar_t* name);
HMODULE LoadLibraryW(const wchar_t* name);
FARPROC GetProcAddress(HMODULE m, const char* name);

// files (paths may use '\\': converted to '/')
HANDLE CreateFileA(const char* path, DWORD access, DWORD share, void* sec, DWORD disp, DWORD flags, HANDLE tmpl);
HANDLE CreateFileW(const wchar_t* path, DWORD access, DWORD share, void* sec, DWORD disp, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, void* buf, DWORD n, DWORD* got, OVERLAPPED* ov);
BOOL WriteFile(HANDLE h, const void* buf, DWORD n, DWORD* put, OVERLAPPED* ov);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* size);
DWORD GetFileAttributesA(const char* path);
typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
BOOL GetFileAttributesExA(const char* path, GET_FILEEX_INFO_LEVELS level, void* info);
DWORD GetFileAttributesW(const wchar_t* path);
BOOL CreateDirectoryA(const char* path, void* sec);
BOOL RemoveDirectoryA(const char* path);
BOOL DeleteFileA(const char* path);
BOOL MoveFileExA(const char* from, const char* to, DWORD flags);
HANDLE FindFirstFileA(const char* pattern, WIN32_FIND_DATAA* fd);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA* fd);
BOOL FindClose(HANDLE h);

// text: wchar_t is UTF-32 on Android
int MultiByteToWideChar(UINT cp, DWORD flags, const char* s, int n, wchar_t* out, int out_n);
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t* s, int n, char* out, int out_n, const char* def, BOOL* used);

static inline int localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : 1; }

typedef uint16_t USHORT;
void* _aligned_malloc(size_t size, size_t align);
void _aligned_free(void* p);
[[noreturn]] void ExitProcess(UINT code);
USHORT RtlCaptureStackBackTrace(DWORD skip, DWORD n, void** frames, DWORD* hash);

// PE image and unwind declarations of the sampling profiler (kernel.cpp); GetThreadContext fails on
// Android, so that code never runs
typedef struct { DWORD SizeOfImage; } IMAGE_OPTIONAL_HEADER64;
typedef struct { IMAGE_OPTIONAL_HEADER64 OptionalHeader; } IMAGE_NT_HEADERS64;
typedef struct { LONG e_lfanew; } IMAGE_DOS_HEADER;
typedef struct { DWORD BeginAddress; } RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;
#define UNW_FLAG_NHANDLER 0
static inline PRUNTIME_FUNCTION RtlLookupFunctionEntry(DWORD64, DWORD64*, void*) { return nullptr; }
static inline void* RtlVirtualUnwind(DWORD, DWORD64, DWORD64, PRUNTIME_FUNCTION, CONTEXT*, void**, DWORD64*, void*) { return nullptr; }
