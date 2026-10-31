// Dev-only measurement (passtime_on.txt): inclusive main-thread time per frame of the draw pass loops and the render
// levels above them, to size a multithreaded draw split (AGENT_LOG 33). Timing only; what runs is unchanged.
// One generic hook for any calling convention: the entry stub notes the time and swaps the return address for an exit
// stub; the exit stub adds the elapsed time and jumps to the real return address. Main thread only; entries are matched
// to exits by stack address, so an exception unwinding past a hooked function cannot desynchronise the shadow stack.
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include "gamerva.h"

// Frame budget (AGENT_LOG 50): the battle frame's subsystem roots (not FUN_100c1950 itself: fps.c hooks it). Copy lengths are whole instructions with no
// relative operands that no jump lands inside (checked against the full disassembly). The pass-loop table of
// AGENT_LOG 34 is in that section.
static const struct { unsigned rva, copy; const char *name; } PT_FUNCS[] = {
    {0x0C1C10, 10, "game update FUN_100c1c10"},
    {0x04552F0, 5, "battle update FUN_104552f0"},
    {0x03FEA50, 6, "  FUN_103fea50"},
    {0x01766B0, 7, "  FUN_101766b0"},
    {0x042D9C0, 5, "  FUN_1042d9c0"},
    {0x04790D0, 6, "  FUN_104790d0"},
    {0x1897A30, 6, "  effects FUN_11897a30"},
    {0x1768FC0, 6, "render frame FUN_11768fc0"},
    {0x1778E90, 5, "  FUN_11778e90"},
    {0x1768A00, 6, "  FUN_11768a00"},
    {0x176A450, 5, "  FUN_1176a450"},
    {0x19237A0, 5, "  FUN_119237a0"},
    {0x179E890, 8, "  FUN_1179e890"},
    {0x192B640, 6, "  FUN_1192b640"},
    {0x16862B0, 5, "  FUN_116862b0"},
    {0x170AF80, 5, "  FUN_1170af80"},
    {0x1748A80, 5, "  FUN_11748a80"},
    {0x179A6B0, 7, "  state commit FUN_1179a6b0"},
    {0x186BE70, 10, "sim join + effects FUN_1186be70"},
    {0x1893CC0, 6, "  effects update FUN_11893cc0"},
    {0x182E080, 8, "  animation batching FUN_1182e080"},
    {0x17DB0F0, 7, "  FUN_117db0f0"},
    // inside the view renderer FUN_1176a4b0 (main 3D view stage vtable +0x1C9CF00: +0x6C, +0xB8, +0x80/+0x84/+0x88)
    {0x17293B0, 9, "view: stage+6C FUN_117293b0"},
    {0x173EED0, 6, "view: stage+B8 FUN_1173eed0"},
    {0x172B570, 6, "view: FUN_1172b570"},
    {0x172B690, 10, "view: FUN_1172b690"},
    {0x1785C60, 6, "view: FUN_11785c60"},
    {0x1749620, 7, "view: FUN_11749620"},
    {0x16FCD40, 7, "view: FUN_116fcd40"},
    {0x176DF00, 5, "view: FUN_1176df00"},
    {0x1779170, 6, "view: stage+80 FUN_11779170"},
    {0x08F91C0, 6, "view: stage+84 FUN_108f91c0"},
    {0x1779250, 8, "view: stage+88 FUN_11779250"},
    // the generic scene node's child renderers (scene node probe, AGENT_LOG 51); FUN_118de492 is a this-adjust thunk to FUN_118de390
    {0x17FA310, 6, "node: FUN_117fa310"},
    // bridge probe: the renderer's "set camera" (camera block, stage); push esi + mov esi,[esp+0Ch] = 5 bytes
    {0x177BB10, 5, "camera: FUN_1177bb10"},
    // draw split sizing (AGENT_LOG 53): the pass loops and the draw submit
    {0x17AEF40, 6, "pass: FUN_117aef40"},
    {0x17AF2B0, 6, "pass: FUN_117af2b0"},
    {0x17AF720, 6, "pass: FUN_117af720"},
    {0x17AFC10, 6, "pass: FUN_117afc10"},
    {0x17AFF90, 6, "pass: FUN_117aff90"},
    {0x17B0400, 6, "pass: FUN_117b0400"},
    {0x17B0F80, 7, "pass: FUN_117b0f80"},
    {0x17B1250, 7, "pass: FUN_117b1250"},
    {0x17B1520, 7, "pass: FUN_117b1520"},
    {0x17B17F0, 8, "pass: FUN_117b17f0"},
    {0x17B1A10, 6, "pass: FUN_117b1a10"},
    {0x17B20E0, 8, "pass: FUN_117b20e0"},
    {0x17B2C80, 8, "pass: FUN_117b2c80"},
    {0x17B3280, 8, "pass: FUN_117b3280"},
    {0x17B3880, 8, "pass: FUN_117b3880"},
    {0x17B4480, 8, "pass: FUN_117b4480"},
    {0x17B09F0, 7, "pass: FUN_117b09f0"},
    {0x1778390, 8, "pass: submit FUN_11778390"},
};
#define PT_N (sizeof PT_FUNCS / sizeof PT_FUNCS[0])

