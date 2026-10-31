// dinput8.dll proxy for Total War: ATTILA 1.6.0 (empire.retail.dll of 2026-09-19).
// The game routes every allocation over 80 bytes through one global dlmalloc heap guarded by a
// single spinlock, so its worker threads and the main thread spend most of their time waiting on
// it. This sends blocks up to 16 KB (all but ~0.02% of the calls, measured) to 8 dlmalloc pools with the game's own
// settings (pools.c), which take address space like the game's heap but do not serialize threads, and keeps larger
// ones on the game's own dlmalloc, called through copies of its first instructions.
// dlmalloc gives big blocks' address space back (the game trims it), which the 32-bit game needs: a separate
// Windows heap for them kept ~700 MB more reserved and crashed heavy mods (Mk1212) out of address space.
// empire.retail.dll imports DINPUT8.dll, so this loads before the game's own startup code runs,
// i.e. before the first allocation.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "version.h"
#include "logtrim.h"

#define DL_MAX_REQUEST 0xFFFFFFC0u   // dlmalloc fails requests this big; keep that behaviour
#define PREFERRED_BASE 0x10000000u
#define RVA_MPARAMS_MAGIC 0x01F20BBCu // dlmalloc mparams.magic: non-zero once dlmalloc was used

static wchar_t g_log_path[MAX_PATH];

