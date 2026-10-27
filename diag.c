// Diagnostics for players' reports ("it doesn't work", crashes): what the log needs to explain a problem without us
// having the player's PC.
//   diag_startup: mod version, which process loaded us, game build, Windows/CPU/RAM/address space, the relevant
//     preferences, and other DLLs that hook the game (ReShade, other proxies) found in the game folder.
//   crash records: a vectored exception handler logs fatal-type exceptions (access violation, heap corruption, ...)
//     with module+offset, registers and a stack scan. It runs first-chance and returns EXCEPTION_CONTINUE_SEARCH,
//     so the game and its crash reporter still see every exception; a record may be one the game later handles,
//     and the log says so. The handler allocates nothing (the heap may be what broke) and writes with WriteFile.
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "version.h"
#define MOD_VERSION "v" MOD_VERSION_STR
#define MAX_RECORDS 8  // per session: the first ones are what matters, and a loop of faults must not flood the log

static wchar_t g_dlog[MAX_PATH];
static DWORD g_main_tid, g_self_base;
volatile DWORD g_uikeep_tid;  // uikeep.c: the thread that is keeping or deleting a menu picture right now, else 0
static volatile LONG g_records, g_busy;
static char g_buf[8192];  // the handler's only buffer: static, so a stack overflow record needs no stack

static void dwrite(const char *s, int n) {
    HANDLE f = CreateFileW(g_dlog, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, s, (DWORD)n, &w, NULL);
    CloseHandle(f);
}

// The image an address belongs to and its name, without the loader lock (a crashing thread may hold it):
// VirtualQuery gives the image base, the PE export directory the name. Returns NULL if not in an image.
static const char *image_of(DWORD a, DWORD *base) {
    MEMORY_BASIC_INFORMATION mb;
    if (a < 0x10000 || !VirtualQuery((void *)(UINT_PTR)a, &mb, sizeof mb) || mb.Type != MEM_IMAGE ||
        mb.State != MEM_COMMIT || !(mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                                  PAGE_EXECUTE_WRITECOPY)))
        return NULL;
    BYTE *b = mb.AllocationBase;
    *base = (DWORD)(UINT_PTR)b;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (d->VirtualAddress && d->Size) return (const char *)(b + ((IMAGE_EXPORT_DIRECTORY *)(b + d->VirtualAddress))->Name);
    return b == (BYTE *)GetModuleHandleW(NULL) ? "exe" : "image";
}

// "name.dll+0x1234" for an address in a loaded module, else the bare address.
static int where(char *out, int cap, DWORD a) {
    DWORD base;
    const char *name = image_of(a, &base);
    return name ? snprintf(out, cap, "%s+0x%lX", name, a - base) : snprintf(out, cap, "%08lX", a);
}

static const char *code_name(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error (file or disk read failed)";
    case 0xC0000374: return "heap corruption";
    case 0xC0000017: return "out of memory";
    default: return NULL;
    }
}

// case-insensitive ASCII compare (the handler avoids CRT functions that may touch locale state)
static int same_ascii(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20)) return 0;
    return *a == *b;
}