static unsigned char *g_pt_code;  // per function: trampoline (16) + entry stub (16)
void *g_pt_tramp[PT_N];
DWORD g_pt_main;
volatile unsigned long long g_pt_ticks[PT_N], g_pt_calls[PT_N];
struct pt_frame { unsigned slot, ret, id, pad; unsigned long long t, pad2; };  // 32 bytes, as the stubs index it
_Static_assert(sizeof(struct pt_frame) == 32, "pt_frame layout");
struct pt_frame g_pt_stack[256];
unsigned g_pt_sp;
unsigned long long g_pt_lost;
// Per-call log of the per-view render function (caller, object vtable, ticks), grouped in the report.
unsigned g_pt_view_id = 0xFFFFFFFF, g_pt_scene_id = 0xFFFFFFFF, g_pt_fx_id = 0xFFFFFFFF, g_pt_vn, g_pt_base, g_pt_submit_id;
unsigned g_pt_vret[65536], g_pt_vvt[65536], g_pt_vtick[65536];
// with "node" in passtime_on.txt the per-call log is FUN_117fa310 instead, plus the frame number and the draw submits
// made inside each call, dumped raw to passtime_calls_<n>.csv (one cascade per call: AGENT_LOG 61)
unsigned g_pt_vframe[65536], g_pt_vsub[65536];
extern volatile LONG g_geo_frame;
static int g_pt_node_mode;
// with "gpu" in passtime_on.txt, gputime.c sets these and the stubs call it on entry/exit of the flagged functions
unsigned char g_pt_gpu[PT_N];
const unsigned *g_pt_args;   // stack arguments of the flagged function being entered (valid inside g_gt_enter)
void (*g_gt_enter)(unsigned id, const void *obj);
void (*g_gt_exit)(unsigned id);
void (*g_gt_frame)(void);
void (*g_gt_report)(void (*log)(const char *fmt, ...));
const char *pt_name(unsigned id) { return id < PT_N ? PT_FUNCS[id].name : "?"; }
unsigned pt_id_of(unsigned rva) { for (unsigned i = 0; i < PT_N; i++) if (PT_FUNCS[i].rva == rva) return i; return 0xFFFFFFFF; }
static wchar_t g_pt_dir[MAX_PATH];

void pt_exit(void);

