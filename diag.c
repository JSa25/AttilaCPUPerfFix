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
#include <stdarg.h>
#include <string.h>
#include <psapi.h>
#include <tlhelp32.h>

#include "version.h"
#define MOD_VERSION "v" MOD_VERSION_STR
#define MAX_RECORDS 8  // per session: the first ones are what matters, and a loop of faults must not flood the log

static wchar_t g_dlog[MAX_PATH];
static DWORD g_main_tid, g_self_base;
int g_crash_logging = 1;  // heapfix.ini [mod] crash_logging (heapfix.c)
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

// Appends to [p, end) and returns the new end of the text: never past end - 1, whatever snprintf would have needed
// (a cut-off write must not move p beyond the buffer, or the next write gets a negative, i.e. huge, size).
static char *put(char *p, char *end, const char *fmt, ...) {
    if (end - p <= 1) return p;
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(p, end - p, fmt, a);
    va_end(a);
    return n < 0 ? p : n < end - p ? p + n : end - 1;
}

// "name.dll+0x1234" for an address in a loaded module, else the bare address.
static char *where(char *p, char *end, DWORD a) {
    DWORD base;
    const char *name = image_of(a, &base);
    return name ? put(p, end, "%s+0x%lX", name, a - base) : put(p, end, "%08lX", a);
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
         : stamp == 0x6A4F4723u ? "GOG version (6A4F4723) - supported"
         : stamp == 0x698089B0u ? "older GOG version - partly supported (some fixes do not fit it and stay off, see STATUS)"
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
    p = put(p, end, "[%02d:%02d:%02d.%03d] EXCEPTION %d: 0x%08lX %s at ", t.wHour, t.wMinute, t.wSecond,
                  t.wMilliseconds, (int)nrec, code, what);
    DWORD at = (DWORD)(UINT_PTR)e->ExceptionRecord->ExceptionAddress, at_base = 0;
    const char *at_name = image_of(at, &at_base);
    p = where(p, end, at);
    p = put(p, end, ", %s thread %lu (first chance: the game may still handle it)\n",
                  tid == g_main_tid ? "main" : "worker", tid);
    // the same in plain words: whose code it happened in
    int ours = at_base && at_base == g_self_base, game = at_name && same_ascii(at_name, "empire.retail.dll");
    p = put(p, end, "  -> %s%s. If the game closed or froze right now, this is why; if it kept running, it "
                  "recovered.\n", ours ? "inside this mod (please report it)" : game ? "in the game's own code, not in this mod"
                  : "not in this mod, in ", ours || game ? "" : at_name ? at_name : "code outside any DLL");
    if (g_uikeep_tid && g_uikeep_tid == tid)
        p = put(p, end, "  -> it happened while the menu texture fix was keeping or deleting a menu picture. "
                      "To switch that fix off, create an empty uikeep_off.txt in the game folder, and please report it.\n");
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        e->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR k = e->ExceptionRecord->ExceptionInformation[0];
        p = put(p, end, "  %s address %08lX\n", k == 0 ? "read of" : k == 1 ? "write to" : "execute at",
                      (DWORD)e->ExceptionRecord->ExceptionInformation[1]);
    }
    p = put(p, end, "  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX\n",
                  c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    {   // Stack scan: values on the stack that point into a loaded module, the likely return addresses (for a stack
        // overflow: the innermost frames, the loop that ran away)
        NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
        DWORD *sp = (DWORD *)(UINT_PTR)c->Esp, *top = (DWORD *)tib->StackBase;
        int found = 0;
        DWORD last = 0;
        p = put(p, end, "  stack:");
        for (; sp < top && sp < (DWORD *)(UINT_PTR)c->Esp + 4096 && found < (code == EXCEPTION_STACK_OVERFLOW ? 40 : 24) && end - p > 80; sp++) {
            DWORD base;
            if (image_of(*sp, &base)) {
                *p++ = ' ';
                p = where(p, end, *sp);
                found++;
                last = *sp;
            }
        }
        *p++ = '\n';
        if (code == EXCEPTION_STACK_OVERFLOW && last) {
            // The rest of the overflowed stack: which modules fill it, and the frames just outside the outermost
            // occurrence of the innermost address (the loop's own return address): who started the loop.
            static DWORD bases[24], counts[24];
            static const char *names[24];
            int nmod = 0;
            DWORD *s0 = (DWORD *)(UINT_PTR)c->Esp, *first = NULL;
            for (sp = s0; sp < top; sp++) {
                DWORD base;
                const char *nm = image_of(*sp, &base);
                if (!nm) continue;
                if (*sp == last) first = sp;
                int i = 0;
                while (i < nmod && bases[i] != base) i++;
                if (i == nmod && nmod < 24) { bases[i] = base; names[i] = nm; counts[i] = 0; nmod++; }
                if (i < nmod) counts[i]++;
            }
            p = put(p, end, "  stack used %lu KB; module-pointing values:", (DWORD)((top - s0) * 4 / 1024));
            for (int i = 0; i < nmod && end - p > 80; i++) p = put(p, end, " %s %lu", names[i], counts[i]);
            p = put(p, end, "\n  outside the loop (from %lu KB below the stack top, outward):",
                          first ? (DWORD)((top - first) * 4 / 1024) : 0);
            found = 0;
            for (sp = first ? first + 1 : top; sp < top && found < 48 && end - p > 80; sp++) {
                DWORD base;
                if (*sp != last && image_of(*sp, &base)) {
                    *p++ = ' ';
                    p = where(p, end, *sp);
                    found++;
                }
            }
            *p++ = '\n';
        }
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

// ---- Main-thread sampler (AGENT_LOG 109: ~65 fps with the DLSS add-on in a big battle, 120-135 without, while the
// add-on's own measured work is ~2 ms of GPU and < 0.5 ms of main thread per frame). With sample_on.txt in the game
// folder, the main thread is paused every ~1 ms: its instruction pointer is counted per module, with the innermost game
// return address on its stack (the game call that led there); every 30 s heapfix.log gets the shares.
#define SM_MODS 192
static struct { DWORD lo, hi; char name[32]; } g_sm_mod[SM_MODS];
static int g_sm_nmod, g_sm_self = -1;
static unsigned g_sm_cnt[SM_MODS + 1];   // + not in a module
static struct { int mod; DWORD caller; unsigned n; } g_sm_pair[4096];
static struct { DWORD off; unsigned n; } g_sm_own[1024];   // our DLL: 16-byte buckets of the instruction pointer
static unsigned char *g_sm_text_lo, *g_sm_text_hi;
static unsigned g_sm_waitby[SM_MODS + 1];   // samples in a Windows module (wait, system call), by the module that called it
static DWORD g_sm_empire;

static void sm_modules(void) {
    HMODULE m[SM_MODS];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), m, sizeof m, &need)) return;
    int n = (int)(need / sizeof *m) < SM_MODS ? (int)(need / sizeof *m) : SM_MODS, k = 0;
    for (int i = 0; i < n; i++) {
        MODULEINFO mi;
        char name[MAX_PATH];
        if (!K32GetModuleInformation(GetCurrentProcess(), m[i], &mi, sizeof mi) || !GetModuleFileNameA(m[i], name, MAX_PATH)) continue;
        const char *b = strrchr(name, '\\');
        g_sm_mod[k].lo = (DWORD)(UINT_PTR)mi.lpBaseOfDll;
        g_sm_mod[k].hi = g_sm_mod[k].lo + mi.SizeOfImage;
        snprintf(g_sm_mod[k].name, sizeof g_sm_mod[k].name, "%s", b ? b + 1 : name);
        if (g_sm_mod[k].lo == g_self_base) g_sm_self = k;
        k++;
    }
    g_sm_nmod = k;
}
static int sm_find(DWORD a) {
    for (int i = 0; i < g_sm_nmod; i++) if (a >= g_sm_mod[i].lo && a < g_sm_mod[i].hi) return i;
    return SM_MODS;
}
static int sm_is_return(const unsigned char *r) {   // as profiler.c: a game code address right after a CALL
    if (r < g_sm_text_lo + 7 || r >= g_sm_text_hi) return 0;
    if (r[-5] == 0xE8) return 1;
    if (r[-2] == 0xFF && (r[-1] & 0x38) == 0x10 && ((r[-1] & 0xC0) == 0xC0 || ((r[-1] & 0xC0) == 0 && (r[-1] & 7) != 4 && (r[-1] & 7) != 5))) return 1;
    if (r[-3] == 0xFF && (r[-2] & 0x38) == 0x10 && (r[-2] & 0xC0) == 0x40) return 1;
    if (r[-6] == 0xFF && (r[-5] & 0x38) == 0x10 && ((r[-5] & 0xC0) == 0x80 || r[-5] == 0x15)) return 1;
    if (r[-7] == 0xFF && (r[-6] & 0x38) == 0x10 && (r[-6] & 0xC0) == 0x80 && (r[-6] & 7) == 4) return 1;
    return 0;
}
static int sm_system(int m) {   // Windows' own layers: who called into them is the interesting part
    if (m >= g_sm_nmod) return 0;
    const char *n = g_sm_mod[m].name;
    return !_stricmp(n, "ntdll.dll") || !_stricmp(n, "KERNELBASE.dll") || !_stricmp(n, "KERNEL32.DLL") || !_stricmp(n, "win32u.dll");
}
static int sm_is_call_return(DWORD v, int m) {   // v in module m, right after a CALL (any module; the bytes are mapped)
    const unsigned char *r = (const unsigned char *)(UINT_PTR)v;
    if (v < g_sm_mod[m].lo + 8) return 0;
    MEMORY_BASIC_INFORMATION mb;
    if (!VirtualQuery(r - 8, &mb, sizeof mb) || mb.State != MEM_COMMIT || !(mb.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE | PAGE_EXECUTE_READWRITE))) return 0;
    if (r[-5] == 0xE8) return 1;
    if (r[-2] == 0xFF && (r[-1] & 0x38) == 0x10) return 1;
    if (r[-3] == 0xFF && (r[-2] & 0x38) == 0x10 && (r[-2] & 0xC0) == 0x40) return 1;
    if (r[-6] == 0xFF && (r[-5] & 0x38) == 0x10 && ((r[-5] & 0xC0) == 0x80 || r[-5] == 0x15)) return 1;
    return 0;
}
// CPU time of every thread of the process (and of the add-on's helper process) since the last report
#define SM_THREADS 256
static struct { DWORD tid; ULONG64 cpu; int seen; } g_sm_thr[SM_THREADS];
static ULONG64 g_sm_helper_cpu;
static void sm_threads(void (*log)(const char *fmt, ...), double secs) {
    typedef LONG(NTAPI * qit_fn)(HANDLE, int, void *, ULONG, ULONG *);
    static qit_fn qit;
    if (!qit) qit = (qit_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD | TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    struct { DWORD tid; double pct; DWORD start; } top[13] = {{0}};
    double total = 0;
    THREADENTRY32 te = {sizeof te};
    for (int i = 0; i < SM_THREADS; i++) g_sm_thr[i].seen = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME a, b2, k, u;
        DWORD start = 0;
        if (qit) qit(h, 9, &start, sizeof start, NULL);   // ThreadQuerySetWin32StartAddress
        if (GetThreadTimes(h, &a, &b2, &k, &u)) {
            ULONG64 cpu = ((ULONG64)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONG64)u.dwHighDateTime << 32 | u.dwLowDateTime);
            int slot = -1, free_ = -1;
            for (int i = 0; i < SM_THREADS; i++) {
                if (g_sm_thr[i].tid == te.th32ThreadID) { slot = i; break; }
                if (!g_sm_thr[i].tid && free_ < 0) free_ = i;
            }
            double pct = slot >= 0 ? (double)(cpu - g_sm_thr[slot].cpu) / 1e5 / secs : 0;   // 100 ns units -> % of one CPU
            if (slot < 0) slot = free_;
            if (slot >= 0) { g_sm_thr[slot].tid = te.th32ThreadID; g_sm_thr[slot].cpu = cpu; g_sm_thr[slot].seen = 1; }
            total += pct;
            for (int i = 0; i < 12; i++)
                if (pct > top[i].pct) {
                    memmove(&top[i + 1], &top[i], (11 - i) * sizeof *top);
                    top[i].tid = te.th32ThreadID; top[i].pct = pct; top[i].start = start;
                    break;
                }
        }
        CloseHandle(h);
    }
    for (int i = 0; i < SM_THREADS; i++) if (!g_sm_thr[i].seen) g_sm_thr[i].tid = 0;
    double helper = -1;
    PROCESSENTRY32W pe = {sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ParentProcessID != GetCurrentProcessId() || _wcsicmp(pe.szExeFile, L"helper64.exe")) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        FILETIME a, b2, k, u;
        if (h && GetProcessTimes(h, &a, &b2, &k, &u)) {
            ULONG64 cpu = ((ULONG64)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONG64)u.dwHighDateTime << 32 | u.dwLowDateTime);
            if (g_sm_helper_cpu) helper = (double)(cpu - g_sm_helper_cpu) / 1e5 / secs;
            g_sm_helper_cpu = cpu;
        }
        if (h) CloseHandle(h);
    }
    CloseHandle(snap);
    char line[1400] = "";
    int len = 0;
    for (int i = 0; i < 12 && top[i].pct >= 2; i++) {
        int m = sm_find(top[i].start);
        len += snprintf(line + len, sizeof line - len, "%s %lu%s %s+0x%X %.0f%%", i ? "," : "", (unsigned long)top[i].tid,
                        top[i].tid == g_main_tid ? " (main)" : "", m < g_sm_nmod ? g_sm_mod[m].name : "?",
                        m < g_sm_nmod ? (unsigned)(top[i].start - g_sm_mod[m].lo) : (unsigned)top[i].start, top[i].pct);
    }
    log("  threads by CPU (%% of one CPU; thread id, start address): all %.0f%%;%s", total, line);
    if (helper >= 0) log("  the add-on's helper64.exe: %.0f%% of one CPU", helper);
}
static void sm_report(void (*log)(const char *fmt, ...), unsigned total, double secs) {
    char line[1400];
    int len = 0;
    unsigned used[16] = {0};
    for (int t = 0; t < 14; t++) {   // modules, largest first
        int best = -1;
        for (int i = 0; i <= SM_MODS; i++) {
            int taken = 0;
            for (int j = 0; j < t; j++) taken |= used[j] == (unsigned)i + 1;
            if (!taken && g_sm_cnt[i] && (best < 0 || g_sm_cnt[i] > g_sm_cnt[best])) best = i;
        }
        if (best < 0 || g_sm_cnt[best] * 200 < total) break;
        used[t] = (unsigned)best + 1;
        len += snprintf(line + len, sizeof line - len, "%s %s %.1f%%", t ? "," : "", best == SM_MODS ? "(no module)" : g_sm_mod[best].name,
                        100.0 * g_sm_cnt[best] / total);
    }
    log("main-thread sampler: %u samples in %.0f s:%s", total, secs, line);
    len = 0;
    for (int t = 0; t < 16; t++) {   // where in which module, by the game call that led there
        int best = -1;
        for (int i = 0; i < 4096; i++) if (g_sm_pair[i].n && (best < 0 || g_sm_pair[i].n > g_sm_pair[best].n)) best = i;
        if (best < 0 || g_sm_pair[best].n * 100 < total) break;
        int m = g_sm_pair[best].mod;
        len += snprintf(line + len, sizeof line - len, "%s %s via game +0x%X %.1f%%", t ? "," : "", m == SM_MODS ? "(no module)" : g_sm_mod[m].name,
                        (unsigned)g_sm_pair[best].caller, 100.0 * g_sm_pair[best].n / total);
        g_sm_pair[best].n = 0;
    }
    log("  main thread by the game call it came from:%s", line);
    len = 0;
    for (int t = 0; t < 12 && g_sm_self >= 0; t++) {   // inside our DLL: offsets for a symbol lookup in an unstripped build
        int best = -1;
        for (int i = 0; i < 1024; i++) if (g_sm_own[i].n && (best < 0 || g_sm_own[i].n > g_sm_own[best].n)) best = i;
        if (best < 0) break;
        len += snprintf(line + len, sizeof line - len, " +0x%X %.2f%%", (unsigned)g_sm_own[best].off, 100.0 * g_sm_own[best].n / total);
        g_sm_own[best].n = 0;
    }
    if (len) log("  in dinput8.dll (16-byte steps):%s", line);
    len = 0;
    for (int t = 0; t < 10; t++) {   // samples inside Windows (waits, system calls), by the module that called into it
        int best = -1;
        for (int i = 0; i <= SM_MODS; i++) if (g_sm_waitby[i] && (best < 0 || g_sm_waitby[i] > g_sm_waitby[best])) best = i;
        if (best < 0 || g_sm_waitby[best] * 200 < total) break;
        len += snprintf(line + len, sizeof line - len, "%s %s %.1f%%", t ? "," : "", best == SM_MODS ? "(not found)" : g_sm_mod[best].name,
                        100.0 * g_sm_waitby[best] / total);
        g_sm_waitby[best] = 0;
    }
    if (len) log("  main thread inside Windows (waits, system calls), by the module that called:%s", line);
    sm_threads(log, secs);
    memset(g_sm_cnt, 0, sizeof g_sm_cnt);
    memset(g_sm_waitby, 0, sizeof g_sm_waitby);
    memset(g_sm_pair, 0, sizeof g_sm_pair);
    memset(g_sm_own, 0, sizeof g_sm_own);
}
static wchar_t g_sm_flag[MAX_PATH];

