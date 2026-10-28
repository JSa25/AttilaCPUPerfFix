// dinput8.dll proxy for Total War: ATTILA 1.6.0 (empire.retail.dll of 2026-09-19).
// The game routes every allocation over 80 bytes through one global dlmalloc heap guarded by a
// single spinlock, so its worker threads and the main thread spend most of their time waiting on
// it. This sends blocks up to 16 KB (all but ~0.02% of the calls, measured) to mimalloc, which does not serialize
// threads, and keeps larger ones on the game's own dlmalloc, called through copies of its first instructions.
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

// mimalloc for small blocks: per-thread free lists, no lock on the common path; much faster than
// the Windows heap for the millions of small allocations per second the game makes.
#include <mimalloc.h>
#define SMALL_MAX 16384

// The game's dlmalloc (set by install_into): trampolines that run its first instructions, then the rest of it.
static void *(__cdecl *dl_malloc)(size_t);
static void(__cdecl *dl_free)(void *);
static void *(__cdecl *dl_realloc)(void *, size_t);

#ifdef HEAPFIX_MEMSTATS
// Dev builds: bytes live on dlmalloc (from its chunk headers) and a log line every 20 s, to see where address space goes.
#include <mimalloc-stats.h>
static volatile LONG g_dl_live, g_dl_calls, g_spill_calls;
static volatile LONG g_small_mb;  // defined below
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
        mi_stats_t st;
        mi_stats_init(&st);
        mi_stats_get(&st);
        size_t fr, big;
        address_space(&fr, &big);
        LONG calls = InterlockedExchange(&g_dl_calls, 0), spills = InterlockedExchange(&g_spill_calls, 0);
        wlog("memstats: mimalloc %ld MB reserved, committed %u MB, live small %u MB; dlmalloc live %u MB, %ld calls/s "
             "(%ld small spills/s); address space free %u MB, largest %u MB", g_small_mb,
             (unsigned)(st.committed.current >> 20), (unsigned)(st.malloc_normal.current >> 20),
             (unsigned)(g_dl_live >> 20), calls / 20, spills / 20, (unsigned)(fr >> 20), (unsigned)(big >> 20));
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

// mimalloc gets its own reservations (install_into): 512 MB, grown in 32 MB steps up to 768 MB when full. Past that,
// small blocks go to dlmalloc as well. A full mimalloc searches its whole reservation before failing (~400x slower
// than a normal allocation, measured), so after a failure it is skipped for 50 ms; frees keep returning space to it.
#define SMALL_HEAP_MB 512
#define SMALL_STEP_MB 32   // the smallest piece mimalloc can use on 32-bit (page info every 32 MiB)
#define SMALL_LIMIT_MB 768
static volatile LONG g_small_mb;  // reserved for mimalloc so far; 0 = no cap (the first reservation failed)
static volatile DWORD g_mi_full_until;
static SRWLOCK g_grow_lock = SRWLOCK_INIT;
static int reserve_small_heap(size_t size);
static int grow_small_heap_step(void);
static void wlog(const char *fmt, ...);

// Called after mimalloc returned NULL: 1 if it has more room now (this thread or another one added a step).
static int grow_small_heap(LONG seen) {
    if (seen == 0 || seen >= SMALL_LIMIT_MB) return 0;
    AcquireSRWLockExclusive(&g_grow_lock);
    int ok = g_small_mb != seen;
    if (!ok && g_small_mb < SMALL_LIMIT_MB) {
        ok = grow_small_heap_step();
        g_small_mb = ok ? g_small_mb + SMALL_STEP_MB : SMALL_LIMIT_MB;  // no room for a step: stop trying
        wlog(ok ? "memory: the mod's small-block heap grew to %ld MB (normal with big mods; the limit is 768 MB)"
                : "memory: the mod's small-block heap is full at %ld MB and found no room to grow - small blocks also "
                  "use the game's own heap from now on (works, a little slower)", g_small_mb);
    }
    ReleaseSRWLockExclusive(&g_grow_lock);
    return ok;
}