// enter: [esp] = id, [esp+4] = the caller's return address
__attribute__((naked)) void pt_enter(void) {
    __asm__ volatile(
        "pushal\n\t"
        "pushfl\n\t"
        "movl %%fs:0x24, %%eax\n\t"
        "cmpl _g_pt_main, %%eax\n\t"
        "jne 1f\n\t"
        "movl _g_pt_sp, %%ecx\n\t"
        "cmpl $256, %%ecx\n\t"
        "jae 1f\n\t"
        "shll $5, %%ecx\n\t"                       // 32-byte frames: slot, ret, id, pad, t (8)
        "leal _g_pt_stack(%%ecx), %%edi\n\t"
        "leal 40(%%esp), %%eax\n\t"                // address of the return-address slot
        "movl %%eax, (%%edi)\n\t"
        "movl 40(%%esp), %%eax\n\t"
        "movl %%eax, 4(%%edi)\n\t"
        "movl 36(%%esp), %%eax\n\t"
        "movl %%eax, 8(%%edi)\n\t"
        "cmpl _g_pt_view_id, %%eax\n\t"           // the per-view function: note the vtable of its object (ecx)
        "jne 5f\n\t"
        "movl 28(%%esp), %%eax\n\t"
        "movl (%%eax), %%eax\n\t"
        "movl %%eax, 12(%%edi)\n\t"
        "movl _g_pt_submit_id, %%eax\n\t"         // submits so far (for the per-call draw count)
        "movl _g_pt_calls(,%%eax,8), %%eax\n\t"
        "movl %%eax, 24(%%edi)\n\t"
        "5:\n\t"
        "rdtsc\n\t"
        "movl %%eax, 16(%%edi)\n\t"
        "movl %%edx, 20(%%edi)\n\t"
        "incl _g_pt_sp\n\t"
        "movl 8(%%edi), %%eax\n\t"                 // GPU timestamp for flagged functions (object = saved ecx)
        "cmpb $0, _g_pt_gpu(%%eax)\n\t"
        "je 6f\n\t"
        "leal 44(%%esp), %%ecx\n\t"                // the hooked function's stack arguments (bridge probe)
        "movl %%ecx, _g_pt_args\n\t"
        "pushl 28(%%esp)\n\t"
        "pushl %%eax\n\t"
        "calll *_g_gt_enter\n\t"
        "addl $8, %%esp\n\t"
        "6:\n\t"
        "movl $_pt_exit, 40(%%esp)\n\t"
        "1:\n\t"
        "popfl\n\t"
        "popal\n\t"
        "pushl %%eax\n\t"
        "movl 4(%%esp), %%eax\n\t"                 // id
        "movl _g_pt_tramp(,%%eax,4), %%eax\n\t"
        "movl %%eax, 4(%%esp)\n\t"
        "popl %%eax\n\t"
        "ret\n\t" ::: "memory");
}

