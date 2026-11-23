// attila64_perffix as a proxy SDL2.dll for Ivan's Attila64Host: every SDL export forwards to SDL2_real.dll (the official
// SDL2 2.32.10, renamed); only SDL_Init is ours. The host resolves SDL_Init on its first call, which the engine makes
// from its main thread after the host mapped, relocated and hooked the ELF. That is when the engine fixes go in.
// Engine: Linux x64 Attila, SHA-256 217e1435...8784, fixed-address ELF (ET_EXEC, text at 0x400000).
#define COBJMACROS
#include <d3d11.h>   // graphics_not_dx11: the loader's own DirectX 11 test
#include "perffix64.h"
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "version64.h"   // the plugin's own version (1.0 on), not the 32-bit mod's
static wchar_t g_dir[MAX_PATH];
static HMODULE g_self;
static unsigned char *g_mem_low, *g_mem_tramp;   // the two pages of stubs and trampolines (install_all)

static void (*g_host_log)(const char *fmt, ...);   // Sol's loader: its own log (attila64_log.txt)
// The plugin's own log, <attila64>\perffix64.log, named as the DLL (the 32-bit engine fix's is <game>\heapfix.log: the user, 2026-11-22, the
// 64-bit plugin's files apart from it). The last start's is kept as perffix64_previous.log. Every line also goes to the
// loader's log (attila64_log.txt) once it has handed its log function over.
static wchar_t g_logpath[MAX_PATH];
static SRWLOCK g_loglock = SRWLOCK_INIT;
static void plog(const char *fmt, ...) {
    char line[512];
    va_list a; va_start(a, fmt); vsnprintf(line, sizeof line, fmt, a); va_end(a);
    if (g_host_log) g_host_log("perffix64: %s", line);
    if (!g_logpath[0]) return;
    AcquireSRWLockExclusive(&g_loglock);
    FILE *f = _wfopen(g_logpath, L"a");
    if (f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&g_loglock);
}

uint32_t fnv1a(const void *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ ((const unsigned char *)p)[i]) * 16777619u;
    return h;
}

int patch_code(void *at, const void *bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    return 1;
}

// Every other thread is paused while the fixes go in, so none runs inside code being rewritten.
#define MAX_THREADS 4096
static HANDLE g_threads[MAX_THREADS];
static uint64_t g_rips[MAX_THREADS];
static int g_nthreads;

int thread_inside(uint64_t start, uint64_t len) {
    for (int i = 0; i < g_nthreads; ++i) if (g_rips[i] - start < len) return 1;
    return 0;
}

static void pause_others(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; memset(&te, 0, sizeof te); te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || g_nthreads == MAX_THREADS) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (SuspendThread(h) == (DWORD)-1) { CloseHandle(h); continue; }
        CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
        g_rips[g_nthreads] = GetThreadContext(h, &c) ? c.Rip : 0;
        g_threads[g_nthreads++] = h;
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}

static void resume_others(void) {
    for (int i = 0; i < g_nthreads; ++i) { ResumeThread(g_threads[i]); CloseHandle(g_threads[i]); }
}

// Settings: heapfix.ini beside attila64.exe (the plugin's folder is attila64\plugins), the 32-bit mod's format.
// Written once with the defaults when missing, never changed. 1 / on / true / yes, 0 / off / false / no.
static wchar_t g_ini[MAX_PATH + 32];
static const char INI_DEFAULT[] =
    "; Attila Engine Performance Fix (64-bit plugin) settings. 1 = on, 0 = off. Restart the game after a change.\r\n"
    "; The mod writes this file once with the defaults and never changes it. Delete it to get the defaults back.\r\n"
    "\r\n"
    "[fixes]\r\n"
    "; FPS\r\n"
    "x87_rounding=1\r\n"
    "small_object_pools=1\r\n"
    "render_list_sorts=1\r\n"
    "battle_draw_batching=1\r\n"
    "terrain_height_cache=1\r\n"
    "render_loop_prefetch=1\r\n"
    "; the game's strlen and memcmp served by Windows' fast ones instead of the loader's byte loops\r\n"
    "fast_string_functions=1\r\n"
    "; the game's memory on the mimalloc allocator instead of the Windows heap (0 = the Windows heap)\r\n"
    "memory_allocator=1\r\n"
    "; loading\r\n"
    "battle_textures=1\r\n"
    "; menus\r\n"
    "menu_textures=1\r\n"
    "; battle: the unit cards shown from a cache, redrawn about 15 times a second and on input\r\n"
    "battle_ui_cache=1\r\n"
    "; crashes\r\n"
    "sound_crash_guard=1\r\n"
    "; Flag Wave: cloth banners on the flags whose models carry it (from a mod pack); 0 = the game's still flags\r\n"
    "flag_wave=1\r\n"
    "\r\n"
    "[tuning]\r\n"
    "; how many closed menus keep their textures (1-1024; fewer uses less memory)\r\n"
    "menu_textures_kept=1024\r\n"
    "; Culling: small parts of far-away soldiers (belts, quivers, bridles) are not drawn, for more fps in big battles:\r\n"
    "; off, light, normal or strong (also in the F8 panel, which saves its pick here)\r\n"
    "culling=light\r\n"
    "\r\n"
    "[mod]\r\n"
    "; the F8 panel (Culling, What's new) over the game\r\n"
    "f8_panel=1\r\n"
    "; copy a newer version from the Steam Workshop into the plugins folder at startup\r\n"
    "self_update=1\r\n"
    "; say in the log when a crash is inside this mod's own code\r\n"
    "crash_logging=1\r\n";