// The clock is only read while a skip window is set (g_mi_full_until != 0), not on every allocation.
static int mi_skipped(void) {
    DWORD until = g_mi_full_until;
    if (!until) return 0;
    if ((LONG)(GetTickCount() - until) < 0) return 1;
    g_mi_full_until = 0;  // window over: try mimalloc again
    return 0;
}

static void mi_full(void) { g_mi_full_until = (GetTickCount() + 50) | 1; }  // | 1: never 0

static void *h_alloc(size_t n) {
    if (n <= SMALL_MAX && !mi_skipped()) {
        LONG seen = g_small_mb;
        void *p = mi_malloc(n);
        if (p || (grow_small_heap(seen) && (p = mi_malloc(n)))) return p;
        mi_full();
    }
    return DL_MALLOC(n);
}

static void h_free(void *p) {
    if (mi_is_in_heap_region(p)) mi_free(p);
    else DL_FREE(p);
}

static void *h_realloc(void *p, size_t n) {
    if (!mi_is_in_heap_region(p)) return DL_REALLOC(p, n);  // large blocks stay with dlmalloc, even when shrinking
    if (n <= SMALL_MAX && !mi_skipped()) {
        LONG seen = g_small_mb;
        void *q = mi_realloc(p, n);
        if (q || (grow_small_heap(seen) && (q = mi_realloc(p, n)))) return q;
        mi_full();  // mimalloc is full: p is still valid, move it to dlmalloc
    }
    void *q = DL_MALLOC(n);  // grew past the small limit (or mimalloc is full): move to dlmalloc
    if (!q) return NULL;                // like dlrealloc: the old block stays valid
    size_t old = mi_usable_size(p);
    memcpy(q, p, old < n ? old : n);
    mi_free(p);
    return q;
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
    void *to;       // NULL: only checked (dlmalloc's trim keeps running as is)
    unsigned copy;  // whole instructions moved into the trampoline (nothing jumps into them; checked in the disassembly)
    void **orig;    // gets the trampoline: the original function
};

static struct patch g_patches[] = {
    {"dlmalloc",  0x112D80, {0x83,0xEC,0x08,0x83,0x3D,0xBC,0x0B,0xF2,0x11,0x00,0x75,0x05}, 5,  (void *)hf_malloc, 10, (void **)&dl_malloc},
    {"dlfree",    0x112730, {0x83,0xEC,0x1C,0x56,0x8B,0x74,0x24,0x24,0x85,0xF6,0x0F,0x84}, -1, (void *)hf_free, 8, (void **)&dl_free},
    {"dlrealloc", 0x113040, {0x53,0x8B,0x5C,0x24,0x08,0x85,0xDB,0x75,0x0E,0xFF,0x74,0x24}, -1, (void *)hf_realloc, 5, (void **)&dl_realloc},
    {"dltrim",    0x1126E0, {0x83,0x3D,0xBC,0x0B,0xF2,0x11,0x00,0x75,0x05,0xE8,0x32,0xE8}, 2,  NULL, 0, NULL},
};
#define NPATCH (sizeof g_patches / sizeof g_patches[0])

// mimalloc's one reservation, 32 MiB aligned as mimalloc wants on 32-bit. Taken bottom-up, while the address space is
// still empty: placed at the top instead, the largest free block left in a 4K Mk1212 battle fell from 377 to 142 MB,
// too small for the ~220 MB block the game needs on "exit to main menu".
static int reserve_small_heap(size_t size) {
    const size_t align = 32u << 20;
    unsigned char *p = VirtualAlloc(NULL, size + align, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) return 0;
    unsigned char *a = (unsigned char *)(((UINT_PTR)p + align - 1) & ~(UINT_PTR)(align - 1));
    VirtualFree(p, 0, MEM_RELEASE);
    if (VirtualAlloc(a, size, MEM_RESERVE, PAGE_NOACCESS) != a) return 0;
    if (mi_manage_os_memory(a, size, false, false, true, -1)) return 1;
    VirtualFree(a, 0, MEM_RELEASE);
    return 0;
}