static void wlog(const char *fmt, ...) {
    FILE *f = _wfopen(g_log_path, L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputc('\n', f);
    fclose(f);
}


// Free and largest free block of this process's (32-bit) address space.
void address_space(size_t *free_total, size_t *largest) {
    MEMORY_BASIC_INFORMATION m;
    *free_total = *largest = 0;
    for (unsigned char *a = (unsigned char *)0x10000; VirtualQuery(a, &m, sizeof m) == sizeof m; a = (unsigned char *)m.BaseAddress + m.RegionSize) {
        if (m.State == MEM_FREE) {
            *free_total += m.RegionSize;
            if (m.RegionSize > *largest) *largest = m.RegionSize;
        }
        if ((unsigned char *)m.BaseAddress + m.RegionSize < a) break;  // wrapped past the top
    }
}

static void log_failure(const char *what, size_t n) {
    size_t free_total, largest;
    address_space(&free_total, &largest);
    wlog("%s of %u bytes FAILED (address space: %u MB free, largest free block %u MB)", what,
         (unsigned)n, (unsigned)(free_total >> 20), (unsigned)(largest >> 20));
}

// Blocks up to this size go to the pools (pools.c).
#define SMALL_MAX 16384

// The game's dlmalloc (set by install_into): trampolines that run its first instructions, then the rest of it.
static void *(__cdecl *dl_malloc)(size_t);
static void(__cdecl *dl_free)(void *);
static void *(__cdecl *dl_realloc)(void *, size_t);
static int(__cdecl *dl_trim)(size_t);

#ifdef HEAPFIX_MEMSTATS
// Dev builds: bytes live on dlmalloc (from its chunk headers) and a log line every 20 s, to see where address space goes.
static volatile LONG g_dl_live, g_dl_calls, g_spill_calls;
unsigned pool_mb(void);
static size_t dl_chunk(void *p) { return p ? (((size_t *)p)[-1] & ~(size_t)7) : 0; }
static void *dl_malloc_c(size_t n) { InterlockedIncrement(&g_dl_calls); if (n <= SMALL_MAX) InterlockedIncrement(&g_spill_calls); void *p = dl_malloc(n); InterlockedExchangeAdd(&g_dl_live, (LONG)dl_chunk(p)); return p; }
static void dl_free_c(void *p) { InterlockedIncrement(&g_dl_calls); InterlockedExchangeAdd(&g_dl_live, -(LONG)dl_chunk(p)); dl_free(p); }
static void *dl_realloc_c(void *p, size_t n) {
    InterlockedIncrement(&g_dl_calls);
    size_t was = dl_chunk(p);
    void *q = dl_realloc(p, n);
    if (q) InterlockedExchangeAdd(&g_dl_live, (LONG)dl_chunk(q) - (LONG)was);
    return q;
}
void address_space(size_t *free_total, size_t *largest);
static DWORD WINAPI memstats(LPVOID a) {
    (void)a;
    for (;;) {
        Sleep(20000);
        size_t fr, big;
        address_space(&fr, &big);
        LONG calls = InterlockedExchange(&g_dl_calls, 0), spills = InterlockedExchange(&g_spill_calls, 0);
        wlog("memstats: pools %u MB; dlmalloc live %u MB, %ld calls/s (%ld small spills/s); address space free %u MB, "
             "largest %u MB", pool_mb(), (unsigned)(g_dl_live >> 20), calls / 20, spills / 20, (unsigned)(fr >> 20),
             (unsigned)(big >> 20));
    }
    return 0;
}
#define DL_MALLOC dl_malloc_c
#define DL_FREE dl_free_c
#define DL_REALLOC dl_realloc_c
#else
#define DL_MALLOC dl_malloc
#define DL_FREE dl_free
#define DL_REALLOC dl_realloc
#endif

// pools.c: 8 dlmalloc mspaces with the game's own settings and a per-thread cache (AGENT_LOG 143)
int pools_init(void);
int pool_owns(const void *p);
unsigned pool_mb(void);
void *pool_alloc(size_t n);
void pool_free(void *p);
void *pool_realloc(void *p, size_t n);
size_t pool_usable(const void *p);
void pool_thread_exit(void);
void pool_trim(void);

static void *h_alloc(size_t n) {
    if (n <= SMALL_MAX) {
        void *p = pool_alloc(n);
        if (p) return p;
    }
    return DL_MALLOC(n);
}

static void h_free(void *p) {
    if (pool_owns(p)) pool_free(p);
    else DL_FREE(p);
}

static void *h_realloc(void *p, size_t n) {
    if (!pool_owns(p)) return DL_REALLOC(p, n);   // large blocks stay with the game's heap, even when shrinking
    if (n <= SMALL_MAX) {
        void *q = pool_realloc(p, n);
        if (q) return q;
    }
    void *q = DL_MALLOC(n);   // grew past the small limit (or the pools are out of address space): move it
    if (!q) return NULL;      // like dlrealloc: the old block stays valid
    size_t old = pool_usable(p);
    memcpy(q, p, old < n ? old : n);
    pool_free(p);
    return q;
}

static int __cdecl hf_trim(size_t pad) {   // the game's trim: its own heap, then the pools
    int r = dl_trim(pad);
    pool_trim();
    return r;
}

static void *__cdecl hf_malloc(size_t n) {
    if (n >= DL_MAX_REQUEST) return NULL;
    void *p;
    p = h_alloc(n ? n : 1);
    if (!p) log_failure("malloc", n);
    return p;
}

static void __cdecl hf_free(void *p) {
    if (!p) return;
    h_free(p);
}

static void *__cdecl hf_realloc(void *p, size_t n) {
    if (!p) return hf_malloc(n);
    if (n >= DL_MAX_REQUEST) return NULL;  // like dlrealloc: the old block stays valid
    void *r;
    r = h_realloc(p, n ? n : 1);
    if (!r) log_failure("realloc", n);
    return r;
}

struct patch {
    const char *name;
    DWORD rva;
    unsigned char expect[12];
    int reloc;      // offset of an absolute address inside expect, or -1
    void *to;       // the replacement
    unsigned copy;  // whole instructions moved into the trampoline (nothing jumps into them; checked in the disassembly)
    void **orig;    // gets the trampoline: the original function
};

static struct patch g_patches[] = {
    {"dlmalloc",  0x112D80, {0x83,0xEC,0x08,0x83,0x3D,0xBC,0x0B,0xF2,0x11,0x00,0x75,0x05}, 5,  (void *)hf_malloc, 10, (void **)&dl_malloc},
    {"dlfree",    0x112730, {0x83,0xEC,0x1C,0x56,0x8B,0x74,0x24,0x24,0x85,0xF6,0x0F,0x84}, -1, (void *)hf_free, 8, (void **)&dl_free},
    {"dlrealloc", 0x113040, {0x53,0x8B,0x5C,0x24,0x08,0x85,0xDB,0x75,0x0E,0xFF,0x74,0x24}, -1, (void *)hf_realloc, 5, (void **)&dl_realloc},
    {"dltrim",    0x1126E0, {0x83,0x3D,0xBC,0x0B,0xF2,0x11,0x00,0x75,0x05,0xE8,0x32,0xE8}, 2,  (void *)hf_trim, 7, (void **)&dl_trim},
};
#define NPATCH (sizeof g_patches / sizeof g_patches[0])

// Returns the number of functions patched: all of them, or 0 if anything does not match.
static int install_into(HMODULE m) {
    unsigned char *base = (unsigned char *)m;
    DWORD delta = (DWORD)(base - (unsigned char *)PREFERRED_BASE);

    for (size_t i = 0; i < NPATCH; i++) {
        unsigned char want[12];
        memcpy(want, g_patches[i].expect, sizeof want);
        if (g_patches[i].reloc >= 0) {
            DWORD v;
            memcpy(&v, want + g_patches[i].reloc, 4);
            v += delta;
            memcpy(want + g_patches[i].reloc, &v, 4);
        }
        if (memcmp(base + g_patches[i].rva, want, sizeof want) != 0) {
            wlog("%s at +0x%X does not match this game version - nothing patched", g_patches[i].name, g_patches[i].rva);
            return 0;
        }
    }
    // Pointers dlmalloc already handed out could not be freed by the new heap.
    if (*(volatile DWORD *)(base + RVA_MPARAMS_MAGIC) != 0) {
        wlog("dlmalloc was already in use when this loaded - nothing patched");
        return 0;
    }
    unsigned char *tramp = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        wlog("VirtualAlloc for the trampolines failed (%lu) - nothing patched", GetLastError());
        return 0;
    }
    for (size_t i = 0; i < NPATCH; i++) {  // original first instructions, then a jump to the rest
        if (!g_patches[i].to) continue;
        unsigned char *t = tramp + 32 * i, *site = base + g_patches[i].rva;
        memcpy(t, site, g_patches[i].copy);
        t[g_patches[i].copy] = 0xE9;
        DWORD rel = (DWORD)((site + g_patches[i].copy) - (t + g_patches[i].copy + 5));
        memcpy(t + g_patches[i].copy + 1, &rel, 4);
        *g_patches[i].orig = t;
    }
    FlushInstructionCache(GetCurrentProcess(), tramp, 4096);
    if (!pools_init()) {
        wlog("the small-block pools could not be set up - nothing patched");
        return 0;
    }
    for (size_t i = 0; i < NPATCH; i++) {
        if (!g_patches[i].to) continue;
        unsigned char *site = base + g_patches[i].rva;
        DWORD old;
        if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
            // Only reachable for the first patch in practice; a partial install would mix heaps.
            wlog("VirtualProtect failed at %s (%lu)%s", g_patches[i].name, GetLastError(),
                 i ? " - GAME STATE UNSAFE, remove dinput8.dll" : " - nothing patched");
            return 0;
        }
        site[0] = 0xE9;
        DWORD rel = (DWORD)((unsigned char *)g_patches[i].to - (site + 5));
        memcpy(site + 1, &rel, 4);
        VirtualProtect(site, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
    }
    wlog("empire.retail.dll at %p: game heap replaced by 8 dlmalloc pools with the game's own settings (blocks <= 16 KB, "
         "nothing reserved up front, grown in 64 KB pieces like the game's heap); larger blocks stay on the game's heap", base);
#ifdef HEAPFIX_MEMSTATS
    CloseHandle(CreateThread(NULL, 0, memstats, NULL, 0, NULL));
#endif
    return (int)NPATCH;
}

