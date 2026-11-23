// In the release since 6.3 (not dev-only any more): the frame bridge's driver for the upscaling add-on, the DLSS glue
// (viewport jitter, pixel-shader camera, sampler bias, bloom), the flag wave, and the dev tools all of it grew out of
// (the frame capture on bridge_go.txt, the motion probe on motion_on.txt). What is drawn DOES change with the add-on or a
// flag pack; without both, heapfix.c installs none of this.
// It began as a probe (passtime_on.txt containing "bridge") that found the four places the DLSS frame bridge needs (DLSS_PLAN,
// AGENT_LOG 93/94) and only looked. Those four are still how it finds its way:
//  1. Present: CreateDXGIFactory1 (looked up with GetProcAddress, like D3D11CreateDevice) hands out the real factory
//     with CreateSwapChain wrapped; the swapchain's Present is wrapped (chained: the 4K UI mod hooks it too).
//  2./3. When bridge_go.txt appears in the game folder, the next frame is captured: every render stage (FUN_1176a450,
//     named by its object's vtable), render level and scene-renderer call, in order, with the targets bound at entry
//     and exit (back buffer or not, size, format, depth buffer), and half-size dumps of the back buffer at the exit of
//     each stage that drew into it and at Present -> where the UI starts, and the main depth buffer.
//  4. For the 30 frames after that, the camera blocks FUN_1173eed0 renders with ([this+0x98] blocks of 0x1C8 bytes at
//     this+0x294, set per sub-viewport; AGENT_LOG 51/94) are written to bridge_camera_<n>.bin.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <intrin.h>
#include <math.h>
#include <d3dcompiler.h>
#include "fgb_motion_api.h"
#include "gamerva.h"
#include "patch.h"

extern unsigned g_pt_base;
extern DWORD g_pt_main;
unsigned pt_id_of(unsigned rva);                          // passtime.c
extern void (*g_gt_enter)(unsigned id, const void *obj);  // passtime.c calls these from its stubs
extern void (*g_gt_exit)(unsigned id);
extern void (*g_gt_frame)(void);
extern void (*g_gt_report)(void (*log)(const char *fmt, ...));

static wchar_t g_dir[MAX_PATH];
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain *g_sc;
static DXGI_SWAP_CHAIN_DESC g_scd;
static unsigned g_id_stage, g_id_level, g_id_scene, g_id_view, g_id_cam, g_mod_size;
extern const unsigned *g_pt_args;   // passtime.c: stack arguments of the function being entered

// live frame bridge (bridge step 2, AGENT_LOG 95): fgbridge_on.txt in the game folder = helper64.exe path, Streamline
// dir, frames to generate (0/1/3), one per line. The game's Present is replaced by the helper's, through Streamline.
int fgb_start(ID3D11Device *, ID3D11DeviceContext *, HWND, UINT, UINT, DXGI_FORMAT, const wchar_t *, const wchar_t *, const wchar_t *,
              unsigned, void (*)(const char *, ...));   // fgbridge.c
void fgb_hudless(ID3D11Resource *backbuffer, ID3D11Resource *depth, const float vp[16]);
void fgb_present(ID3D11Resource *backbuffer);
int fgb_active(void);
unsigned fgb_frames(void), fgb_pace_timeouts(void);
void fgb_stop(void);
IDXGISwapChain *fgb_fallback_frame(ID3D11Resource *src, HRESULT(STDMETHODCALLTYPE *create)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *,
                                                                                          IDXGISwapChain **));