// A growth step comes late in a session, when the address space is already in pieces. A plain reservation takes the
// lowest gap with room, i.e. it cuts into the big free gaps the game needs for its large blocks (a player's MK1212
// campaign: grown to 768 MB in 128 MB steps, then a 36 MB allocation failed with 488 MB free in pieces of <= 32 MB).
// So each 32 MB step goes into the smallest free gap that holds an aligned 32 MB piece, at the end of that gap that
// leaves the larger part free.
static unsigned char *best_fit_piece(size_t size) {
    const UINT_PTR align = 32u << 20;
    MEMORY_BASIC_INFORMATION m;
    unsigned char *best = NULL;
    size_t best_gap = ~(size_t)0, best_left = 0;
    for (unsigned char *a = (unsigned char *)0x10000; VirtualQuery(a, &m, sizeof m) == sizeof m; a = (unsigned char *)m.BaseAddress + m.RegionSize) {
        if (m.State == MEM_FREE) {
            UINT_PTR lo = (UINT_PTR)m.BaseAddress, hi = lo + m.RegionSize;
            UINT_PTR first = (lo + align - 1) & ~(align - 1), last = (hi - size) & ~(align - 1);
            if (hi >= size && first >= lo && last >= first && last + size <= hi) {
                // the larger free part left over: below the first piece or above the last one
                size_t keep_first = hi - (first + size) > first - lo ? hi - (first + size) : first - lo;
                size_t keep_last = last - lo > hi - (last + size) ? last - lo : hi - (last + size);
                UINT_PTR at = keep_last > keep_first ? last : first;
                size_t left = keep_last > keep_first ? keep_last : keep_first;
                if (m.RegionSize < best_gap || (m.RegionSize == best_gap && left > best_left)) {
                    best = (unsigned char *)at;
                    best_gap = m.RegionSize;
                    best_left = left;
                }
            }
        }
        if ((unsigned char *)m.BaseAddress + m.RegionSize < a) break;  // wrapped past the top
    }
    return best;
}

// One growth step for mimalloc: 1 when it got another 32 MB.
static int grow_small_heap_step(void) {
    const size_t size = (size_t)SMALL_STEP_MB << 20;
    for (int tries = 0; tries < 4; tries++) {  // another thread may take the gap between the search and the reservation
        unsigned char *p = best_fit_piece(size);
        if (!p) return 0;
        if (VirtualAlloc(p, size, MEM_RESERVE, PAGE_NOACCESS) != p) continue;
        if (mi_manage_os_memory(p, size, false, false, true, -1)) return 1;
        VirtualFree(p, 0, MEM_RELEASE);
        return 0;
    }
    return 0;
}

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
    // mimalloc only keeps space for small blocks, and after 8 arenas it doubles each new reservation (built for
    // 64-bit): with heavy mods it held 1,088 MB for 557 MB of blocks and the game ran out of address space. So it
    // gets its own reservations instead, the first made now while the address space is empty: 512 MB keeps the big
    // free block the game needs (~220 MB on "exit to main menu"); heavy mods grow it in 32 MB steps (best fit, see above) up to 768 MB
    // (Mk1212 at 4K peaked at ~600 MB committed), and only past that do small blocks spill to dlmalloc, which is
    // ~100x slower under contention (measured). A fixed 768 MB left a 142 MB largest free block in that battle.
    mi_option_set(mi_option_retry_on_oom, 0);  // a full cap must fail fast, not wait 400 ms
    mi_option_set(mi_option_max_errors, 0);
    mi_option_set(mi_option_max_warnings, 0);
    unsigned cap_mb = SMALL_HEAP_MB;
    while (cap_mb >= 256 && !reserve_small_heap((size_t)cap_mb << 20)) cap_mb -= 128;
    if (cap_mb >= 256) {
        g_small_mb = (LONG)cap_mb;
        mi_option_enable(mi_option_disallow_os_alloc);
    } else {
        cap_mb = 0;
        mi_option_set(mi_option_arena_reserve, 32 * 1024);  // KiB: no cap, but small arenas at least
    }
