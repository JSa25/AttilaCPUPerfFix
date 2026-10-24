// dinput8.dll proxy for Total War: ATTILA 1.6.0 (empire.retail.dll of 2026-09-19).
// The game routes every allocation over 80 bytes through one global dlmalloc heap guarded by a
// single spinlock, so its worker threads and the main thread spend most of their time waiting on
// it. This swaps dlmalloc's malloc/free/realloc for a hybrid heap that does not serialize threads:
// blocks up to 16 KB (all but ~0.02% of the calls, measured) go to mimalloc, larger ones to a private Windows
// low-fragmentation heap. Large blocks stay on the Windows heap because the game needs big
// contiguous blocks (e.g. ~220 MB when returning to the main menu) and mimalloc's 128 MB arena
// reservations fragment the 32-bit address space too much (bad_alloc crashes at 4K).
// Built with HEAPFIX_WINHEAP everything goes to the Windows heap.
// empire.retail.dll imports DINPUT8.dll, so this loads before the game's own startup code runs,
// i.e. before the first allocation.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define DL_MAX_REQUEST 0xFFFFFFC0u   // dlmalloc fails requests this big; keep that behaviour
#define PREFERRED_BASE 0x10000000u
#define RVA_MPARAMS_MAGIC 0x01F20BBCu // dlmalloc mparams.magic: non-zero once dlmalloc was used

static HANDLE g_heap;
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

static void *h_alloc(size_t n) { return n <= SMALL_MAX ? mi_malloc(n) : HeapAlloc(g_heap, 0, n); }

static void h_free(void *p) {
    if (mi_is_in_heap_region(p)) mi_free(p);
    else HeapFree(g_heap, 0, p);
}