// reached by the hooked function's ret; esp is just above its arguments
__attribute__((naked)) void pt_exit(void) {
    __asm__ volatile(
        "subl $4, %%esp\n\t"                       // the real return address goes here
        "pushal\n\t"
        "pushfl\n\t"
        "rdtsc\n\t"
        "movl %%eax, %%esi\n\t"
        "movl %%edx, %%edi\n\t"
        "leal 40(%%esp), %%ebx\n\t"                // esp at the hooked function's return
        "2:\n\t"
        "movl _g_pt_sp, %%ecx\n\t"
        "decl %%ecx\n\t"
        "movl %%ecx, _g_pt_sp\n\t"
        "shll $5, %%ecx\n\t"
        "leal _g_pt_stack(%%ecx), %%ecx\n\t"
        "movl (%%ecx), %%eax\n\t"                  // slot
        "addl $0x104, %%eax\n\t"                   // slot + 4 + up to 256 bytes of arguments
        "cmpl %%ebx, %%eax\n\t"
        "jae 3f\n\t"
        "addl $1, _g_pt_lost\n\t"                  // a frame below ours: left by an exception, drop it
        "adcl $0, _g_pt_lost+4\n\t"
        "jmp 2b\n\t"
        "3:\n\t"
        "movl 8(%%ecx), %%edx\n\t"                 // id
        "cmpb $0, _g_pt_gpu(%%edx)\n\t"
        "je 7f\n\t"
        "pushl %%ecx\n\t"
        "pushl %%edx\n\t"
        "calll *_g_gt_exit\n\t"
        "popl %%edx\n\t"
        "popl %%ecx\n\t"
        "7:\n\t"
        "subl 16(%%ecx), %%esi\n\t"
        "sbbl 20(%%ecx), %%edi\n\t"
        "addl %%esi, _g_pt_ticks(,%%edx,8)\n\t"
        "adcl %%edi, _g_pt_ticks+4(,%%edx,8)\n\t"
        "addl $1, _g_pt_calls(,%%edx,8)\n\t"
        "adcl $0, _g_pt_calls+4(,%%edx,8)\n\t"
        "cmpl _g_pt_view_id, %%edx\n\t"           // per-view calls: log caller, vtable and time
        "jne 4f\n\t"
        "movl _g_pt_vn, %%eax\n\t"
        "cmpl $65536, %%eax\n\t"
        "jae 4f\n\t"
        "movl 4(%%ecx), %%ebx\n\t"
        "movl %%ebx, _g_pt_vret(,%%eax,4)\n\t"
        "movl 12(%%ecx), %%ebx\n\t"
        "movl %%ebx, _g_pt_vvt(,%%eax,4)\n\t"
        "movl %%esi, _g_pt_vtick(,%%eax,4)\n\t"
        "movl _g_geo_frame, %%ebx\n\t"
        "movl %%ebx, _g_pt_vframe(,%%eax,4)\n\t"
        "movl _g_pt_submit_id, %%ebx\n\t"
        "movl _g_pt_calls(,%%ebx,8), %%ebx\n\t"
        "subl 24(%%ecx), %%ebx\n\t"
        "movl %%ebx, _g_pt_vsub(,%%eax,4)\n\t"
        "incl _g_pt_vn\n\t"
        "4:\n\t"
        "movl 4(%%ecx), %%eax\n\t"
        "movl %%eax, 36(%%esp)\n\t"
        "popfl\n\t"
        "popal\n\t"
        "ret\n\t" ::: "memory");
}

static void (*g_pt_log)(const char *fmt, ...);
static double g_pt_tsc_per_ms;
static LONG g_pt_frame0;
extern volatile LONG g_geo_frame;  // heapfix.c frame counter