static void (*g_sm_log)(const char *fmt, ...);
static DWORD WINAPI sampler_thread(void *arg) {
    (void)arg;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, g_main_tid);
    if (!th) return 0;
    for (;;) {
        if (GetFileAttributesW(g_sm_flag) == INVALID_FILE_ATTRIBUTES) { Sleep(1000); continue; }
        timeBeginPeriod(1);
        sm_modules();
        g_sm_log("main-thread sampler on (sample_on.txt): the main thread's place every ~1 ms, shares every 30 s");
        LARGE_INTEGER f, t0, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        unsigned total = 0, since_mod = 0;
        for (int checks = 0;; checks++) {
            if (checks % 1000 == 999 && GetFileAttributesW(g_sm_flag) == INVALID_FILE_ATTRIBUTES) break;
            Sleep(1);
            CONTEXT c = {.ContextFlags = CONTEXT_CONTROL};
            DWORD eip = 0, caller = 0, nst = 0;
            static DWORD st[512];   // the top of its stack, looked at after it runs again
            if (SuspendThread(th) == (DWORD)-1) break;
            if (GetThreadContext(th, &c)) {   // only reads while it is paused: it may hold the loader or heap lock
                eip = c.Eip;
                MEMORY_BASIC_INFORMATION mb;
                if (VirtualQuery((void *)(UINT_PTR)c.Esp, &mb, sizeof mb) && mb.State == MEM_COMMIT) {
                    const DWORD *sp = (const DWORD *)(UINT_PTR)c.Esp, *end = (const DWORD *)((const unsigned char *)mb.BaseAddress + mb.RegionSize);
                    if (end - sp > 16384) end = sp + 16384;
                    nst = (DWORD)(end - sp < 512 ? end - sp : 512);
                    memcpy(st, sp, nst * 4);
                    for (; sp < end && !caller; sp++) if (sm_is_return((const unsigned char *)(UINT_PTR)*sp)) caller = *sp;
                    if (sm_is_return((const unsigned char *)(UINT_PTR)eip)) caller = 0;   // in game code itself: no call to show
                }
            }
            ResumeThread(th);
            if (!eip) continue;
            int m = sm_find(eip);
            if (m == SM_MODS && since_mod++ > 1000) { sm_modules(); m = sm_find(eip); since_mod = 0; }   // a DLL loaded since
            g_sm_cnt[m]++;
            total++;
            if (sm_system(m)) {   // in Windows: the first return address into a module of its own
                int who = SM_MODS;
                for (DWORD i = 0; i < nst && who == SM_MODS; i++) {
                    int k = sm_find(st[i]);
                    if (k < g_sm_nmod && !sm_system(k) && sm_is_call_return(st[i], k)) who = k;
                }
                g_sm_waitby[who]++;
            }
            DWORD cv = caller ? caller - g_sm_empire : 0;
            for (unsigned h = ((unsigned)m * 2654435761u ^ cv * 40503u) & 4095, k = 0; k < 4096; k++, h = (h + 1) & 4095)
                if (!g_sm_pair[h].n || (g_sm_pair[h].mod == m && g_sm_pair[h].caller == cv)) { g_sm_pair[h].mod = m; g_sm_pair[h].caller = cv; g_sm_pair[h].n++; break; }
            if (m == g_sm_self && g_sm_self >= 0) {
                DWORD off = (eip - g_sm_mod[m].lo) & ~15u;
                for (unsigned h = (off >> 4) & 1023, k = 0; k < 1024; k++, h = (h + 1) & 1023)
                    if (!g_sm_own[h].n || g_sm_own[h].off == off) { g_sm_own[h].off = off; g_sm_own[h].n++; break; }
            }
            QueryPerformanceCounter(&t);
            double secs = (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
            if (secs >= 30) { sm_report(g_sm_log, total, secs); total = 0; t0 = t; }
        }
        timeEndPeriod(1);
    }
    return 0;
}
static void sampler_start(HMODULE empire, const wchar_t *dir, void (*log)(const char *fmt, ...)) {
    if (wcslen(dir) + 20 >= MAX_PATH) return;
    swprintf(g_sm_flag, MAX_PATH, L"%ls\\sample_on.txt", dir);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const BYTE *)empire + ((const IMAGE_DOS_HEADER *)empire)->e_lfanew);
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            g_sm_text_lo = (unsigned char *)empire + s->VirtualAddress;
            g_sm_text_hi = g_sm_text_lo + s->Misc.VirtualSize;
            break;
        }
    g_sm_empire = (DWORD)(UINT_PTR)empire;
    g_sm_log = log;
    HANDLE h = CreateThread(NULL, 0, sampler_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
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
    log("Attila Engine Performance Fix %s loaded into %ls", MOD_VERSION, exe);
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

    if (!g_crash_logging)
        log("crash logging off (crash_logging=0 in heapfix.ini)");
    else {
        AddVectoredExceptionHandler(0, on_exception);  // last in the chain; it only logs
        log("crash logging on: exceptions are recorded here as EXCEPTION lines");
    }
    sampler_start(empire, dir, log);
}