void fgb_fallback_close(void);
void fgb_hud_post(ID3D11Resource *backbuffer);
// rslink.cpp: ReShade's effects at the HUD-less point (DLSS add-on on, ReShade in the game folder; Rome 2's port)
int rsl_attach(HMODULE self, HMODULE reshade, void (*log)(const char *, ...), int (*swapchain_cb)(void *), const wchar_t *state);
int rsl_effects(ID3D11DeviceContext *ctx, ID3D11Resource *bb, ID3D11Resource *depth);
void rsl_camera(const float vp[16]);
void fgp_set_note(const wchar_t *note);   // fgpanel.c
// weak: the offline tests that include this file stub the bridge without them
void fgb_release_gpu(void) __attribute__((weak));
void fgm_release(void) __attribute__((weak));
int fgp_move_device(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT bw, UINT bh);
IDXGISwapChain *fgb_fallback_sc(void);
const wchar_t *fgb_gpu_name(void);
int fgb_d3d12_ok(ID3D11Device *dev);   // fgbridge.c: the card has Direct3D 12 feature level 12_0
HWND fgb_fallback_window(void);
void *fgb_vt_original(void **vt, size_t slot);
void *fgb_unhooked(void *fn);
void fgb_set_sr(int on, UINT render_w, UINT render_h);
void fgb_jitter(float *jx, float *jy);
static void jit_frame_start(void), jit_frame_end(void);   // DLAA viewport jitter (below)
static void sr_set(int mode);   // below the settings
static int g_depth_bad;   // the game's depth is MSAA: no upscaling this session (sr_set)
// per-object motion (fgmotion.c, AGENT_LOG 99): fgbridge_on.txt line 4 contains "motion"
static int g_motion;              // the motion pass is set up (bridge mode); on / off: g_motion_on and the F11 panel
static int g_motion_on;
unsigned fgb_sr_backend(void);
static int g_copy_patched;         // the game's instance record copy calls fgm_copy (stays patched for the session)
// the F11 panel (fgpanel.c)
#include "fgpanel.h"   // FgpApply: what a pick in the panel calls
void fgp_set_cap(int choice, unsigned fps);
void fgp_set_preset(int choice);
void fgp_set_sharp(int choice);
void fgp_set_presets_available(int on);
void fgb_set_preset(unsigned preset);
int fgb_helper_has_presets(void);
void fgb_set_vsync(int on);
int fgb_helper_has_vsync(void);
int fgb_driver_vsync(void);
void fgp_set_vsync(int on);
void fgp_set_vsync_available(int on);
void fgp_set_vsync_driver(int v);
void fgp_set_fps_counter(int corner);
void fgp_set_key(int vk, const wchar_t *name);
void fgp_set_fg(int fg);
int fgb_restart(unsigned tech);
int fgb_can_restart(void);
int fgb_fences_lost(void);
void fgp_set_tech(int sr, int fg);
void fgb_set_sharpness(float con);
void fgb_set_frame_limit(unsigned us);
int fgp_init(ID3D11Device *, ID3D11DeviceContext *, UINT bw, UINT bh, DXGI_FORMAT fmt, int sr, int fg, int motion, FgpApply apply);
void fgp_frame(ID3D11Resource *backbuffer, HWND hwnd);
void fgp_set_fps(double game, double shown);
void fgp_di8_created(void *di);
extern void (*g_dev_di8_created)(void *di);   // heapfix.c, dev builds
unsigned fgb_shown(void);
void fgb_set_fg(unsigned frames_to_generate);
unsigned fgb_fg_max(void);
void fgb_set_fg_test_limit(unsigned limit);
void fgb_set_test_not_nvidia(int on);
void fgb_set_tech(unsigned tech);   // fgbridge.c: 0 NVIDIA only, 1 auto, 2 AMD FSR for both
void fgm_set_enabled(int on);
static HWND g_out_hwnd;           // the game's window (the helper presents into it)
static int g_sr;                 // DLAA (defined with the jitter below)
static void jit_install(void);
void fgb_set_motion(int (*pass)(ID3D11Resource *depth, float jx, float jy));
ID3D11Texture2D *fgb_mv_texture(void);
int fgb_make_vectors(ID3D11Resource *depth, const float vp[16]);
static ID3D11Resource *g_main_depth;   // the scene's depth buffer, seen at the HUD-less point
static unsigned g_mvx_calls, g_mvx_frames, g_mvx_none, g_mvx_last = ~0u;   // fgb_motion_vectors(): other mods' calls
int fgm_init(ID3D11Device *, ID3D11DeviceContext *, ID3D11Texture2D *mv, UINT w, UINT h, void (*log)(const char *fmt, ...));
void fgm_layout_created(const void *il, const D3D11_INPUT_ELEMENT_DESC *e, UINT n, unsigned long long sig);
void fgm_mapped(ID3D11Resource *r, D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE *m);
void *__cdecl fgm_copy(void *dst, const void *src, size_t n);
void fgm_draw(ID3D11DeviceContext *c, UINT idx, UINT inst, UINT start_idx, INT base_v, UINT start_inst);
int fgm_pass(ID3D11Resource *depth, float jx, float jy);
void fgm_frame_end(void);
void fgm_report(void (*log)(const char *fmt, ...));
void fgm_dump_next(const wchar_t *path);
static void mp_capture_start(void), mp_capture_end(FILE *f), mp_hook(const char *where);   // motion probe (below)
static int g_live, g_live_state;   // state: 0 not started, 1 running, 2 off for good
static int g_start_failed;          // the add-on is installed but did not start (helper, GPU): F11 says so (start_failed_panel)
static wchar_t g_helper[MAX_PATH], g_sl[MAX_PATH], g_logdir[MAX_PATH];
static wchar_t g_settings[MAX_PATH];   // the add-on's settings.txt (the F11 panel saves to it); empty in dev runs
static unsigned g_fg, g_live_w, g_live_h, g_vp_frame = ~0u, g_hud_frame = ~0u, g_br_hud, g_br_novp, g_br_nodepth, g_br_native;
// Loading screens and movies have no 3D scene (no HUD-less point): they must not be shrunk for DLSS, or they show at the
// render size. A frame is shrunk only when the previous one had a scene; a scene frame that was not shrunk (the first
// after a loading screen) is shown at native size, without DLSS for that frame.
static int g_hud_this_frame, g_scene_last, g_frame_scaled;
static float g_vp[16];