void nodeprobe_report(void (*log)(const char *fmt, ...), long frames);
int install_nodeprobe(HMODULE m, void (*log)(const char *fmt, ...));
int install_gputime(HMODULE m, void (*log)(const char *fmt, ...));  // gputime.c
int install_bridgeprobe(HMODULE m, const wchar_t *dir, void (*log)(const char *fmt, ...));  // bridgeprobe.c
void passtime_report(void (*log)(const char *fmt, ...)) {
    if (!g_pt_code || !g_pt_tsc_per_ms) return;
    LONG frames = g_geo_frame - g_pt_frame0;
    g_pt_frame0 = g_geo_frame;
    if (frames <= 0) return;
    unsigned long long t[PT_N], c[PT_N];
    for (unsigned i = 0; i < PT_N; i++) {
        t[i] = g_pt_ticks[i];
        c[i] = g_pt_calls[i];
        g_pt_ticks[i] = g_pt_calls[i] = 0;  // shortcut: racy against the main thread for one entry, fine for a per-minute report
    }
    double f = frames * g_pt_tsc_per_ms;
    log("frame budget (%ld frames, inclusive main-thread time per frame; lost frames %llu):", frames, g_pt_lost);
    for (unsigned i = 0; i < PT_N; i++)
        log("  %-36s %6.2f ms  %8.1f calls", PT_FUNCS[i].name, t[i] / f, (double)c[i] / frames);
    // per-view breakdown: group by (caller, vtable)
    static struct { unsigned ret, vt; unsigned long long ticks; unsigned n; } g[512];
    unsigned ng = 0, vn = g_pt_vn < 65536 ? g_pt_vn : 65536;
    for (unsigned i = 0; i < vn; i++) {
        unsigned j = 0;
        while (j < ng && (g[j].ret != g_pt_vret[i] || g[j].vt != g_pt_vvt[i])) j++;
        if (j == ng) { if (ng == 512) continue; g[ng].ret = g_pt_vret[i]; g[ng].vt = g_pt_vvt[i]; g[ng].ticks = 0; g[ng].n = 0; ng++; }
        g[j].ticks += g_pt_vtick[i];
        g[j].n++;
    }
    if (g_pt_node_mode) {  // raw per-call dump: frame, caller, node vtable, microseconds, submits
        static int dump_n;
        wchar_t path[MAX_PATH];
        _snwprintf(path, MAX_PATH, L"%ls\\passtime_calls_%d.csv", g_pt_dir, dump_n++);
        FILE *f = _wfopen(path, L"w");
        if (f) {
            fprintf(f, "frame,caller,vtable,us,submits\n");
            for (unsigned i = 0; i < vn; i++)
                fprintf(f, "%u,%X,%X,%.1f,%u\n", g_pt_vframe[i], g_pt_vret[i] - g_pt_base, g_pt_vvt[i] - g_pt_base,
                        g_pt_vtick[i] / g_pt_tsc_per_ms * 1000.0, g_pt_vsub[i]);
            fclose(f);
            log("  per-call log: %u calls of FUN_117fa310 written to passtime_calls_%d.csv", vn, dump_n - 1);
        }
    }
    LONG vframes = frames;  // the log covers the first 65536 calls of the minute: scale per call, not per frame
    double ms_per_call_total = 0;
    for (unsigned j = 0; j < ng; j++) ms_per_call_total += g[j].ticks / g_pt_tsc_per_ms;
    for (unsigned pass = 0; pass < 20 && ng; pass++) {  // top 20 by time
        unsigned best = 0;
        for (unsigned j = 1; j < ng; j++) if (g[j].ticks > g[best].ticks) best = j;
        if (!g[best].n) break;
        log("  view caller +0x%X vtable +0x%X: %4.1f%% of view time, %u calls, %.3f ms per call", g[best].ret - g_pt_base,
            g[best].vt - g_pt_base, 100.0 * (g[best].ticks / g_pt_tsc_per_ms) / ms_per_call_total, g[best].n,
            g[best].ticks / g_pt_tsc_per_ms / g[best].n);
        g[best].ticks = 0; g[best].n = 0;
    }
    (void)vframes;
    g_pt_vn = 0;
    nodeprobe_report(log, frames);
    if (g_gt_report) g_gt_report(log);
}

// Hooks one function: its first `copy` bytes (whole instructions, no relative branches) move to a trampoline.
int pt_hook_one(unsigned char *site, unsigned copy, unsigned id) {
    unsigned char *tr = g_pt_code + 32 * id, *st = tr + 16;
    memcpy(tr, site, copy);
    tr[copy] = 0xE9;
    DWORD rel = (DWORD)((site + copy) - (tr + copy + 5));
    memcpy(tr + copy + 1, &rel, 4);
    g_pt_tramp[id] = tr;
    st[0] = 0x68;  // push id
    memcpy(st + 1, &id, 4);
    st[5] = 0xE9;  // jmp pt_enter
    rel = (DWORD)((unsigned char *)pt_enter - (st + 10));
    memcpy(st + 6, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), tr, 32);
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE9;
    rel = (DWORD)(st - (site + 5));
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return 1;
}