static void ini_open(void) {
    if (!g_dir[0]) return;
    wcscpy(g_ini, g_dir);
    wchar_t *sl = wcsrchr(g_ini, L'\\');
    if (!sl) { g_ini[0] = 0; return; }
    wcscpy(sl + 1, L"heapfix.ini");
    HANDLE f = CreateFileW(g_ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);   // only when missing
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(f, INI_DEFAULT, sizeof INI_DEFAULT - 1, &n, NULL);
    CloseHandle(f);
}
static int ini_int(const wchar_t *section, const wchar_t *key, int def) {
    wchar_t v[32];
    if (!g_ini[0] || !GetPrivateProfileStringW(section, key, L"", v, 32, g_ini)) return def;
    if ((v[0] >= L'0' && v[0] <= L'9') || v[0] == L'-') return _wtoi(v);
    static const wchar_t *const yes[3] = {L"on", L"true", L"yes"}, *const no[3] = {L"off", L"false", L"no"};
    for (int i = 0; i < 3; i++) {
        if (!_wcsicmp(v, yes[i])) return 1;
        if (!_wcsicmp(v, no[i])) return 0;
    }
    return def;
}
static int fix_on(const wchar_t *key) { return ini_int(L"fixes", key, 1) != 0; }
static const float CULL_PX[4] = {0, 16, 22, 28};   // off, light, normal, strong (x86 fgpanel.c)
static int ini_culling(void) {
    static const wchar_t *const keys[4] = {L"off", L"light", L"normal", L"strong"};
    wchar_t v[16] = L"light";
    if (g_ini[0]) GetPrivateProfileStringW(L"tuning", L"culling", L"light", v, 16, g_ini);
    for (int i = 0; i < 4; i++) if (!_wcsicmp(v, keys[i])) return i;
    return 1;
}