static void (*g_log)(const char *fmt, ...);
static int g_fw_on;   // flag wave (defined with it below)

// Present statistics (reported every 30 s)
static unsigned g_presents, g_present_other_thread, g_sync[5];
static struct { unsigned ret, game_ret, n; } g_callers[8];
static HRESULT(STDMETHODCALLTYPE *r_present)(IDXGISwapChain *, UINT, UINT);
static HRESULT(STDMETHODCALLTYPE *r_create_sc)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
static IDXGISwapChain *g_hid_sc;
static HWND g_hid_hwnd;            // the hidden window the game's swapchain was made on (w_create_sc)
// ReShade (rslink.cpp): it wraps the game's swapchain, so the bridge's Present / GetDesc hooks go on the real one under
// the wrapper (g_rs_sc): ReShade draws its menu before the frame goes to the helper, and its menu reads the game window's keys
static int g_rs, g_rs_tried, g_rs_creating, g_rs_fb;   // g_rs_fb: the fallback's Present through ReShade's wrapper
static IDXGISwapChain *g_rs_sc, *g_rs_new;
static unsigned g_rs_frames;        // frames with ReShade's effects at the HUD-less point
// DXGI's own Present / CreateSwapChain (dxgi_own): what the mod calls on. r_present / r_create_sc are what the slots held
// when we hooked them; that can be another mod's hook (the Steam overlay hooks early), and when that mod later hooks
// again on top of ours, calling it leads back to us: overlay -> UI mod -> ours -> overlay ... (6.3.2 tests: the
// alt-tab crash, and the fallback's frames never shown, each Present caught by the nested-Present guard).
static HRESULT(STDMETHODCALLTYPE *g_present_real)(IDXGISwapChain *, UINT, UINT);
static HRESULT(STDMETHODCALLTYPE *g_create_real)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
// Those functions past a jump another mod put over their first bytes (fgb_unhooked; 6.3.2 test: dxgi's Present started
// with E9 and led through the UI mod back to us, so the fallback's frames were all dropped as nested Presents). Used for
// the mod's own swapchain and for a Present that comes back to us; checked when needed (a mod may hook later).
#include "bp_unhooked.inc.c"
static HRESULT STDMETHODCALLTYPE present_body(IDXGISwapChain *sc, UINT sync, UINT flags) {
    static void *cache, *gave_up;
    return ((HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT))unhooked((void *)g_present_real, &cache, &gave_up, "Present"))(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE create_body(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out) {
    static void *cache, *gave_up;
    return ((HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **))unhooked(
        (void *)g_create_real, &cache, &gave_up, "CreateSwapChain"))(f, dev, d, out);
}   // the game's swapchain on the hidden window (w_create_sc), until the game makes another