#define HEAP_NAME "mimalloc " MI_STR(MI_MALLOC_VERSION) " (blocks <= 16 KB, %u MB reserved, grows to 768 MB in 32 MB steps if needed; 0 = no cap); larger blocks stay on the game's heap"
#define MI_STR(x) MI_STR2(x)
#define MI_STR2(x) #x
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
    wlog("empire.retail.dll at %p: game heap replaced by " HEAP_NAME, base, cap_mb);
#ifdef HEAPFIX_MEMSTATS
    CloseHandle(CreateThread(NULL, 0, memstats, NULL, 0, NULL));
#endif
    return (int)NPATCH - 1;
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
    if (g_passtime_on) {   // the frame bridge's frame counter and its 30 s report in heapfix.log
        g_geo_frame++;
        if (g_gt_frame) g_gt_frame();
        static DWORD last;
        DWORD now = GetTickCount();
        if (!last) last = now;
        if (now - last >= 30000) { passtime_report(wlog); last = now; }
    }
}

static void set_log_path(HINSTANCE h) {
    DWORD n = GetModuleFileNameW(h, g_log_path, MAX_PATH);
    wchar_t *slash = wcsrchr(g_log_path, L'\\');
    if (n == 0 || n >= MAX_PATH || !slash || slash - g_log_path > MAX_PATH - 20) {
        g_log_path[0] = 0;
        return;
    }
    wcscpy(slash + 1, L"heapfix.log");
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
             st[i] == FIX_SWITCHED_OFF ? " (switched off by its _off.txt file in the game folder)"
             : st[i] == FIX_PARTLY ? " (only part of it fits this game version)"
             : st[i] == FIX_INACTIVE ? " (does not fit this game version, so it stays off; details above)" : "");
}