int g_pt_force_bridge;   // release with the DLSS add-on (heapfix.c): lean bridge mode without passtime_on.txt
int install_passtime(HMODULE m, void (*log)(const char *fmt, ...)) {
    unsigned char *base = (unsigned char *)m;
    g_pt_base = (unsigned)(UINT_PTR)base;
    // passtime_on.txt containing "node": per-call log of the scene renderer instead of the per-view function
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)install_passtime, &self);
    GetModuleFileNameW(self, g_pt_dir, MAX_PATH);
    wchar_t *sl = wcsrchr(g_pt_dir, L'\\');
    if (sl) *sl = 0;
    wchar_t flag[MAX_PATH];
    _snwprintf(flag, MAX_PATH, L"%ls\\passtime_on.txt", g_pt_dir);
    int gpu_mode = 0, bridge_mode = 0, bridge_full = 0;
    FILE *ff = _wfopen(flag, L"rb");
    if (ff) {
        char buf[64] = {0};
        fread(buf, 1, sizeof buf - 1, ff);
        fclose(ff);
        g_pt_node_mode = strstr(buf, "node") != NULL;
        gpu_mode = strstr(buf, "gpu") != NULL;
        bridge_mode = strstr(buf, "bridge") != NULL;
        // AGENT_LOG 102: all 53 timed functions cost the game ~100 fps at a busy spot (134 -> ~40). "bridge" hooks only what
        // the bridge needs (the stage driver; the set-camera has its own jump hook, bridgeprobe.c); "bridgefull" all of
        // them plus the scene node probe (captures)
        bridge_full = strstr(buf, "bridgefull") != NULL;
        if (bridge_mode) gpu_mode = 0;   // both use the stage callbacks
    }
    if (g_pt_force_bridge) { bridge_mode = 1; bridge_full = gpu_mode = g_pt_node_mode = 0; }
    if (gpu_mode) g_pt_node_mode = 0;  // GPU time is grouped by stage (the per-view function)
    for (unsigned i = 0; i < PT_N; i++) {
        if (PT_FUNCS[i].rva == (g_pt_node_mode ? 0x17FA310u : 0x176A450u)) g_pt_view_id = i;  // gets the per-call log
        if (PT_FUNCS[i].rva == 0x1778390) g_pt_submit_id = i;
        if (PT_FUNCS[i].rva == 0x17FA310) g_pt_scene_id = i;
        if (PT_FUNCS[i].rva == 0x1893CC0) g_pt_fx_id = i;  // gpu mode: its D3D11 calls are timed  // gpu mode: per call (shadow cascades)
    }
    g_pt_log = log;
    g_pt_main = GetCurrentThreadId();
    g_pt_code = VirtualAlloc(NULL, PT_N * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_pt_code) return 0;
    // calibrate the TSC against QPC over ~50 ms
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    unsigned long long ta = __builtin_ia32_rdtsc();
    Sleep(50);
    QueryPerformanceCounter(&b);
    unsigned long long tb = __builtin_ia32_rdtsc();
    g_pt_tsc_per_ms = (double)(tb - ta) / ((b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart);
    g_pt_frame0 = g_geo_frame;
    int n = 0;
    for (unsigned i = 0; i < PT_N; i++) {
        unsigned char *site = base + game_rva(base, PT_FUNCS[i].rva);
        // the set-camera gets the bridge's own jump hook (bridgeprobe.c cam_hook): timed through here it cost ~9 ms per frame
        if (bridge_mode && !bridge_full && PT_FUNCS[i].rva != 0x176A450) continue;
        if (site[0] == 0xE9) {  // already hooked by another fix: leave it
            log("pass time: +0x%X already patched - not timed", PT_FUNCS[i].rva);
            continue;
        }
        n += pt_hook_one(site, PT_FUNCS[i].copy, i);
    }
    FlushInstructionCache(GetCurrentProcess(), g_pt_code, PT_N * 32);
    log("pass time: %d of %d functions timed (TSC %.0f per ms)", n, (int)PT_N, g_pt_tsc_per_ms);
    if (gpu_mode && install_gputime(m, log))
        for (unsigned i = 0; i < PT_N; i++)
            g_pt_gpu[i] = PT_FUNCS[i].rva == 0x176A450 || PT_FUNCS[i].rva == 0x1778E90 || PT_FUNCS[i].rva == 0x17FA310 ||
                           PT_FUNCS[i].rva == 0x186BE70 || PT_FUNCS[i].rva == 0x1893CC0;  // stages, render level, scene renderer calls, sim join, effects update
    if (bridge_mode && install_bridgeprobe(m, g_pt_dir, log))
        for (unsigned i = 0; i < PT_N; i++)   // stages, render level, scene renderer calls, the camera loop
            g_pt_gpu[i] = PT_FUNCS[i].rva == 0x176A450 || PT_FUNCS[i].rva == 0x1778E90 || PT_FUNCS[i].rva == 0x17FA310 ||
                           PT_FUNCS[i].rva == 0x173EED0 || PT_FUNCS[i].rva == 0x177BB10;
    if (bridge_mode && !bridge_full) {
        g_pt_view_id = 0xFFFFFFFF;   // no per-call log either
        log("pass time: lean bridge mode (stage driver only)");
    } else install_nodeprobe(m, log);
    return n;
}