// for the log: the game version in plain words, by empire.retail.dll's TimeDateStamp (NULL = unknown build)
static const char *build_name(DWORD stamp) {
    return stamp == 0x69CE1786u ? "normal Steam version (1.6.0) - supported"
         : stamp == 0x698089B0u ? "GOG version - partly supported (some fixes do not fit it and stay off, see STATUS)"
         : NULL;
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    const char *what = code_name(code);
    if (!what || g_records >= MAX_RECORDS || InterlockedExchange(&g_busy, 1)) return EXCEPTION_CONTINUE_SEARCH;
    LONG nrec = InterlockedIncrement(&g_records);
    CONTEXT *c = e->ContextRecord;
    char *p = g_buf, *end = g_buf + sizeof g_buf - 64;
    SYSTEMTIME t;
    GetLocalTime(&t);
    DWORD tid = GetCurrentThreadId();
    p += snprintf(p, end - p, "[%02d:%02d:%02d.%03d] EXCEPTION %d: 0x%08lX %s at ", t.wHour, t.wMinute, t.wSecond,
                  t.wMilliseconds, (int)nrec, code, what);
    DWORD at = (DWORD)(UINT_PTR)e->ExceptionRecord->ExceptionAddress, at_base = 0;
    const char *at_name = image_of(at, &at_base);
    p += where(p, end - p, at);
    p += snprintf(p, end - p, ", %s thread %lu (first chance: the game may still handle it)\n",
                  tid == g_main_tid ? "main" : "worker", tid);
    // the same in plain words: whose code it happened in
    int ours = at_base && at_base == g_self_base, game = at_name && same_ascii(at_name, "empire.retail.dll");
    p += snprintf(p, end - p, "  -> %s%s. If the game closed or froze right now, this is why; if it kept running, it "
                  "recovered.\n", ours ? "inside this mod (please report it)" : game ? "in the game's own code, not in this mod"
                  : "not in this mod, in ", ours || game ? "" : at_name ? at_name : "code outside any DLL");
    if (g_uikeep_tid && g_uikeep_tid == tid)
        p += snprintf(p, end - p, "  -> it happened while the menu texture fix was keeping or deleting a menu picture. "
                      "To switch that fix off, create an empty uikeep_off.txt in the game folder, and please report it.\n");
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        e->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR k = e->ExceptionRecord->ExceptionInformation[0];
        p += snprintf(p, end - p, "  %s address %08lX\n", k == 0 ? "read of" : k == 1 ? "write to" : "execute at",
                      (DWORD)e->ExceptionRecord->ExceptionInformation[1]);
    }
    p += snprintf(p, end - p, "  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX\n",
                  c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    if (code != EXCEPTION_STACK_OVERFLOW) {
        // Stack scan: values on the stack that point into a loaded module, the likely return addresses.
        NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
        DWORD *sp = (DWORD *)(UINT_PTR)c->Esp, *top = (DWORD *)tib->StackBase;
        int found = 0;
        p += snprintf(p, end - p, "  stack:");
        for (; sp < top && sp < (DWORD *)(UINT_PTR)c->Esp + 4096 && found < 24 && end - p > 80; sp++) {
            DWORD base;
            if (image_of(*sp, &base)) {
                *p++ = ' ';
                p += where(p, end - p, *sp);
                found++;
            }
        }
        *p++ = '\n';
    }
    dwrite(g_buf, (int)(p - g_buf));
    InterlockedExchange(&g_busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

// One preference line ("name value;") from Attila's preferences.script.txt, or "?" when absent.
static void pref(const char *text, const char *name, char *out, int cap) {
    snprintf(out, cap, "?");
    for (const char *s = text; (s = strstr(s, name)) != NULL; s++) {
        if ((s != text && s[-1] != '\n') || s[strlen(name)] != ' ') continue;
        const char *v = s + strlen(name) + 1;
        int n = 0;
        while (v[n] && v[n] != ';' && v[n] != '\r' && v[n] != '\n' && n < cap - 1) n++;
        snprintf(out, cap, "%.*s", n, v);
        return;
    }
}

void diag_startup(HMODULE empire, const wchar_t *log_path, void (*log)(const char *fmt, ...)) {
    g_main_tid = GetCurrentThreadId();
    MEMORY_BASIC_INFORMATION mb;
    if (VirtualQuery((void *)diag_startup, &mb, sizeof mb)) g_self_base = (DWORD)(UINT_PTR)mb.AllocationBase;
    wcscpy(g_dlog, log_path);
    wchar_t exe[MAX_PATH], dir[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wcscpy(dir, log_path);
    wchar_t *sl = wcsrchr(dir, L'\\');
    if (sl) *sl = 0;
    log("Attila CPU Performance Fix %s loaded into %ls", MOD_VERSION, exe);
    if (!empire) {
        log("empire.retail.dll is not loaded and Windows could not be asked to report when it loads - nothing "
            "patched in this process");
        return;
    }
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)empire + ((IMAGE_DOS_HEADER *)empire)->e_lfanew);
    HMODULE exem = GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *xnt = (IMAGE_NT_HEADERS *)((BYTE *)exem + ((IMAGE_DOS_HEADER *)exem)->e_lfanew);
    log("game: empire.retail.dll build %08lX, image %lu bytes, loaded at %p; Attila.exe large-address-aware: %s",
        nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, (void *)empire,
        (xnt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) ? "yes" : "NO");
    const char *bn = build_name(nt->FileHeader.TimeDateStamp);
    if (bn)
        log("Game version: %s", bn);
    else
        log("Game version: unknown (build %08lX) - NOT supported yet: each fix checks the game's code and switches "
            "itself off where it does not fit (see STATUS); the game then runs as without it.", nt->FileHeader.TimeDateStamp);

    OSVERSIONINFOW ov = {.dwOSVersionInfoSize = sizeof ov};
    LONG(WINAPI * rtl)(OSVERSIONINFOW *) = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rtl) rtl(&ov);
    int r[4];
    char cpu[49] = {0};
    for (int i = 0; i < 3; i++) {
        __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000002 + i), "c"(0));
        memcpy(cpu + 16 * i, r, 16);
    }
    for (int i = (int)strlen(cpu) - 1; i >= 0 && cpu[i] == ' '; i--) cpu[i] = 0;
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
    GlobalMemoryStatusEx(&ms);
    log("system: Windows %lu.%lu build %lu; %s, %lu logical CPUs; RAM %llu MB; address space %llu MB",
        ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber, cpu + strspn(cpu, " "), si.dwNumberOfProcessors,
        ms.ullTotalPhys >> 20, ms.ullTotalVirtual >> 20);

    wchar_t pp[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", pp, MAX_PATH);
    if (n && n < MAX_PATH - 64) {
        wcscat(pp, L"\\The Creative Assembly\\Attila\\scripts\\preferences.script.txt");
        static char text[65536];
        FILE *f = _wfopen(pp, L"rb");
        size_t len = f ? fread(text, 1, sizeof text - 1, f) : 0;
        if (f) fclose(f);
        text[len] = 0;
        char th[16], vm[16], xr[16], yr[16], fs[16];
        pref(text, "number_of_threads", th, sizeof th);
        pref(text, "gfx_video_memory", vm, sizeof vm);
        pref(text, "x_res", xr, sizeof xr);
        pref(text, "y_res", yr, sizeof yr);
        pref(text, "gfx_fullscreen", fs, sizeof fs);
        if (len)
            log("preferences: number_of_threads %s, gfx_video_memory %s, %sx%s, fullscreen %s", th, vm, xr, yr, fs);
        else
            log("preferences: preferences.script.txt not found or empty");
    }

    // Other DLLs that hook the game when placed next to it (ReShade, overlays, other mods' proxies).
    static const wchar_t *const hooks[] = {L"d3d11.dll", L"dxgi.dll", L"d3d9.dll", L"version.dll", L"winmm.dll",
                                           L"dsound.dll", L"xinput1_3.dll", L"opengl32.dll", L"wininet.dll",
                                           L"ReShade.ini", L"dxvk.conf"};
    char found[512] = "";
    int fl = 0;
    for (unsigned i = 0; i < sizeof hooks / sizeof *hooks; i++) {
        wchar_t f[MAX_PATH];
        if (wcslen(dir) + wcslen(hooks[i]) + 2 >= MAX_PATH) continue;
        swprintf(f, MAX_PATH, L"%ls\\%ls", dir, hooks[i]);
        if (GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES)
            fl += snprintf(found + fl, sizeof found - fl, " %ls", hooks[i]);
    }
    log("other hook DLLs in the game folder:%s", fl ? found : " none");

    AddVectoredExceptionHandler(0, on_exception);  // last in the chain; it only logs
    log("crash logging on: exceptions are recorded here as EXCEPTION lines");
}