// ---- dinput8 forwarding (the real DLL is loaded on first use, never inside DllMain)
static HMODULE g_real;

static FARPROC real_proc(const char *name) {
    if (!g_real) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for this 32-bit game
        if (n == 0 || n > MAX_PATH - 14) return NULL;
        wcscat(path, L"\\dinput8.dll");
        g_real = LoadLibraryW(path);
        if (!g_real) return NULL;
    }
    return GetProcAddress(g_real, name);
}

void (*g_dev_di8_created)(void *di);   // the frame bridge panel filters the game's mouse while open (fgpanel.c)
HRESULT WINAPI stub_DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID *o, LPUNKNOWN u) {
    typedef HRESULT(WINAPI * fn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    fn f = (fn)real_proc("DirectInput8Create");
    if (f && g_dev_di8_created) {
        HRESULT hr = f(h, v, r, o, u);
        if (SUCCEEDED(hr) && o && *o) g_dev_di8_created(*o);
        return hr;
    }
    return f ? f(h, v, r, o, u) : E_FAIL;
}
HRESULT WINAPI stub_DllCanUnloadNow(void) {
    typedef HRESULT(WINAPI * fn)(void);
    fn f = (fn)real_proc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI stub_DllGetClassObject(REFCLSID c, REFIID r, LPVOID *o) {
    typedef HRESULT(WINAPI * fn)(REFCLSID, REFIID, LPVOID *);
    fn f = (fn)real_proc("DllGetClassObject");
    return f ? f(c, r, o) : E_FAIL;
}
HRESULT WINAPI stub_DllRegisterServer(void) {
    typedef HRESULT(WINAPI * fn)(void);
    fn f = (fn)real_proc("DllRegisterServer");
    return f ? f() : E_FAIL;
}
HRESULT WINAPI stub_DllUnregisterServer(void) {
    typedef HRESULT(WINAPI * fn)(void);
    fn f = (fn)real_proc("DllUnregisterServer");
    return f ? f() : E_FAIL;
}

#ifndef HEAPFIX_TEST
int install_pool(HMODULE m, void (*log)(const char *fmt, ...));  // pool.c
int install_sortfix(HMODULE m, void (*log)(const char *fmt, ...));  // sortfix.c
int install_fps(HMODULE m, const wchar_t *dir, void (*log)(const char *fmt, ...));  // fps.c
int install_batchfix(HMODULE m, void (*log)(const char *fmt, ...));  // batchfix.c
int install_campfix(HMODULE m, int ab, void (*log)(const char *fmt, ...));  // campfix.c
void campfix_frame(void);                                            // campfix.c
int install_x87fix(HMODULE m, void (*log)(const char *fmt, ...));    // x87fix.c
void x87fix_frame(void);                                             // x87fix.c
int install_heightcache(HMODULE m, int mode, void (*log)(const char *fmt, ...));  // heightcache.c: 1 verify, 2 cache
int install_memfast(HMODULE m, void (*log)(const char *fmt, ...));  // memfast.c
int install_loopfetch(HMODULE m, void (*log)(const char *fmt, ...));  // loopfetch.c
int install_fileio(HMODULE m, void (*log)(const char *fmt, ...));     // fileio.c
int install_attrcache(HMODULE m, int verify, void (*log)(const char *fmt, ...));  // attrcache.c
int install_texfix(HMODULE m, int verify, void (*log)(const char *fmt, ...));    // texfix.c
int install_uikeep(HMODULE m, void (*log)(const char *fmt, ...));                // uikeep.c
void diag_startup(HMODULE empire, const wchar_t *log_path, void (*log)(const char *fmt, ...));  // diag.c
void update_check(HMODULE self, void (*log)(const char *fmt, ...));                             // update.c
extern unsigned g_uikeep_keep;                                       // uikeep.c: closed menus kept (heapfix.ini)
extern int g_crash_logging;                                          // diag.c (heapfix.ini [mod] crash_logging)
extern void (*g_frame_check)(void);                                  // fps.c
// release (6.3, AGENT_LOG 104): the frame bridge (bridgeprobe.c, fgbridge.c, ...) for the upscaling add-on runs on passtime's
// stage-driver hook in its lean bridge mode; it starts only when the add-on folder holds the helper
int install_passtime(HMODULE m, void (*log)(const char *fmt, ...));                        // passtime.c
void passtime_report(void (*log)(const char *fmt, ...));                                   // passtime.c
extern void (*g_gt_frame)(void);                                                           // passtime.c
extern int g_pt_force_bridge;                                                              // passtime.c
volatile LONG g_geo_frame;
static int g_passtime_on;

static int g_campfix_on, g_x87fix_on;

// Per-frame checks on the main thread (called from the frame hook in fps.c).
static void frame_checks(void) {
    if (g_campfix_on) campfix_frame();
    if (g_x87fix_on) x87fix_frame();
    if (g_passtime_on)   // release: the frame bridge's frame counter
    {
        g_geo_frame++;
        if (g_gt_frame) g_gt_frame();  // gputime.c: close the GPU frame, open the next
    }
    if (g_passtime_on) {  // the game exits without DLL_PROCESS_DETACH: report every 30 s
        static DWORD last;
        DWORD now = GetTickCount();
        if (!last) last = now;
        if (now - last >= 30000) {
            passtime_report(wlog);
            last = now;
        }
    }
}

static wchar_t g_ini[MAX_PATH];
static void set_log_path(HINSTANCE h) {
    DWORD n = GetModuleFileNameW(h, g_log_path, MAX_PATH);
    wchar_t *slash = wcsrchr(g_log_path, L'\\');
    if (n == 0 || n >= MAX_PATH || !slash || slash - g_log_path > MAX_PATH - 40) {  // room for FrameBridge\settings.txt
        g_log_path[0] = 0;
        return;
    }
    wcscpy(slash + 1, L"heapfix.log");
    wcscpy(g_ini, g_log_path);
    wcscpy(g_ini + (slash + 1 - g_log_path), L"heapfix.ini");
}

// heapfix.ini in the game folder (as Rome 2's rome2fix.ini): written once with the defaults when missing, never
// changed by the mod after that.
static const char INI_DEFAULTS[] =
    "; Attila Engine Performance Fix settings. 1 = on, 0 = off. Restart the game after a change.\r\n"
    "; The mod writes this file once with the defaults and never changes it. Delete it to get the defaults back.\r\n"
    "; (The older way still works too: an empty <name>_off.txt file in the game folder switches that fix off.)\r\n"
    "\r\n"
    "[fixes]\r\n"
    "; FPS\r\n"
    "memory_allocator=1\r\n"
    "small_object_pools=1\r\n"
    "render_list_sorts=1\r\n"
    "battle_draw_batching=1\r\n"
    "campaign_rounding=1\r\n"
    "x87_rounding=1\r\n"
    "terrain_height_cache=1\r\n"
    "small_memory_copies=1\r\n"
    "render_loop_prefetch=1\r\n"
    "; loading\r\n"
    "file_reading=1\r\n"
    "missing_file_cache=1\r\n"
    "battle_textures=1\r\n"
    "; menus\r\n"
    "menu_textures=1\r\n"
    "\r\n"
    "[mod]\r\n"
    "; copy a newer version from the Steam Workshop into the game folder at startup\r\n"
    "self_update=1\r\n"
    "; record crashes in heapfix.log (EXCEPTION lines)\r\n"
    "crash_logging=1\r\n"
    "\r\n"
    "[tuning]\r\n"
    "; how many closed menus keep their textures (1-1024; fewer uses less memory)\r\n"
    "menu_textures_kept=1024\r\n"
    "\r\n"
    "; The DLSS add-on has its own settings: FrameBridge\\settings.txt (F11 in game).\r\n";
static void ini_create(void) {
    if (!g_ini[0]) return;
    HANDLE f = CreateFileW(g_ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);  // only when missing
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, INI_DEFAULTS, sizeof INI_DEFAULTS - 1, &w, NULL);
    CloseHandle(f);
}
static int ini_int(const wchar_t *section, const wchar_t *key, int def) {
    return g_ini[0] ? (int)GetPrivateProfileIntW(section, key, def, g_ini) : def;
}
// 1 when a fix is switched off: <key>=0 in [fixes] of heapfix.ini, or the older <name>_off.txt in the game folder.
static int fix_off(const wchar_t *key, const wchar_t *old_file, const char *name, const char *then) {
    wchar_t p[MAX_PATH];
    wcscpy(p, g_log_path);
    wchar_t *sl = wcsrchr(p, L'\\');
    int file = sl && wcslen(old_file) + (sl + 1 - p) < MAX_PATH && (wcscpy(sl + 1, old_file), GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES);
    if (ini_int(L"fixes", key, 1) && !file) return 0;
    wlog("%s switched off (heapfix.ini or %ls) - %s", name, old_file, then);
    return 1;
}

static HINSTANCE g_self;


// One line per game start that stands out in the log: version and date.
static void log_header(void) {
    trim_log(g_log_path);
    FILE *f = _wfopen(g_log_path, L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "\n======== Attila Engine Performance Fix v%s - game started %04d-%02d-%02d %02d:%02d ========\n",
            MOD_VERSION_STR, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    fclose(f);
}

// The plain-language summary after the install lines (those stay as the details for bug reports).
enum { FIX_SWITCHED_OFF = -1, FIX_INACTIVE = 0, FIX_ON = 1, FIX_PARTLY = 2 };
enum { S_HEAP, S_POOL, S_SORTS, S_BATCH, S_X87, S_HEIGHT, S_COPY, S_PREFETCH, S_FILES, S_MISSING, S_TEXTURES, S_MENUS, S_N };
static const char *const g_fix_names[S_N] = {
    "FPS: memory allocator (heap)", "FPS: small-object pools", "FPS: render-list sorts", "FPS: battle draw batching",
    "FPS: x87 rounding", "FPS: terrain height cache", "FPS: small memory copies", "FPS: render-loop prefetch",
    "Loading: file reading", "Loading: missing-file cache", "Loading: battle textures (sieges)",
    "Menus: textures stay loaded (no stutter on reopening)"};

static void log_status(const int st[S_N]) {
    int on = 0;
    for (int i = 0; i < S_N; i++) on += st[i] == FIX_ON;
    if (on == S_N)
        wlog("STATUS: all %d fixes are on", S_N);
    else
        wlog("STATUS: %d of %d fixes are on", on, S_N);
    for (int i = 0; i < S_N; i++)
        wlog("  %-4s %s%s", st[i] == FIX_ON ? "on" : st[i] == FIX_PARTLY ? "part" : "OFF", g_fix_names[i],
             st[i] == FIX_SWITCHED_OFF ? " (switched off in heapfix.ini, or by its _off.txt file)"
             : st[i] == FIX_PARTLY ? " (only part of it fits this game version)"
             : st[i] == FIX_INACTIVE ? " (does not fit this game version, so it stays off; details above)" : "");
}

// Everything that patches the game, run once empire.retail.dll is in memory and before its own startup code.
static void install_all(HMODULE m) {
    static int done;
    if (done) return;
    done = 1;
    int st[S_N] = {0};
    ini_create();
    g_crash_logging = ini_int(L"mod", L"crash_logging", 1) != 0;
    diag_startup(m, g_log_path, wlog);  // version, process, game build, system, settings; crash records
    if (ini_int(L"mod", L"self_update", 1))
        update_check(g_self, wlog);     // a newer Workshop copy replaces this DLL for the next launch
    else
        wlog("self-update off (self_update=0 in heapfix.ini)");
    wchar_t off[MAX_PATH];
    wcscpy(off, g_log_path);
    wchar_t *sl = wcsrchr(off, L'\\');
    // Switching the heap or the pool lookup off tells whether a crash depends on them. This runs before the game's
    // first allocation, so leaving the heap alone is always safe.
    st[S_HEAP] = fix_off(L"memory_allocator", L"heapfix_off.txt", "memory allocator", "the game keeps its own heap") ? FIX_SWITCHED_OFF : install_into(m) > 0;
    st[S_POOL] = fix_off(L"small_object_pools", L"pool_off.txt", "pool free lookup", "not replaced") ? FIX_SWITCHED_OFF : install_pool(m, wlog) > 0;
    int sorts = 0, camp = 0, x87 = 0;
    if (fix_off(L"render_list_sorts", L"sortfix_off.txt", "render-list sorts", "not replaced")) st[S_SORTS] = FIX_SWITCHED_OFF;
    else sorts = install_sortfix(m, wlog);
    st[S_BATCH] = fix_off(L"battle_draw_batching", L"batchfix_off.txt", "batch fix", "not installed") ? FIX_SWITCHED_OFF : install_batchfix(m, wlog) > 0;
    if (fix_off(L"campaign_rounding", L"campfix_off.txt", "campaign rounding fixes", "not installed")) camp = -1;
    else g_campfix_on = (camp = install_campfix(m, 0, wlog)) > 0;
    if (fix_off(L"x87_rounding", L"x87fix_off.txt", "x87 rounding fix", "not installed")) st[S_X87] = FIX_SWITCHED_OFF;
    else g_x87fix_on = (x87 = install_x87fix(m, wlog)) > 0;
    if (g_campfix_on || g_x87fix_on) g_frame_check = frame_checks;
    // Terrain height cache: verified in game: 7.5 M queries over map, end turn, save load and battle, 0 mismatches.
    st[S_HEIGHT] = fix_off(L"terrain_height_cache", L"heightcache_off.txt", "height cache", "not installed") ? FIX_SWITCHED_OFF
                                                                                                    : install_heightcache(m, 2, wlog) > 0;
    st[S_COPY] = fix_off(L"small_memory_copies", L"memfast_off.txt", "memcpy fast path", "not installed") ? FIX_SWITCHED_OFF : install_memfast(m, wlog) > 0;
    // Battle render loops prefetch the next items' data (loopfetch.c).
    if (fix_off(L"render_loop_prefetch", L"loopfetch_off.txt", "render loop prefetch", "not installed")) st[S_PREFETCH] = FIX_SWITCHED_OFF;
    else {
        int n = install_loopfetch(m, wlog);
        st[S_PREFETCH] = n >= 6 ? FIX_ON : n ? FIX_PARTLY : FIX_INACTIVE;
    }
    // Loading: packs through the file cache, buffered small reads of the save (fileio.c), then the missing-folder
    // cache for loose-file checks and opens (attrcache.c), whose open hook calls fileio's.
    st[S_FILES] = fix_off(L"file_reading", L"fileio_off.txt", "file I/O fix", "not installed") ? FIX_SWITCHED_OFF : install_fileio(m, wlog) > 0;
    if (fix_off(L"missing_file_cache", L"attrcache_off.txt", "missing-file cache", "not installed")) st[S_MISSING] = FIX_SWITCHED_OFF;
    else {
        int verify = 0;
        st[S_MISSING] = install_attrcache(m, verify, wlog) > 0;
    }
    // Battle loading: the DXT1 texture compressor with SSE2, same output (texfix.c).
    if (fix_off(L"battle_textures", L"texfix_off.txt", "texture compression", "not replaced")) st[S_TEXTURES] = FIX_SWITCHED_OFF;
    else {
        int verify = 0;
        st[S_TEXTURES] = install_texfix(m, verify, wlog) > 0;
    }
    // Menus: closed panels' textures stay loaded, so reopening does not load them again (uikeep.c).
    int keep = ini_int(L"tuning", L"menu_textures_kept", 1024);
    g_uikeep_keep = keep < 1 ? 1 : keep > 1024 ? 1024 : (unsigned)keep;
    st[S_MENUS] = fix_off(L"menu_textures", L"uikeep_off.txt", "menu textures fix", "freed when closed, as the game does") ? FIX_SWITCHED_OFF
                                                                                                                     : install_uikeep(m, wlog) > 0;
    if (st[S_SORTS] != FIX_SWITCHED_OFF) st[S_SORTS] = sorts == 2 ? FIX_ON : sorts ? FIX_PARTLY : FIX_INACTIVE;
    if (st[S_X87] != FIX_SWITCHED_OFF)  // the 4 campaign blocks (campfix) and the general x87 fix together
        st[S_X87] = x87 > 0 && camp == 4 ? FIX_ON : x87 > 0 || camp > 0 ? FIX_PARTLY : FIX_INACTIVE;
    wchar_t dir[MAX_PATH];
    wcscpy(dir, g_log_path);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = 0;
        install_fps(m, dir, wlog);  // the frame hook also runs the per-frame checks
    }
    // the DLSS add-on: <game folder>\FrameBridge\ with the 64-bit helper and NVIDIA's DLLs; without it nothing is installed
    if (sl) wcscpy(sl + 1, L"FrameBridge\\helper64.exe");
    int addon = sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES;
    if (addon) {   // addon=off in its settings.txt: the add-on does nothing at all (no hooks), as if it were not installed
        char text[4096] = {0};   // settings.txt is past 1 KB since dlss_preset / sharpness: addon=off sat beyond 1024 bytes
        wcscpy(sl + 1, L"FrameBridge\\settings.txt");
        FILE *sf = _wfopen(off, L"r");
        if (sf) { fread(text, 1, sizeof text - 1, sf); fclose(sf); }
        if (strstr(text, "\naddon=off") || !strncmp(text, "addon=off", 9)) {
            wlog("DLSS add-on found but switched off (addon=off in FrameBridge\\settings.txt) - the game runs with the performance fix only");
            addon = 0;
        }
    }
    if (addon) {
        g_pt_force_bridge = 1;
        g_passtime_on = install_passtime(m, wlog) > 0;
        if (g_passtime_on) g_frame_check = frame_checks;
    }
    log_status(st);  // last, so the summary closes the startup lines
}

// Something else in the process (an overlay, another hook DLL) can load dinput8.dll before the game loads
// empire.retail.dll; Windows then never runs this DllMain again. So wait for the game DLL and install when it is
// mapped, which is still before its own startup code runs (the same state as the normal path).
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } NOTIF_STR;
typedef struct { ULONG Flags; const NOTIF_STR *FullDllName, *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } NOTIF_DATA;
static PVOID g_notif_cookie;