// Scene node probe (AGENT_LOG 51): FUN_11722420 is the generic scene-graph node (for each child at +0x1C, count
// +0x18: child->vfunc+8(arg, this)). On the main thread its entry stub records each child's render function before
// the original runs, so the report can count how many distinct node render functions draw in a battle frame.
#define NP_SIZE 8192
static unsigned g_np_fn[NP_SIZE], g_np_n[NP_SIZE];
static unsigned long long g_np_calls;
static unsigned char *g_np_tramp;

void __cdecl np_record(const unsigned *self) {
    if (GetCurrentThreadId() != g_pt_main) return;
    unsigned count = self[0x18 / 4];
    const unsigned *const *kids = (const unsigned *const *)(UINT_PTR)self[0x1C / 4];
    g_np_calls++;
    for (unsigned i = 0; i < count && i < 4096; i++) {
        unsigned fn = ((const unsigned *)(UINT_PTR)*kids[i])[2];  // child->vtable[2]
        unsigned h = (fn * 2654435761u) & (NP_SIZE - 1);
        while (g_np_fn[h] && g_np_fn[h] != fn) h = (h + 1) & (NP_SIZE - 1);
        g_np_fn[h] = fn;
        g_np_n[h]++;
    }
}

__attribute__((naked)) void np_stub(void) {
    __asm__ volatile("pushal\n\tpushfl\n\t"
                     "pushl %ecx\n\tcalll _np_record\n\taddl $4, %esp\n\t"
                     "popfl\n\tpopal\n\t"
                     "jmpl *_g_np_tramp\n\t");
}

void nodeprobe_report(void (*log)(const char *fmt, ...), long frames) {
    if (!g_np_tramp || frames <= 0) return;
    unsigned distinct = 0;
    unsigned long long total = 0;
    for (unsigned i = 0; i < NP_SIZE; i++) if (g_np_fn[i]) { distinct++; total += g_np_n[i]; }
    log("scene nodes: %u distinct child render functions under the generic node; %.0f node visits, %.0f child calls per frame",
        distinct, (double)g_np_calls / frames, (double)total / frames);
    for (int k = 0; k < 25; k++) {
        unsigned best = NP_SIZE;
        for (unsigned i = 0; i < NP_SIZE; i++) if (g_np_fn[i] && g_np_n[i] && (best == NP_SIZE || g_np_n[i] > g_np_n[best])) best = i;
        if (best == NP_SIZE) break;
        log("  child render +0x%X: %.1f calls per frame", g_np_fn[best] - g_pt_base, (double)g_np_n[best] / frames);
        g_np_n[best] = 0;
    }
    memset(g_np_fn, 0, sizeof g_np_fn);
    memset(g_np_n, 0, sizeof g_np_n);
    g_np_calls = 0;
}

int install_nodeprobe(HMODULE m, void (*log)(const char *fmt, ...)) {
    static const unsigned char PRO[7] = {0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x77, 0x1C};  // push esi; push edi; mov edi,ecx; mov esi,[edi+1Ch]
    unsigned char *site = (unsigned char *)m + game_rva(m, 0x1722420);
    if (memcmp(site, PRO, 7) != 0) { log("scene node probe: code differs - skipped"); return 0; }
    g_np_tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_np_tramp) return 0;
    memcpy(g_np_tramp, site, 7);
    g_np_tramp[7] = 0xE9;
    DWORD rel = (DWORD)((site + 7) - (g_np_tramp + 12));
    memcpy(g_np_tramp + 8, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), g_np_tramp, 64);
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE9;
    rel = (DWORD)((unsigned char *)(void *)np_stub - (site + 5));
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log("scene node probe installed");
    return 1;
}