static DWORD WINAPI stats_thread(void *a);
static wchar_t g_home[MAX_PATH];   // Sol's loader: the attila64 folder (the plugins folder's parent); empty under Ivan's host
extern void (*g_d3d_frame)(void *swapchain);   // d3d64.c
extern void (*g_d3d_device)(void *dev, void *ctx);
static int g_scriptext_on, g_panel_on, g_flagwave_on;
// Culling: off / light / normal / strong. The F8 panel's pick is applied at once and saved in heapfix.ini (the one
// line the mod ever changes in that file), as on x86.
static const wchar_t *const CULL_KEYS[4] = {L"off", L"light", L"normal", L"strong"};
static void culling_choose(int c) {
    if (c < 0 || c > 3) c = 0;
    if (c) g_partcull_px = CULL_PX[c];
    InterlockedExchange(&g_partcull_on, c > 0);
}
void culling_pick(int c) {   // from the F8 panel
    if (c < 0 || c > 3) c = 0;
    culling_choose(c);
    if (g_ini[0]) WritePrivateProfileStringW(L"tuning", L"culling", CULL_KEYS[c], g_ini);
    plog("culling: %ls (picked in the F8 panel, saved in heapfix.ini)", CULL_KEYS[c]);
}
static void frame_all(void *swapchain) {       // before each Present, on the drawing thread
    (void)swapchain;
    if (g_scriptext_on) scriptext64_frame();
    if (g_panel_on && !bridge64_owns_panel()) panel64_frame(swapchain);   // with the upscaling add-on its panel is the F8 panel
    if (g_flagwave_on) flagwave64_frame(swapchain);
    uicache64_frame(swapchain);   // the UI cache probe's frame sequence; returns at once without attila64\uicache_probe.txt
}
static void install_all(void) {
    static const char *const x87_why[] = {"", "no x64 ELF executable at 0x400000", "CPU without SSE3 (fisttp)",
                                          "engine thread rounding control is not round-to-nearest", "sites differ: other engine build?"};
    static const char *const tf_why[] = {"VirtualProtect failed", "a thread was inside its first bytes", "code differs from this engine build", "", "switched off in heapfix.ini"};
    struct x87_result x87;
    unsigned char *tramp = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);   // before the pause
    unsigned char *low = NULL;   // batchfix stubs: 5-byte jumps from the engine must reach them (below 2 GB)
    for (uintptr_t a = 0x7FF00000; !low && a >= 0x10000000; a -= 0x10000)
        low = VirtualAlloc((void *)a, LOW_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    ini_open();
    int se_wanted = g_home[0] && scriptext64_prepare(g_home);   // only with a lua_ext folder
    int bx_wanted = g_home[0] && bridge64_prepare(g_home, plog);   // the upscaling add-on: <game>\FrameBridge\helper64.exe, not addon=off
    int fw_sw;
    char fw_who[160];   // the flag wave's pack switch: asked after the other threads are going again (it reads files)
    int on_x87 = fix_on(L"x87_rounding"), on_tf = fix_on(L"battle_textures"), on_pl = fix_on(L"small_object_pools"), on_so = fix_on(L"render_list_sorts"),
        on_ba = fix_on(L"battle_draw_batching"), on_hq = fix_on(L"terrain_height_cache"), on_lf = fix_on(L"render_loop_prefetch"),
        on_uk = fix_on(L"menu_textures"), on_sg = fix_on(L"sound_crash_guard"), cull = ini_culling();
    int keep = ini_int(L"tuning", L"menu_textures_kept", 1024);
    g_uikeep_keep = keep < 1 ? 1 : keep > 1024 ? 1024 : (unsigned)keep;
    g_mem_low = low; g_mem_tramp = tramp;
    int on_panel = ini_int(L"mod", L"f8_panel", 1) != 0;
    int on_uc = g_home[0] && ini_int(L"fixes", L"battle_ui_cache", 1) != 0;   // on by default, as on 32-bit
    if (on_uc) { uicache64_setup(plog); if (g_home[0]) uicache64_probe(g_home); }   // the probe: only with attila64\uicache_probe.txt
    culling_choose(cull);
    wchar_t hqv[16] = L"";
    GetEnvironmentVariableW(L"PERFFIX_HQ", hqv, 16);
    int hq_mode = !on_hq || !_wcsicmp(hqv, L"off") ? 0 : !_wcsicmp(hqv, L"verify") ? 1 : 2;
    MEMORY_BASIC_INFORMATION mbi;   // the engine: a committed ELF header at 0x400000
    int elf = VirtualQuery((void *)0x400000, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && *(const uint32_t *)0x400000 == 0x464c457f;
    enum { OFF = 2 };               // a fix switched off in heapfix.ini (the installs return 1, 0, -1, -2)
    int on_heap = ini_int(L"fixes", L"memory_allocator", 1) != 0, on_str = fix_on(L"fast_string_functions");
    int sl = elf && g_home[0] ? install_slots64(on_heap, on_str) : 0;   // pointer slots only: no code is rewritten
    pause_others();
    memset(&x87, 0, sizeof x87);
    if (elf && on_x87) install_x87fix(&x87);
    int tf = !elf ? 0 : !on_tf ? OFF : install_texfix64();   // no engine: touch nothing
    int pl = !elf ? 0 : !on_pl ? OFF : install_pool64();
    int so = !elf || !tramp || !on_so ? 0 : install_sortfix64(tramp);
    int ba = !elf || !on_ba ? 0 : install_batchfix64(low ? low + LOW_BATCH : NULL);
    int hq = !elf || !tramp || !hq_mode ? 0 : install_heightcache64(tramp + 256, hq_mode);
    int pc = !elf ? 0 : !low ? -1 : install_partcull64(low + LOW_PARTCULL);   // always in: the F8 panel switches it while playing
    int uk = !elf ? 0 : !on_uk ? OFF : install_uikeep64();
    int lf = !elf || !on_lf ? 0 : install_loopfetch64(low ? low + LOW_LOOPFETCH : NULL);
    int sg = !elf ? 0 : !on_sg ? OFF : !low ? -1 : install_soundguard64(low + LOW_SOUNDGUARD);
    int se = !elf || !se_wanted ? 0 : install_scriptext64(low ? low + LOW_SCRIPTEXT : NULL);
    int uc = !elf ? 0 : !on_uc ? OFF : install_uicache64();
    int bx = !elf || !bx_wanted ? 0 : install_bridge64(low ? low + LOW_BRIDGE : NULL);
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    int paused = g_nthreads;
    resume_others();
    if (!elf) { plog("no x64 ELF executable at 0x400000 - nothing changed"); return; }
    if (g_ini[0]) plog("settings: %ls", g_ini);
    if (!on_x87) plog("x87fix: off in heapfix.ini");
    else if (x87.why) plog("x87fix: %s - nothing changed", x87_why[x87.why]);
    else plog("x87fix: %d of %d rounding-mode blocks replaced, %d kept (other bytes or a thread inside)", x87.patched, x87.sites, x87.kept);
    plog(tf == 1 ? "texfix: DXT1 imposter compression replaced (SSE2, same output)" : "texfix: not replaced - %s", tf_why[tf + 2]);
    plog(pl == 1 ? "pool: campaign AI pool free lookup replaced (per-thread cache + move-to-front)" : "pool: not replaced - %s", tf_why[pl + 2]);
    char notes[400];
    if (!on_so) plog("sortfix: off in heapfix.ini");
    else {
        notes[0] = 0;
        if (tramp) loader64_site_notes(notes, sizeof notes, g_sort_state, g_sort_label, 5, "fast sort");
        plog("sortfix: %d of 5 render-list sorts replaced (key extraction + radix sort)%s", so, notes);
    }
    if (!hq_mode) plog("heightcache: off (heapfix.ini or PERFFIX_HQ=off)");
    else plog(hq == 1 ? "heightcache: campaign terrain height cache installed (%s)" : "heightcache: not installed - %s",
              hq == 1 ? (hq_mode == 1 ? "VERIFY mode: original always runs, mismatches counted" : "cache") : tf_why[hq + 2]);
    if (!on_ba) plog("batchfix: off in heapfix.ini");
    else if (ba < 0) plog("batchfix: no executable memory below 2 GB - not installed");
    else {
        notes[0] = 0;
        loader64_site_notes(notes, sizeof notes, g_batch_state, g_batch_label, batchfix64_count, "batching patch");
        plog("batchfix: %d of %d draw loops patched (equal variant values share batches)%s", ba, batchfix64_count, notes);
    }
    fgp_set_cull(cull); fgp_set_cull_available(pc == 1);
    if (on_panel && g_home[0]) { panel64_setup(plog); g_panel_on = 1; }
    if (pc == 1 && !cull) plog("unit detail cull: off (culling=off in heapfix.ini; the F8 panel can switch it on)");
    else if (pc == 1) plog("unit detail cull on (%ls): soldier parts under %.0f px tall (helmets and heads %.0f px) are not drawn", CULL_KEYS[cull], (double)g_partcull_px, (double)g_partcull_head_px);
    else plog("unit detail cull: not installed - %s", pc == -1 && !low ? "no executable memory below 2 GB" : tf_why[pc + 2]);
    if (uk == 1) plog("menu textures: the last %u closed ones stay loaded (faster reopening)", g_uikeep_keep);
    else plog("menu textures: not kept - %s", tf_why[uk + 2]);
    if (!on_lf) plog("loop prefetch: off in heapfix.ini");
    else if (lf < 0) plog("loop prefetch: no executable memory below 2 GB - not installed");
    else plog("loop prefetch: %d of %d loops prefetch ahead (render lists, animation texture)", lf, loopfetch64_count);
    if (!on_heap) plog("memory allocator: off (memory_allocator=0 in heapfix.ini): the game keeps the Windows heap");
    else plog(sl & 1 ? "memory allocator: the game's malloc, free, realloc and operator new go to mimalloc (blocks of the Windows heap go back to it)"
                     : "memory allocator: not installed - the game's memory slots or the loader's free are not where expected");
    if (!on_str) plog("string functions: off in heapfix.ini");
    else plog(sl & 2 ? "string functions: the game's strlen and memcmp go to Windows' own" : "string functions: not installed - the slots are not filled in");
    plog(sg == 1 ? "sound crash guard: a sound without a buffer goes silent for a moment instead of crashing the game" : "sound crash guard: not installed - %s",
         sg == -1 && !low ? "no executable memory below 2 GB" : tf_why[sg + 2]);
    if (se == 1) { g_scriptext_on = 1; plog("script extender on: lua_ext scripts run in each script environment, Ctrl+` opens the Lua console (log: lua_ext, lua_ext.log)"); }
    else if (se_wanted) plog("script extender: lua_ext folder found, but this game build's Lua is not where expected - off");
    if (uc == 1) plog("battle UI cache installed: the unit cards (cards_panel) are shown from a cache, redrawn about 15 times a second and on input");
    else plog("battle UI cache: not installed - %s", uc == OFF ? "off (battle_ui_cache=0 in heapfix.ini)" : tf_why[uc + 2]);
    if (bx_wanted) bridge64_installed(bx);
    if (!fix_on(L"flag_wave")) plog("flag wave: off in heapfix.ini");
    else if (!g_home[0]) plog("flag wave: not under the 64-bit loader - off");
    else if ((fw_sw = flagwave64_pack_switch(g_home, fw_who, sizeof fw_who)) < 0)
        plog("flag wave not installed: no loaded pack carries flagwave\\flag_wave.txt (a flag pack does)");
    else if (!fw_sw) plog("flag wave not installed: %s carries flagwave\\flag_wave.txt and it says 0", fw_who);
    else if (flagwave64_setup(g_home)) { g_d3d_device = flagwave64_device; g_flagwave_on = 1; plog("flag wave: on (%s carries flagwave\\flag_wave.txt = 1; log: flagwave.log)", fw_who); }
    else plog("flag wave: its shaders did not compile (d3dcompiler_47) - off, see flagwave.log");
    g_d3d_frame = frame_all;
    plog("%d other threads were paused while patching", paused);
    CloseHandle(CreateThread(NULL, 64 * 1024, stats_thread, NULL, 0, NULL));
}

// Every 10 s, when something changed: texfix calls/blocks and the engine's own count of failed file opens and stats
// (CA::open_file_fails, int at 0x4c0c750; research/fileio_attrcache.md) to size an attrcache port.
static DWORD WINAPI stats_thread(void *a) {
    (void)a;
    LONG64 last_calls = -1, last_hq = -1; int last_fails = -1;
    unsigned last_parts = 0; LONG last_uk = 0, last_sg = 0;
    for (;;) {
        Sleep(10000);
        unsigned parts, skipped, forced;
        partcull64_counts(&parts, &skipped, &forced);
        if (parts != last_parts) {   // counters since the last line: only while soldiers are being drawn
            static unsigned p0, s0;
            if (parts - p0) plog("stats: unit detail cull: %u of %u soldier parts not drawn (%.0f%%), %u left alone (forced distance)",
                                 skipped - s0, parts - p0, 100.0 * (skipped - s0) / (parts - p0), forced);
            p0 = last_parts = parts; s0 = skipped;
        }
        if (g_uk[0] + g_uk[1] != last_uk) {
            last_uk = g_uk[0] + g_uk[1];
            plog("stats: menu textures: %ld kept instead of deleted, %ld kept again after reuse, %ld deleted as oldest, %ld already gone", g_uk[0], g_uk[1], g_uk[2], g_uk[3]);
        }
        static LONG64 last_heap;
        if (g_heap64[0] != last_heap) {   // only with the allocator on
            plog("stats: allocator: %lld allocations served since the last line, %lld frees of Windows-heap blocks passed on, %lld operator new fallbacks",
                 (long long)(g_heap64[0] - last_heap), (long long)g_heap64[1], (long long)g_heap64[2]);
            last_heap = g_heap64[0];
        }
        if (g_soundguard_skips != last_sg) {
            last_sg = g_soundguard_skips;
            plog("stats: sound crash guard: %ld sounds had no buffer and went silent for a moment instead of crashing the game", last_sg);
        }
        int fails = *(volatile const int *)0x4c0c750;
        if (g_tf_calls == last_calls && fails == last_fails && g_hq[0] == last_hq) continue;
        last_calls = g_tf_calls; last_fails = fails; last_hq = g_hq[0];
        plog("stats: texfix %lld calls, %lld blocks; engine failed opens/stats %d; height %lld calls, %lld engine thread, %lld hits, %lld mismatches, %ld clears",
             (long long)g_tf_calls, (long long)g_tf_blocks, fails, (long long)g_hq[0], (long long)g_hq[1], (long long)g_hq[2], (long long)g_hq[3], g_hq_clears);
    }
    return 0;
}

typedef int (*sdl_init_fn)(unsigned);
__declspec(dllexport) int SDL_Init(unsigned flags) {
    static sdl_init_fn real;
    static LONG once;
    if (!real) {
        wchar_t path[MAX_PATH + 20]; wcscpy(path, g_dir); wcscat(path, L"\\SDL2_real.dll");
        HMODULE m = LoadLibraryW(path);
        real = m ? (sdl_init_fn)(void *)GetProcAddress(m, "SDL_Init") : NULL;
        if (!real) { plog("SDL2_real.dll or its SDL_Init not found next to the proxy"); return -1; }
    }
    if (!InterlockedExchange(&once, 1)) { plog("attila64_perffix loaded, first SDL_Init(%#x)", flags); install_all(); }
    return real(flags);
}

// Report log (x86 diag.c): Sol's loader keeps its own crash log, so only what it cannot know is added: that a crash
// is inside this mod (the DLL itself, or its stubs and trampolines), or has this mod on the call stack - the loader's
// crash lines name game and loader addresses only.
// The loader's own handler is a vectored one that ends the process at the first exception it sees (loader.c,
// crash_filter), so an unhandled-exception filter is never reached under it. Ours is a vectored handler put in front
// of the loader's. It writes one line, once, and always passes the exception on. Nothing in this mod raises or
// handles exceptions of its own, and C++ exceptions and the debugger's notices (which the loader also lets pass) are
// left alone, so a line here is a real fault.
int crash_is_ours(uintptr_t at, uintptr_t self, uintptr_t self_size, uintptr_t low, uintptr_t tramp) {
    return (at - self < self_size) ? 1 : (low && at - low < LOW_SIZE) ? 2 : (tramp && at - tramp < 4096) ? 3 : 0;
}
// 1 = an exception this mod reports: an error or warning status that is not a C++ exception
int crash_code_counts(DWORD code) { return (code & 0x80000000u) && code != 0xE06D7363u; }
// 1 = `at` is where a call made by this DLL's code returns to: inside an executable section of the DLL and right
// behind a call instruction (E8 rel32, or FF /2 in its 2, 3, 6 and 7 byte forms). A plain number on the stack that
// only lies in the DLL's range (its module handle, a pointer to its data) does not count. The stubs and trampolines
// make no calls of their own (the part cull's calls are made from the DLL's code), so only the DLL is looked at.
int crash_return_address(uintptr_t at, uintptr_t self, const IMAGE_NT_HEADERS *nt) {
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        uintptr_t lo = self + s->VirtualAddress, hi = lo + s->Misc.VirtualSize;
        if (!(s->Characteristics & IMAGE_SCN_MEM_EXECUTE) || at < lo + 7 || at >= hi) continue;
        const unsigned char *p = (const unsigned char *)at;
        if (p[-5] == 0xE8) return 1;
        static const int back[4] = {2, 3, 6, 7};
        for (int k = 0; k < 4; k++) if (p[-back[k]] == 0xFF && (p[-back[k] + 1] & 0x38) == 0x10) return 1;
    }
    return 0;
}
static uintptr_t g_game_hi;
void crash64_setup(const wchar_t *dir);                                                                     // crash64.c
void crash64_record(EXCEPTION_POINTERS *e, uintptr_t game_lo, uintptr_t game_hi, void (*log)(const char *fmt, ...));
static LONG CALLBACK crash_first(EXCEPTION_POINTERS *e) {
    static LONG once;
    if (!e || !e->ExceptionRecord || !e->ContextRecord || !crash_code_counts(e->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    // every such exception ends the game under this loader: its record line and dump, once (crash64.c)
    if (g_game_hi) crash64_record(e, 0x400000, g_game_hi, plog);
    if (once) return EXCEPTION_CONTINUE_SEARCH;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)g_self + ((const IMAGE_DOS_HEADER *)g_self)->e_lfanew);
    uintptr_t self = (uintptr_t)g_self, size = nt->OptionalHeader.SizeOfImage, low = (uintptr_t)g_mem_low, tramp = (uintptr_t)g_mem_tramp;
    uintptr_t at = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    static const char *const where[4] = {"", "perffix64.dll", "stubs", "trampolines"};
    int ours = crash_is_ours(at, self, size, low, tramp);
    if (ours) {
        if (!InterlockedExchange(&once, 1))
            plog("EXCEPTION %08lx inside the engine fix: %s+%#llx (thread %lu) - please report this with the log; heapfix.ini switches single fixes off",
                 code, where[ours], (unsigned long long)(at - (ours == 1 ? self : ours == 2 ? low : tramp)), GetCurrentThreadId());
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == 0xC00000FDu) return EXCEPTION_CONTINUE_SEARCH;   // stack overflow: no room to look at the stack
    // return addresses of ours on this thread's stack: the fault is in code this mod called, or that called it
    const NT_TIB *tib = (const NT_TIB *)NtCurrentTeb();
    uintptr_t sp = (uintptr_t)e->ContextRecord->Rsp, top = (uintptr_t)tib->StackBase, bottom = (uintptr_t)tib->StackLimit;
    if (sp < bottom || sp >= top || (sp & 7)) return EXCEPTION_CONTINUE_SEARCH;   // not on this thread's own stack
    char list[160]; size_t n = 0; int found = 0;
    for (const uintptr_t *p = (const uintptr_t *)sp; (uintptr_t)(p + 1) <= top && p < (const uintptr_t *)sp + 512 && found < 4; p++) {
        if (!crash_return_address(*p, self, nt)) continue;
        n += (size_t)snprintf(list + n, sizeof list - n, "%sperffix64.dll+%#llx", found ? ", " : "", (unsigned long long)(*p - self));
        if (n >= sizeof list) n = sizeof list - 1;
        found++;
    }
    if (found && !InterlockedExchange(&once, 1))
        plog("EXCEPTION %08lx at %p with the engine fix on the call stack (%s; thread %lu) - please report this with the log; heapfix.ini switches single fixes off",
             code, (void *)at, list, GetCurrentThreadId());
    return EXCEPTION_CONTINUE_SEARCH;
}
int update64_check(HMODULE self, void (*log)(const char *fmt, ...));   // update64.c
extern int g_update_state;
void fgp_set_update(int state);                                        // heapfix/fgpanel.c