// Everything that patches the game, run once empire.retail.dll is in memory and before its own startup code.
static void install_all(HMODULE m) {
    static int done;
    if (done) return;
    done = 1;
    int st[S_N] = {0};
    diag_startup(m, g_log_path, wlog);  // version, process, game build, system, settings; crash records
    update_check(g_self, wlog);  // a newer Workshop copy replaces this DLL for the next launch
    wchar_t off[MAX_PATH];
    wcscpy(off, g_log_path);
    wchar_t *sl = wcsrchr(off, L'\\');
    // heapfix_off.txt / pool_off.txt: the game keeps its own heap / pool lookup (to tell whether a crash depends on
    // them). This runs before the game's first allocation, so leaving the heap alone is always safe.
    if (sl) wcscpy(sl + 1, L"heapfix_off.txt");
    if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        wlog("heapfix_off.txt found - the game keeps its own heap");
        st[S_HEAP] = FIX_SWITCHED_OFF;
    } else
        st[S_HEAP] = install_into(m) > 0;
    if (sl) wcscpy(sl + 1, L"pool_off.txt");
    if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        wlog("pool_off.txt found - pool free lookup not replaced");
        st[S_POOL] = FIX_SWITCHED_OFF;
    } else
        st[S_POOL] = install_pool(m, wlog) > 0;
    int sorts = 0, camp = 0, x87 = 0;
    // sortfix_off.txt in the game folder skips the draw-list sort fix (for A/B FPS tests).
    if (sl) wcscpy(sl + 1, L"sortfix_off.txt");
    if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        wlog("sortfix_off.txt found - draw-list sort fix not installed");
        st[S_SORTS] = FIX_SWITCHED_OFF;
    } else
        sorts = install_sortfix(m, wlog);
    // batchfix_off.txt in the game folder skips the battle batch fix (for A/B tests).
    if (sl) wcscpy(sl + 1, L"batchfix_off.txt");
    if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        wlog("batchfix_off.txt found - batch fix not installed");
        st[S_BATCH] = FIX_SWITCHED_OFF;
    } else
        st[S_BATCH] = install_batchfix(m, wlog) > 0;
    // campfix_off.txt in the game folder skips the campaign x87 rounding fixes (for A/B tests).
    if (sl) wcscpy(sl + 1, L"campfix_off.txt");
    if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        wlog("campfix_off.txt found - campaign rounding fixes not installed");
        camp = -1;
    } else {
        camp = install_campfix(m, 0, wlog);
        g_campfix_on = camp > 0;
    }
    // x87fix_off.txt skips the remaining x87 rounding-mode blocks (all of empire.retail.dll).
    if (sl) wcscpy(sl + 1, L"x87fix_off.txt");
    if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        wlog("x87fix_off.txt found - x87 rounding fix not installed");
        st[S_X87] = FIX_SWITCHED_OFF;
    } else {
        x87 = install_x87fix(m, wlog);
        g_x87fix_on = x87 > 0;
    }
    if (g_campfix_on || g_x87fix_on) g_frame_check = frame_checks;
    // Terrain height cache: counters builds verify it; normal builds cache (verified in game: 7.5 M
    // queries over map, end turn, save load and battle, 0 mismatches). heightcache_off.txt disables.
    if (sl) wcscpy(sl + 1, L"heightcache_off.txt");
    if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        wlog("heightcache_off.txt found - height cache not installed");
        st[S_HEIGHT] = FIX_SWITCHED_OFF;
    } else
        st[S_HEIGHT] = install_heightcache(m, 2, wlog) > 0;
    // memcpy small-copy fast path (the counters build counts sizes on the same function instead).
    if (sl) wcscpy(sl + 1, L"memfast_off.txt");
    if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        wlog("memfast_off.txt found - memcpy fast path not installed");
        st[S_COPY] = FIX_SWITCHED_OFF;
    } else
        st[S_COPY] = install_memfast(m, wlog) > 0;
    // Battle render loops prefetch the next items' data (loopfetch.c). loopfetch_off.txt disables.
    if (sl) wcscpy(sl + 1, L"loopfetch_off.txt");
    if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        wlog("loopfetch_off.txt found - render loop prefetch not installed");
        st[S_PREFETCH] = FIX_SWITCHED_OFF;
    } else {
        int n = install_loopfetch(m, wlog);
        st[S_PREFETCH] = n >= 6 ? FIX_ON : n ? FIX_PARTLY : FIX_INACTIVE;
    }
    // Loading: packs through the file cache, buffered small reads of the save (fileio.c), then the missing-folder
    // cache for loose-file checks and opens (attrcache.c), whose open hook calls fileio's. Off switches for each.
    if (sl) wcscpy(sl + 1, L"fileio_off.txt");
    if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        wlog("fileio_off.txt found - file I/O fix not installed");
        st[S_FILES] = FIX_SWITCHED_OFF;
    } else
        st[S_FILES] = install_fileio(m, wlog) > 0;
    if (sl) wcscpy(sl + 1, L"attrcache_off.txt");
    if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        wlog("attrcache_off.txt found - attribute cache not installed");
        st[S_MISSING] = FIX_SWITCHED_OFF;
    } else {
        int verify = 0;
        st[S_MISSING] = install_attrcache(m, verify, wlog) > 0;
    }
    // Battle loading: the DXT1 texture compressor with SSE2, same output (texfix.c).
    if (sl) wcscpy(sl + 1, L"texfix_off.txt");
    if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        wlog("texfix_off.txt found - texture compression not replaced");
        st[S_TEXTURES] = FIX_SWITCHED_OFF;
    } else {
        int verify = 0;
        st[S_TEXTURES] = install_texfix(m, verify, wlog) > 0;
    }
    // Menus: closed panels' textures stay loaded, so reopening does not load them again (uikeep.c).
    if (sl) wcscpy(sl + 1, L"uikeep_off.txt");
    if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        wlog("uikeep_off.txt found - menu textures freed when closed, as the game does");
        st[S_MENUS] = FIX_SWITCHED_OFF;
    } else
        st[S_MENUS] = install_uikeep(m, wlog) > 0;
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
        char text[1024] = {0};
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
    if (reason == DLL_PROCESS_ATTACH) {
        set_log_path(h);  // thread notifications stay on: mimalloc cleans up per-thread state
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
