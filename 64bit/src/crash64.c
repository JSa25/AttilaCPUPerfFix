// crash64: the plugin's own crash record, in the attila64 folder (the user, 2026-11-22: logs, crash records and settings
// of the 64-bit plugin apart from the 32-bit engine fix's). As heapfix/diag.c does on 32-bit: on the first exception
// that ends the game (the loader's own handler ends the process at every one that is not a C++ exception or a debugger
// notice), one record line in perffix64.log and a minidump crash_<date>_<time>.dmp beside it, the newest 3 kept. The dump
// is written by its own thread (a crashing thread may have no stack left), with System32's dbghelp (never one planted
// next to the game): threads, their stacks and the memory those point at, a few MB, no full heap.
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "perffix64.h"

typedef BOOL(WINAPI *MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
static MiniDumpWriteDump_t g_mdwd;
static HANDLE g_go, g_done;
static EXCEPTION_POINTERS *g_ep;
static DWORD g_tid;
static wchar_t g_cdir[MAX_PATH];
static wchar_t g_name[40];
static DWORD g_kb;
static int g_ok;
static DWORD g_err;

static DWORD WINAPI dump_thread(void *arg) {
    (void)arg;
    wchar_t p[MAX_PATH];
    UINT n = GetSystemDirectoryW(p, MAX_PATH - 16);
    if (n && n < MAX_PATH - 16) {
        wcscat(p, L"\\dbghelp.dll");
        HMODULE d = LoadLibraryW(p);
        if (d) g_mdwd = (MiniDumpWriteDump_t)(void *)GetProcAddress(d, "MiniDumpWriteDump");
    }
    WaitForSingleObject(g_go, INFINITE);
    WIN32_FIND_DATAW fd[16];   // keep the newest 2 older dumps: with this one at most 3
    int nf = 0;
    _snwprintf(p, MAX_PATH, L"%ls\\crash_*.dmp", g_cdir);
    HANDLE h = FindFirstFileW(p, &fd[0]);
    if (h != INVALID_HANDLE_VALUE) {
        do nf++; while (nf < 16 && FindNextFileW(h, &fd[nf]));
        FindClose(h);
    }
    for (int k = 0; k < nf - 2; k++) {   // names sort by time: the oldest goes
        int o = -1;
        for (int i = 0; i < nf; i++)
            if (fd[i].cFileName[0] && (o < 0 || wcscmp(fd[i].cFileName, fd[o].cFileName) < 0)) o = i;
        _snwprintf(p, MAX_PATH, L"%ls\\%ls", g_cdir, fd[o].cFileName);
        DeleteFileW(p);
        fd[o].cFileName[0] = 0;
    }
    if (g_mdwd) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        _snwprintf(g_name, 40, L"crash_%04d%02d%02d_%02d%02d%02d.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        _snwprintf(p, MAX_PATH, L"%ls\\%ls", g_cdir, g_name);
        HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
#pragma pack(push, 4)   // as dbghelp.h declares MINIDUMP_EXCEPTION_INFORMATION: on x64 the pointer sits at offset 4
        struct { DWORD tid; EXCEPTION_POINTERS *ep; BOOL client; } ei = {g_tid, g_ep, FALSE};
#pragma pack(pop)
        int type = 0x40 /* IndirectlyReferencedMemory */ | 0x1000 /* ThreadInfo */ | 0x20 /* UnloadedModules */;
        g_ok = f != INVALID_HANDLE_VALUE && g_mdwd(GetCurrentProcess(), GetCurrentProcessId(), f, type, &ei, NULL, NULL);
        if (!g_ok) g_err = GetLastError();
        g_kb = g_ok ? GetFileSize(f, NULL) >> 10 : 0;
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (!g_ok) DeleteFileW(p);
    }
    SetEvent(g_done);
    return 0;
}

// dir: the attila64 folder. The dump thread waits from the start (dbghelp is not loaded inside a crash).
void crash64_setup(const wchar_t *dir) {
    wcsncpy(g_cdir, dir, MAX_PATH - 1);
    g_go = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_go && g_done) CloseHandle(CreateThread(NULL, 256 * 1024, dump_thread, NULL, 0, NULL));
}

// The first exception that ends the game: the record line and the dump. game_lo / game_hi: the Linux game file's range
// (it is no Windows module, so its addresses are named by hand).
void crash64_record(EXCEPTION_POINTERS *e, uintptr_t game_lo, uintptr_t game_hi, void (*log)(const char *fmt, ...)) {
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return;
    uintptr_t at = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    char where[MAX_PATH + 40] = "an unknown place";
    HMODULE m = NULL;
    wchar_t mn[MAX_PATH];
    if (at >= game_lo && at < game_hi) snprintf(where, sizeof where, "Attila+%#llx", (unsigned long long)(at - game_lo));
    else if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)at, &m) &&
             GetModuleFileNameW(m, mn, MAX_PATH)) {
        const wchar_t *b = wcsrchr(mn, L'\\');
        snprintf(where, sizeof where, "%ls+%#llx", b ? b + 1 : mn, (unsigned long long)(at - (uintptr_t)m));
    }
    log("crash: the game ends on exception %08lx at %p (%s), thread %lu", e->ExceptionRecord->ExceptionCode, (void *)at, where, GetCurrentThreadId());
    if (!g_go || !g_done) { log("crash: no dump (its thread could not be started)"); return; }
    g_ep = e; g_tid = GetCurrentThreadId();
    SetEvent(g_go);
    if (WaitForSingleObject(g_done, 15000) != WAIT_OBJECT_0) { log("crash: the dump took longer than 15 s - it may be incomplete"); return; }
    if (g_ok) log("crash: dump %ls (%lu KB) in the attila64 folder - please attach it and perffix64.log to a crash report", g_name, g_kb);
    else log("crash: no dump written (%ls, error %#lx)", g_mdwd ? L"dbghelp failed" : L"no dbghelp.dll", g_err);
}