static VOID CALLBACK on_dll_loaded(ULONG reason, const NOTIF_DATA *d, PVOID ctx) {
    (void)ctx;
    static const wchar_t want[] = L"empire.retail.dll";
    if (reason != 1 || !d || !d->BaseDllName || d->BaseDllName->Length != sizeof want - sizeof(wchar_t)) return;
    if (_wcsnicmp(d->BaseDllName->Buffer, want, d->BaseDllName->Length / sizeof(wchar_t)) != 0) return;
    wlog("empire.retail.dll is loading now - installing the fixes");
    install_all((HMODULE)d->DllBase);
}

static int wait_for_game(void) {
    typedef LONG(NTAPI * reg_fn)(ULONG, VOID(CALLBACK *)(ULONG, const NOTIF_DATA *, PVOID), PVOID, PVOID *);
    reg_fn reg = (reg_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    return reg && reg(0, on_dll_loaded, NULL, &g_notif_cookie) == 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_THREAD_DETACH) pool_thread_exit();   // the thread's cached blocks go back to their pools
    if (reason == DLL_PROCESS_ATTACH) {
        set_log_path(h);  // thread notifications stay on: the pools take a thread's cached blocks back when it ends
        if (g_log_path[0]) log_header();
        g_self = h;
        HMODULE m = GetModuleHandleW(L"empire.retail.dll");
        if (m)
            install_all(m);
        else if (wait_for_game())
            wlog("dinput8.dll was loaded before empire.retail.dll (by another program or DLL) - the fixes install "
                 "when the game loads it");
        else
            diag_startup(NULL, g_log_path, wlog);
    }
    return TRUE;
}
#endif