// ---- The parts (split 2026-11-21: this file was 3,300 lines). Still ONE translation unit: they are included here, in
// this order, share the state declared above and each other's, and are not compiled on their own - the compiled DLL
// was byte for byte the same before and after the split. A part's first line says what it holds; a part may only use
// what an earlier part (or a declaration above) provides. The install (the entry point) is at the end of this file.
#include "bp_capture.inc.c"     // dev: the frame capture's helpers; the stage vtables and switches the later parts read
#include "bp_stages.inc.c"      // bp_enter / bp_exit / bp_frame, called by passtime.c's stage hook: the HUD-less point
#include "bp_present.inc.c"     // Present: frame end, the fallback after the bridge, nested Presents; the vtable slot helpers
#include "bp_dlss.inc.c"        // the DLSS glue on the context: jitter, the pixel shaders' camera, mip bias (-> bp_campatch, bp_lodsampler)
#include "bp_create.inc.c"      // shaders / layouts the game creates (-> bp_flagwave: the flag wave)
#include "bp_draw.inc.c"        // the draw hooks, Map, the bloom source pass (-> bp_bloomsrc); dev: per-draw logging, vertex dump
#include "bp_settings.inc.c"    // settings.txt, the panel's callbacks, a new helper; dev: the sweep
#include "bp_swapchain.inc.c"   // the game's swapchain on the hidden window; the device / factory hooks; ReShade
#include "bp_api.inc.c"         // the three exports for other mods
#include "bp_report.inc.c"      // the 30 s report

// The renderer's set-camera FUN_1177bb10(camera block, stage) (AGENT_LOG 94): the bridge needs the main view's
// view-projection (block +0x1B0, stage vtable +0x1CCE4E4). Timing it through passtime (which swaps the return address)
// cost ~9 ms per frame at a busy battle spot (PresentMon: 60 fps vs 134 without it, AGENT_LOG 102), so this is a plain
// jump hook: the stub calls cam_seen(block, stage) and goes on into the original code (push esi; mov esi,[esp+0Ch] copied).
static unsigned char *g_cam_tramp;
__attribute__((cdecl, used)) void cam_seen(const unsigned char *cam, const unsigned *stage) {
    g_cam_calls++;
    if (!g_live || !cam || !stage || GetCurrentThreadId() != g_pt_main) return;
    if (stage[0] - g_pt_base != g_vt_main) return;
    memcpy(g_vp, cam + 0x1B0, 64);
    g_vp_frame = g_frame;
}
__attribute__((naked)) static void cam_stub(void) {
    __asm__ volatile(
        "pushal\n\t"
        "pushfl\n\t"
        "pushl 44(%%esp)\n\t"          // stage (ret at +36, camera +40, stage +44 after pushal / pushfl)
        "pushl 44(%%esp)\n\t"          // camera (shifted by the push above)
        "call _cam_seen\n\t"
        "addl $8, %%esp\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *_g_cam_tramp\n\t" ::: "memory");
}
static int cam_hook(HMODULE m, void (*log)(const char *fmt, ...)) {
    unsigned rva = game_rva(m, 0x177BB10);
    unsigned char *f = (unsigned char *)m + rva;
    static const unsigned char expect[5] = {0x56, 0x8B, 0x74, 0x24, 0x0C};   // push esi; mov esi, [esp+0Ch]
    if (!rva || rva + 5 > g_mod_size || memcmp(f, expect, 5)) { log("frame bridge: set-camera FUN_1177bb10 is not the expected code - no camera"); return 0; }
    g_cam_tramp = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_cam_tramp) return 0;
    patch_tramp(g_cam_tramp, f, 5);
    return patch_jump(f, 5, (void *)cam_stub);
}

// flag_wave=0 under [fixes] in heapfix.ini, or flagwave_off.txt in the game folder: none of the flag hooks go in, so a
// benchmark run carries none of this even with a flag pack loaded. The rule is heapfix.c's fix_off, the one every fix
// has (fixoff.h); this file had a second copy of it until 2026-11-21.
int flag_wave_off(void);   // heapfix.c: 1 when switched off (it logs that)

// The pack switch, asked once: 1 a loaded pack says on, 0 it says off, -1 no loaded pack carries the file. heapfix.c
// asks before it brings up the bridge for the flag wave's sake, so a player with neither a flag pack nor the add-on
// gets none of the bridge's hooks either (the first version asked only in here, after they were in: audit, finding 4).
int fw_pack_switch(const wchar_t *dir, char *who, int wn) {
    static int sw = -2;
    static char name[160];
    if (sw == -2) {
        static char mods[65536];
        diag_mods_text(dir, mods, sizeof mods);
        sw = fwp_from_list(mods, dir, name, sizeof name);
    }
    if (who && wn > 0) { strncpy(who, name, wn - 1); who[wn - 1] = 0; }
    return sw;
}