// Sol Invictus's 64-bit Attila (attila64.exe 1.3.1) loads plugins\*.dll and calls this after it mapped and patched
// the same ELF, before any engine code runs. The block starts: int version; const char *text; game start; game end;
// plugins folder; log(fmt, ...). Sites his loader already patched fail each fix's own byte check and are left alone.
// (His published header, attila64_plugin.h, has the same layout: uint32 api_version, engine_version, game_lo, game_hi,
// plugin_dir, log, then patch, hook and code_near_game, which this plugin does not use: it patches with its own
// checked writes and its own memory near the game, so it does not share the loader's one 8 KB page of hook stubs.)
struct attila64_api { int version; const char *text; void *game_lo, *game_hi; const char *dir; void (*log)(const char *, ...); };
// The loader's own choice of graphics route, made before any plugin loads (its graphics_choice.inc, setup_graphics_route;
// picker.c settings_graphics_dx11): DirectX 11 unless a test switch says otherwise, settings.txt says
// graphics=opengl_fallback (written after a DirectX 11 start that closed early), or this PC cannot make a DirectX 11
// device of level 11_0 or has no d3dcompiler_47.dll. The fixes are made for its DirectX 11 route: with them in, the
// game stalled at its start under OpenGL (2026-11-22, three starts). NULL = DirectX 11, else why not.
static const char *graphics_not_dx11(const wchar_t *home) {
    char e[32];
    DWORD n = GetEnvironmentVariableA("ATTILA_DX11", e, sizeof e);
    if (n && n < sizeof e && !strcmp(e, "1")) return NULL;
    n = GetEnvironmentVariableA("ATTILA_GRAPHICS", e, sizeof e);
    if (n && n < sizeof e) { if (_stricmp(e, "dx11")) return "test switch ATTILA_GRAPHICS"; }
    else {
        wchar_t p[MAX_PATH];
        char t[4096] = "\n";
        _snwprintf(p, MAX_PATH, L"%ls\\settings.txt", home);
        FILE *f = _wfopen(p, L"rb");
        if (f) { size_t got = fread(t + 1, 1, sizeof t - 2, f); t[got + 1] = 0; fclose(f); }
        for (const char *q = t; (q = strstr(q, "\ngraphics=")) != NULL; q++)
            if (!_strnicmp(q + 10, "opengl_fallback", 15)) return "graphics=opengl_fallback in attila64\\settings.txt";
    }
    static const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;   // the loader's own test (dx11_usable)
    ID3D11Device *d = NULL;
    ID3D11DeviceContext *c = NULL;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &want, 1, D3D11_SDK_VERSION, &d, NULL, &c);
    if (c) ID3D11DeviceContext_Release(c);
    if (d) ID3D11Device_Release(d);
    if (FAILED(hr)) return "this PC could not make a DirectX 11 device";
    if (!LoadLibraryA("d3dcompiler_47.dll")) return "d3dcompiler_47.dll missing";
    return NULL;
}
static DWORD WINAPI opengl_notice(void *a) {
    (void)a;
    MessageBoxW(NULL, L"64-bit Attila is starting with OpenGL this time.\n\nThe Attila Engine Performance Fix works only with "
                      L"DirectX 11, so it stays off and the game runs without it.\n\nTo use DirectX 11 again, press "
                      L"\"Use DirectX 11 again\" in the 64-bit Attila launcher.",
                L"Attila Engine Performance Fix", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    return 0;
}

__declspec(dllexport) int attila64_plugin_init(const struct attila64_api *api) {
    static LONG once;
    if (!api || api->version < 1 || !api->log) return 1;
    if (InterlockedExchange(&once, 1)) return 0;
    g_host_log = api->log;
    // One engine fix per game, the same rule and name as the 32-bit dinput8.dll (heapfix.c DllMain): a second copy of
    // this plugin under another name finds Local\AttilaEngineFix_<pid> held and stays out. Held for the process's life.
    wchar_t mx[64];
    _snwprintf(mx, 64, L"Local\\AttilaEngineFix_%lu", GetCurrentProcessId());
    HANDLE mh = CreateMutexW(NULL, FALSE, mx);
    if (mh && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mh);
        plog("Attila Engine Performance Fix %s: another copy is already running in this game (%ls) - this one stays out; "
             "only one engine fix should be installed", MOD_VERSION_STR, mx);
        return 0;
    }
    unsigned lv[4] = {0, 0, 0, 0};
    char ltext[40] = "";
    int known = loader64_version(GetModuleHandleW(NULL), lv);
    if (known) snprintf(ltext, sizeof ltext, " %u.%u.%u", lv[0], lv[1], lv[2]);
    plog("Attila Engine Performance Fix %s, 64-bit plugin for 64-bit Attila%s (interface %d)", MOD_VERSION_STR, ltext, api->version);
    if (!known) plog("loader: its version could not be read from attila64.exe; every fix checks the game's code before it changes it");
    else if (loader64_compare(lv) > 0)
        plog("loader: newer than the newest one this fix was checked with (" LOADER64_CHECKED_STR "); every fix checks the game's code before it changes it, the lines below say what went in");
    if ((uintptr_t)api->game_lo != 0x400000) {   // every address of ours is the game file's own, fixed one
        plog("the game is at %p under this loader, not at 0x400000 - nothing changed", api->game_lo);
        return 1;
    }
    wcscpy(g_home, g_dir);   // the attila64 folder: the plugins folder's parent
    wchar_t *sl = wcsrchr(g_home, L'\\');
    if (sl) *sl = 0;
    {   // the plugin's own log in the attila64 folder: this start's fresh, the last one's kept
        wchar_t prev[MAX_PATH];
        _snwprintf(g_logpath, MAX_PATH, L"%ls\\perffix64.log", g_home);
        _snwprintf(prev, MAX_PATH, L"%ls\\perffix64_previous.log", g_home);
        MoveFileExW(g_logpath, prev, MOVEFILE_REPLACE_EXISTING);
        plog("Attila Engine Performance Fix %s, 64-bit plugin for 64-bit Attila%s (interface %d); log, settings and crash "
             "records of this plugin are in this folder", MOD_VERSION_STR, ltext, api->version);
    }
    g_game_hi = (uintptr_t)api->game_hi;
    const char *gl = graphics_not_dx11(g_home);
    if (gl) {   // the loader draws with OpenGL this start: nothing of ours goes in, the player is told why
        plog("this start draws with OpenGL (%s): the engine fix works only with DirectX 11 and changes nothing in this start", gl);
        CloseHandle(CreateThread(NULL, 64 * 1024, opengl_notice, NULL, 0, NULL));   // the game goes on loading behind it
        return 0;
    }
    install_all();
    if (!install_d3d64(g_home, plog)) plog("d3d11: attila64.exe does not import D3D11CreateDevice - no fps log, no per-frame checks");
    if (ini_int(L"mod", L"crash_logging", 1)) { crash64_setup(g_home); AddVectoredExceptionHandler(1, crash_first); }   // first: in front of the loader's own
    if (ini_int(L"mod", L"self_update", 1)) { update64_check(g_self, plog); fgp_set_update(g_update_state); }
    else plog("update: off (self_update=0 in heapfix.ini)");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason == DLL_THREAD_DETACH) { pool64_thread_exit(); sortfix64_thread_exit(); }
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DWORD n = GetModuleFileNameW(h, g_dir, MAX_PATH);
        if (n && n < MAX_PATH) { wchar_t *p = wcsrchr(g_dir, L'\\'); if (p) *p = 0; }
    }
    return TRUE;
}