static void *h_realloc(void *p, size_t n) {
    if (!mi_is_in_heap_region(p)) return HeapReAlloc(g_heap, 0, p, n);  // large blocks stay put, even when shrinking
    if (n <= SMALL_MAX) return mi_realloc(p, n);
    void *q = HeapAlloc(g_heap, 0, n);  // grew past the small limit: move to the Windows heap
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

// The game periodically trims dlmalloc's heap, which is now unused.
static int __cdecl hf_trim(void) { return 0; }

struct patch {
    const char *name;
    DWORD rva;
    unsigned char expect[12];
    int reloc;  // offset of an absolute address inside expect, or -1
    void *to;
};

static struct patch g_patches[] = {
    {"dlmalloc",  0x112D80, {0x83,0xEC,0x08,0x83,0x3D,0xBC,0x0B,0xF2,0x11,0x00,0x75,0x05}, 5,  (void *)hf_malloc},
    {"dlfree",    0x112730, {0x83,0xEC,0x1C,0x56,0x8B,0x74,0x24,0x24,0x85,0xF6,0x0F,0x84}, -1, (void *)hf_free},
    {"dlrealloc", 0x113040, {0x53,0x8B,0x5C,0x24,0x08,0x85,0xDB,0x75,0x0E,0xFF,0x74,0x24}, -1, (void *)hf_realloc},
    {"dltrim",    0x1126E0, {0x83,0x3D,0xBC,0x0B,0xF2,0x11,0x00,0x75,0x05,0xE8,0x32,0xE8}, 2,  (void *)hf_trim},
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
    if (!g_heap) {
        g_heap = HeapCreate(0, 0, 0);
        if (!g_heap) {
            wlog("HeapCreate failed (%lu) - nothing patched", GetLastError());
            return 0;
        }
        ULONG lfh = 2;
        if (!HeapSetInformation(g_heap, HeapCompatibilityInformation, &lfh, sizeof lfh))
            wlog("note: could not force the low-fragmentation heap (%lu), using the default", GetLastError());
    }
    // Smaller arena reservations (default 128 MB on 32-bit) keep mimalloc's address space compact.
    mi_option_set(mi_option_arena_reserve, 32 * 1024);  // KiB
#define HEAP_NAME "mimalloc " MI_STR(MI_MALLOC_VERSION) " (blocks <= 16 KB) + Windows heap (larger)"
#define MI_STR(x) MI_STR2(x)
#define MI_STR2(x) #x
    for (size_t i = 0; i < NPATCH; i++) {
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
    wlog("empire.retail.dll at %p: game heap replaced by " HEAP_NAME, base);
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

HRESULT WINAPI stub_DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID *o, LPUNKNOWN u) {
    typedef HRESULT(WINAPI * fn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    fn f = (fn)real_proc("DirectInput8Create");
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
void diag_startup(HMODULE empire, const wchar_t *log_path, void (*log)(const char *fmt, ...));  // diag.c
void update_check(HMODULE self, void (*log)(const char *fmt, ...));                             // update.c
extern void (*g_frame_check)(void);                                  // fps.c

static int g_campfix_on, g_x87fix_on;

// Per-frame checks on the main thread (called from the frame hook in fps.c).
static void frame_checks(void) {
    if (g_campfix_on) campfix_frame();
    if (g_x87fix_on) x87fix_frame();
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

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        set_log_path(h);  // thread notifications stay on: mimalloc cleans up per-thread state
        HMODULE m = GetModuleHandleW(L"empire.retail.dll");
        diag_startup(m, g_log_path, wlog);  // version, process, game build, system, settings; crash records
        if (m) {
            update_check(h, wlog);  // a newer Workshop copy replaces this DLL for the next launch
            install_into(m);
            install_pool(m, wlog);
            // sortfix_off.txt in the game folder skips the draw-list sort fix (for A/B FPS tests).
            wchar_t off[MAX_PATH];
            wcscpy(off, g_log_path);
            wchar_t *sl = wcsrchr(off, L'\\');
            if (sl) wcscpy(sl + 1, L"sortfix_off.txt");
            if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES)
                wlog("sortfix_off.txt found - draw-list sort fix not installed");
            else
                install_sortfix(m, wlog);
            // batchfix_off.txt in the game folder skips the battle batch fix (for A/B tests).
            if (sl) wcscpy(sl + 1, L"batchfix_off.txt");
            if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES)
                wlog("batchfix_off.txt found - batch fix not installed");
            else
                install_batchfix(m, wlog);
            // campfix_off.txt in the game folder skips the campaign x87 rounding fixes (for A/B tests).
            if (sl) wcscpy(sl + 1, L"campfix_off.txt");
            if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES)
                wlog("campfix_off.txt found - campaign rounding fixes not installed");
            else {
                g_campfix_on = install_campfix(m, 0, wlog) > 0;
            }
            // x87fix_off.txt skips the remaining x87 rounding-mode blocks (all of empire.retail.dll).
            if (sl) wcscpy(sl + 1, L"x87fix_off.txt");
            if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES)
                wlog("x87fix_off.txt found - x87 rounding fix not installed");
            else
                g_x87fix_on = install_x87fix(m, wlog) > 0;
            if (g_campfix_on || g_x87fix_on) g_frame_check = frame_checks;
            // Terrain height cache: counters builds verify it; normal builds cache (verified in game: 7.5 M
            // queries over map, end turn, save load and battle, 0 mismatches). heightcache_off.txt disables.
            if (sl) wcscpy(sl + 1, L"heightcache_off.txt");
            if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES)
                wlog("heightcache_off.txt found - height cache not installed");
            else
                install_heightcache(m, 2, wlog);
            // memcpy small-copy fast path (the counters build counts sizes on the same function instead).
            if (sl) wcscpy(sl + 1, L"memfast_off.txt");
            if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES)
                wlog("memfast_off.txt found - memcpy fast path not installed");
            else
                install_memfast(m, wlog);
            // Battle render loops prefetch the next items' data (loopfetch.c). loopfetch_off.txt disables.
            if (sl) wcscpy(sl + 1, L"loopfetch_off.txt");
            if (sl && GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES)
                wlog("loopfetch_off.txt found - render loop prefetch not installed");
            else
                install_loopfetch(m, wlog);
            wchar_t dir[MAX_PATH];
            wcscpy(dir, g_log_path);
            wchar_t *slash = wcsrchr(dir, L'\\');
            if (slash) {
                *slash = 0;
                install_fps(m, dir, wlog);  // the frame hook also runs the per-frame checks
            }
        }
    }
    return TRUE;
}
#endif