// addon=off in <fbdir>\settings.txt (the F8 panel writes it; read past 1 KB: AGENT_LOG 141). heapfix.c asks it too.
int addon_switched_off(const wchar_t *fbdir) {
    wchar_t sp[MAX_PATH];
    _snwprintf(sp, MAX_PATH, L"%ls\\settings.txt", fbdir);
    char text[4096] = {0};
    FILE *sf = _wfopen(sp, L"r");
    if (sf) { fread(text, 1, sizeof text - 1, sf); fclose(sf); }
    return strstr(text, "\naddon=off") || !strncmp(text, "addon=off", 9);
}

int install_bridgeprobe(HMODULE m, const wchar_t *dir, void (*log)(const char *fmt, ...)) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((unsigned char *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    g_mod_size = nt->OptionalHeader.SizeOfImage;
    wcsncpy(g_dir, dir, MAX_PATH - 1);
    g_vt_main = game_rva(m, 0x1CCE4E4); g_vt_hud = game_rva(m, 0x1C9A900); g_vt_overlay = game_rva(m, 0x1CA0858);
    for (int i = 0; i < 3; i++) g_vt_pre[i] = game_rva(m, g_vt_pre[i]);
    g_vt_fx = game_rva(m, 0x1C99FB8); g_vt_fx_end = game_rva(m, 0x1C9D064);
    g_id_stage = pt_id_of(0x176A450); g_id_level = pt_id_of(0x1778E90); g_id_scene = pt_id_of(0x17FA310); g_id_view = pt_id_of(0x173EED0); g_id_cam = pt_id_of(0x177BB10);
    void *old = iat_hook(m, "GetProcAddress", (void *)w_gpa);
    if (!old) { log("bridge probe: GetProcAddress import not found - not installed"); return 0; }
    r_gpa = (FARPROC(WINAPI *)(HMODULE, LPCSTR))old;
    g_gt_enter = bp_enter;
    g_gt_exit = bp_exit;
    g_gt_frame = bp_frame;
    g_gt_report = bp_report;
    {   wchar_t mp[MAX_PATH];
        _snwprintf(mp, MAX_PATH, L"%ls\\motion_on.txt", dir);
        if (GetFileAttributesW(mp) != INVALID_FILE_ATTRIBUTES) {
            InitializeCriticalSection(&g_mp_lock);
            g_mp_il = calloc(MP_MAX_LAYOUTS, sizeof *g_mp_il);
            _snwprintf(mp, MAX_PATH, L"%ls\\motion_vs.bin", dir);
            g_mp_vsf = _wfopen(mp, L"wb");
            g_mp_on = g_mp_il && g_mp_vsf;
            log("motion probe %s: vertex shaders to motion_vs.bin, draws logged in bridge captures", g_mp_on ? "on" : "NOT on");
        }
    }
    if (!flag_wave_off()) {
        // One question, asked once: does a loaded pack carry the switch file, and does it say 1 (fwpack.c)? Only then
        // are the game's prop shaders made from ours and the hooks placed. Everyone else gets none of it.
        char who[160];
        int sw = fw_pack_switch(dir, who, sizeof who);
        if (sw < 0) log("flag wave not installed: no loaded pack carries " FWP_NAME " (a flag pack does)");
        else if (!sw) log("flag wave not installed: %s carries " FWP_NAME " and it says 0", who);
    if (sw > 0) {
        wchar_t fp[MAX_PATH];
        g_fw_on = 1;
        log("flag wave on: %s carries " FWP_NAME " = 1; the game's prop shaders 55ca645c / 0cae34c7 (main view) and "
            "eb3006be (shadows) are made from ours", who);
        _snwprintf(fp, MAX_PATH, L"%ls\\flagwave_ab.txt", dir);
        g_fw_ab = GetFileAttributesW(fp) != INVALID_FILE_ATTRIBUTES;
        if (g_fw_ab) log("flag wave A/B: on in even 10 s windows of UTC time, off in odd (flagwave_ab.txt)");
    }
    }
    g_log = log;
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\fgbridge_on.txt", g_dir);
    wcscpy(g_logdir, g_dir);
    FILE *f = _wfopen(p, L"r");
    wchar_t addon[MAX_PATH];
    _snwprintf(addon, MAX_PATH, L"%ls\\FrameBridge", g_dir);
    _snwprintf(p, MAX_PATH, L"%ls\\helper64.exe", addon);
    // addon=off: heapfix.c leaves the add-on out, but a flag pack still brings this bridge up for the flag wave, and up to
    // 7.3 the helper then started here anyway (user's 1212 capture 2026-11-21: DLSS on with addon=off)
    if (!f && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES && addon_switched_off(addon)) {
        log("DLSS add-on switched off (addon=off in FrameBridge\\settings.txt): the frame bridge runs for the flag wave only, no DLSS");
        p[0] = 0;
    }
    if (!f && p[0] && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
        // the release add-on (AGENT_LOG 104): <game>\FrameBridge\ holds helper64.exe, NVIDIA's Streamline / DLSS DLLs (the
        // helper loads them from there after checking NVIDIA's signature), settings.txt, and the helper's logs
        wcscpy(g_helper, p);
        wcscpy(g_sl, addon);
        wcscpy(g_logdir, addon);
        _snwprintf(g_settings, MAX_PATH, L"%ls\\settings.txt", addon);
        char text[4096] = {0};   // the file is past 1 KB since dlss_preset / sharpness (Rome 2 DLSS_LOG 38)
        FILE *sf = _wfopen(g_settings, L"r");
        if (sf) { fread(text, 1, sizeof text - 1, sf); fclose(sf); }
        static const char *const MV_KEYS[2] = {"camera_only", "per_object"};
        g_sr = settings_value(text, "super_resolution=", SR_KEYS, 6, 2);       // default Quality (faster big battles)
        g_fg = (unsigned)settings_value(text, "frame_generation=", FG_KEYS, 4, 1);   // default 2x (limited to the GPU later)
        g_motion_on = settings_value(text, "motion_vectors=", MV_KEYS, 2, 1);  // default per object ("Full", AGENT_LOG 179)
        g_fcap = settings_value(text, "frame_cap=", CAP_KEYS, 5, 1);   // default auto
        g_preset = settings_value(text, "dlss_preset=", PRESET_KEYS, 5, 0);   // default: NVIDIA's per mode
        g_sharp = settings_value(text, "sharpness=", SHARP_KEYS, 4, 2);       // default medium (files without the key too)
        g_vsync = settings_value(text, "vsync=", VSYNC_KEYS, 2, 1);           // default on (files without the key too)
        g_fpsc = settings_value(text, "fps_counter=", FPSC_KEYS, 5, 0);       // default off
        g_pkey = settings_value(text, "panel_key=", KEY_KEYS, 18, 7);         // default F8 (files without the key too)
        fgp_set_key(KEY_VK[g_pkey], KEY_NAMES[g_pkey]);
        g_tech = settings_value(text, "technology=", TECH_KEYS, 4, 0);        // default auto (files without the key too)
        fgb_set_tech(g_tech == 1 ? 2 : g_tech == 3 ? 3 : 1);   // auto and dlss: 1 (NVIDIA where it can, FSR for the rest)
        log("  technology: %s", g_tech == 1 ? "AMD FSR for upscaling and frame generation" : g_tech == 3 ? "DLSS upscaling + AMD FSR frame generation"
                                : g_tech == 2 ? "NVIDIA DLSS (AMD FSR where the GPU lacks it)" : "auto (NVIDIA DLSS where the GPU has it, AMD FSR for the rest)");
        // Every first start saved the defaults, so a changed default is applied once to files from before it: per_object
        // before version=2 (players' reports: half the fps in big battles on 6-core CPUs; it mostly helps frame
        // generation), DLAA before version=3 (Quality renders smaller: faster big battles), camera_only before version=4
        // (per object now tracks trees and grass too and is the default: "Full"; user: "its needed to look good", AGENT_LOG 179),
        // medium with AMD FSR before version=5 (High now with FSR: AGENT_LOG 194; set at the bridge start, when the upscaler is known)
        int ver = !sf ? 5 : strstr(text, "version=5") ? 5 : strstr(text, "version=4") ? 4 : strstr(text, "version=3") ? 3 : strstr(text, "version=2") ? 2 : 1;
        int reset_mv = ver < 4 && !g_motion_on, reset_sr = ver < 3 && g_sr == 1;
        if (reset_mv) g_motion_on = 1;
        if (reset_sr) g_sr = 2;
        g_sharp_auto = !strstr(text, "sharpness=") || (ver < 5 && g_sharp == 2);
        g_live = 1;
        g_motion = 1;
        log("DLSS add-on found: super resolution %s, frame generation %s, motion vectors %s, frame cap %s, preset %s, sharpness %s, V-Sync %s (%ls%s%s)",
            SR_KEYS[g_sr], FG_KEYS[g_fg], MV_KEYS[g_motion_on], CAP_KEYS[g_fcap], PRESET_KEYS[g_preset], SHARP_KEYS[g_sharp], VSYNC_KEYS[g_vsync],
            sf ? L"settings.txt" : L"defaults",
            reset_mv ? "; camera_only was the old default, now per_object (Full)" : "", reset_sr ? "; dlaa was the old default, now quality" : "");
        if (!sf || ver < 5) settings_save();
        g_dev_di8_created = fgp_di8_created;
    }
    if (f) {
        char l[4][MAX_PATH] = {{0}};
        for (int i = 0; i < 4 && fgets(l[i], MAX_PATH, f); i++) l[i][strcspn(l[i], "\r\n")] = 0;
        fclose(f);
        MultiByteToWideChar(CP_ACP, 0, l[0], -1, g_helper, MAX_PATH);
        MultiByteToWideChar(CP_ACP, 0, l[1], -1, g_sl, MAX_PATH);
        g_fg = (unsigned)atoi(l[2]);
        g_live = g_helper[0] && g_sl[0] && (g_fg == 0 || g_fg == 1 || g_fg == 3);
        g_sr = strstr(l[3], "ultraperf") ? 5 : strstr(l[3], "perf") ? 4 : strstr(l[3], "balanced") ? 3 : strstr(l[3], "quality") ? 2
             : strstr(l[3], "dlaa") ? 1 : 0;   // the F11 panel switches it
        g_motion_on = strstr(l[3], "motion") != NULL;
        // dev: "preset<j|k|l|m>" (none: default), "sharp<off|low|high>" (none: medium)
        for (int k = 1; k < 5; k++) { char t[16]; snprintf(t, sizeof t, "preset%s", PRESET_KEYS[k]); if (strstr(l[3], t)) g_preset = k; }
        for (int k = 0; k < 4; k++) { char t[16]; snprintf(t, sizeof t, "sharp%s", SHARP_KEYS[k]); if (strstr(l[3], t)) g_sharp = k; }
        if (strstr(l[3], "vsyncoff")) g_vsync = 0;   // dev: V-Sync off
        // dev test of NVIDIA's GPU rules on this PC: treat the GPU as an RTX 20 / 30 (no frame generation) or an RTX 40 (2x)
        if (strstr(l[3], "fakegpu20")) { fgb_set_fg_test_limit(1); log("  test: GPU treated as an RTX 20 / 30 (no frame generation)"); }
        else if (strstr(l[3], "fakegpu40")) { fgb_set_fg_test_limit(2); log("  test: GPU treated as an RTX 40 (frame generation 2x at most)"); }
        if (strstr(l[3], "fakeamd")) { fgb_set_test_not_nvidia(1); log("  test: GPU treated as not NVIDIA (bridge stays off)"); }
        // dev: "forcefsr" AMD FSR for both (as technology=fsr), "nvonly" as an older engine fix (fakeamd then stays off)
        if (strstr(l[3], "forcefsr")) { g_tech = 1; fgb_set_tech(2); log("  test: AMD FSR for upscaling and frame generation"); }
        if (strstr(l[3], "nvonly")) { fgb_set_tech(0); log("  test: NVIDIA only, as an older engine fix"); }
        g_motion = g_live;   // set up whenever the bridge runs, so the F11 panel can switch it on
        log("frame bridge %s: helper %s, Streamline %s, frame generation %ux, DLSS %s", g_live ? "armed" : "NOT armed (bad fgbridge_on.txt)",
            l[0], l[1], g_fg + 1, (const char *[]){"off", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"}[g_sr]);
        log("  per-object motion vectors: %s (the settings panel switches it)", g_motion_on ? "on" : "off");
        if (g_live) g_dev_di8_created = fgp_di8_created;   // the panel hides the game's mouse buttons while the cursor is over it
    }
    if (g_live && cam_hook(m, log)) log("frame bridge: camera read through a jump hook on FUN_1177bb10");
    log("bridge probe installed: Present, render targets per stage, back buffer dumps and camera blocks on bridge_go.txt");
    return 1;
}
