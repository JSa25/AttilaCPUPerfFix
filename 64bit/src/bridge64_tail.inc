// bridge64_tail.inc: the 64-bit glue of the upscaling add-on's game side. Included at the end of out/gen/bridge64.c
// (tools/gen_bridge64.py: the 32-bit heapfix/bridgeprobe.c with its parts), so it is one translation unit with it and
// uses its state directly. What it replaces of the 32-bit add-on:
//   - passtime.c's stage hook (x86): an entry hook on WARSCAPE::SCENE_VIEW::process 0x30ce1c0, the 32-bit
//     FUN_1176a450 (same loop: the view's count, then process_internal per index). Sol's loader hooks process_internal
//     0x30ce260 (shadow_rate.inc), never process;
//   - the stage vtables: the 64-bit classes by name (vptr = vtable symbol + 0x10), each checked at install from the
//     class name the game file carries (vptr - 8 -> typeinfo, typeinfo + 8 -> its mangled name);
//   - the set-camera jump hook (x86): an entry hook on DEFERRED_PIPELINE_STATE::set_shared_camera 0x30c55c0, the
//     32-bit FUN_117864c0, where every camera setup ends (scene_camera_setup and its inlined copies). The main view's
//     matrix is the game's own UTILITYLIB::CAMERA::view_projection() 0x7501a0: the product the 32-bit add-on read at
//     camera +0x1B0 (in the frustum the scene stores behind its camera; bridge64_test: the frustum's copy, at its +0x6C, is
//     this function's result bit for bit);
//   - the record copy for the motion pass: three memcpy calls (the 32-bit one at +0x17DABAF) go to fgm_copy64;
//   - the game's GetProcAddress lookups of D3D11CreateDevice / CreateDXGIFactory1: d3d64.c's hooks call bridge64_device
//     (the context the loader gets is ctxwrap64's wrapper: its table is ours, so the 32-bit slot hooks stay in),
//     bridge64_swapchain (the 32-bit w_create_sc, adapted to CreateSwapChainForHwnd) and bridge64_present.
// The add-on is installed only when <game>\FrameBridge\helper64.exe is there and its settings.txt does not say
// addon=off (bridge64_prepare); then nothing of it exists in the game, Flag Wave or not (it has its own path here).
#include <stdint.h>
#include "d3d64.h"
// perffix64.c's (perffix64.h is not included: its fnv1a is the same FNV-1a as gamerva.h's, under another type)
int thread_inside(uint64_t start, uint64_t len);
int patch_code(void *at, const void *bytes, size_t n);
#define B64_SYSV __attribute__((sysv_abi))

// ---- what passtime.c gave the 32-bit bridge
unsigned g_pt_base;            // 0: the game is at its own addresses, so "vptr - base" is the vptr (every one is below 4 GB)
DWORD g_pt_main;               // the drawing thread: the device's (bridge64_device)
const unsigned *g_pt_args;     // only the 32-bit dev probes read it
void (*g_gt_enter)(unsigned id, const void *obj);
void (*g_gt_exit)(unsigned id);
void (*g_gt_frame)(void);
void (*g_gt_report)(void (*log)(const char *fmt, ...));
unsigned pt_id_of(unsigned rva) { (void)rva; return ~0u; }
// ReShade's add-on link (rslink.cpp) is 32-bit only: g_rs stays 0, nothing calls these
int rsl_attach(HMODULE self, HMODULE reshade, void (*log)(const char *, ...), int (*swapchain_cb)(void *), const wchar_t *state) {
    (void)self; (void)reshade; (void)log; (void)swapchain_cb; (void)state; return 0;
}
int rsl_effects(ID3D11DeviceContext *ctx, ID3D11Resource *bb, ID3D11Resource *depth) { (void)ctx; (void)bb; (void)depth; return 0; }
void rsl_camera(const float vp[16]) { (void)vp; }

#include "bridge64_sites.h"
static unsigned *const B64_VT_SLOT[8] = {&g_vt_main, &g_vt_hud, &g_vt_overlay, &g_vt_pre[0], &g_vt_pre[1], &g_vt_pre[2], &g_vt_fx, &g_vt_fx_end};

// the class name the game file carries for a vptr (Itanium ABI: typeinfo pointer just before the table)
static const char *b64_class(uint32_t vptr) {
    const uint64_t ti = *(const uint64_t *)(uintptr_t)(vptr - 8);
    if (ti < 0x400000u || ti >= 0x5280000u) return NULL;
    const uint64_t name = *(const uint64_t *)(uintptr_t)(ti + 8);
    return name >= 0x400000u && name < 0x5280000u ? (const char *)(uintptr_t)name : NULL;
}

// ---- the hooks
typedef B64_SYSV void (*b64_process_fn)(void *view, void *dc, const void *vp, void *rt, uint32_t face, uint32_t a, uint32_t b);
typedef B64_SYSV void (*b64_camera_fn)(void *self, const void *cam, void *shader, const void *view);
typedef B64_SYSV void (*b64_viewproj_fn)(float out[16], const void *cam);
static b64_process_fn b64_process_orig;
static b64_camera_fn b64_camera_orig;
static volatile LONG b64_stages_seen;
B64_SYSV static void b64_process(void *view, void *dc, const void *vp, void *rt, uint32_t face, uint32_t a, uint32_t b) {
    if (!b64_stages_seen && GetCurrentThreadId() == g_pt_main && !InterlockedExchange(&b64_stages_seen, 1))
        g_log("frame bridge: first render stage seen on the drawing thread (vptr %#llx)", (unsigned long long)*(const uint64_t *)view);
    bp_enter(g_id_stage, view);
    b64_process_orig(view, dc, vp, rt, face, a, b);
    bp_exit(g_id_stage);
}
// cam_seen of the 32-bit add-on: the main view's view-projection, on the drawing thread
B64_SYSV static void b64_camera(void *self, const void *cam, void *shader, const void *view) {
    g_cam_calls++;
    if (g_live && cam && view && GetCurrentThreadId() == g_pt_main && *(const uint64_t *)view == g_vt_main) {
        float m[16];
        ((b64_viewproj_fn)(uintptr_t)B64_VIEWPROJ)(m, cam);
        memcpy(g_vp, m, 64);
        g_vp_frame = g_frame;
    }
    b64_camera_orig(self, cam, shader, view);
}
B64_SYSV static void *fgm_copy64(void *dst, const void *src, size_t n) { return fgm_copy(dst, src, n); }

// relay (16 bytes) jmp [rip+0] -> to; trampoline (32 bytes) the 5 moved bytes, then jmp [rip+0] -> at + 5
static void b64_relay(uint8_t *at, const void *to) {
    uint64_t t = (uint64_t)(uintptr_t)to;
    at[0] = 0xFF; at[1] = 0x25; memset(at + 2, 0, 4); memcpy(at + 6, &t, 8);
}
static int b64_near(const uint8_t *code, uint32_t site) {
    int64_t d = (int64_t)((uintptr_t)code - site);
    return code && d > -0x7FF00000ll && d < 0x7FF00000ll;
}
// 1 hooked, 0 the game's code differs, -1 a thread inside / no near memory, -2 VirtualProtect failed
static int b64_entry(uint32_t at, uint32_t len, uint32_t fnv, uint8_t *code, void *fn, void **orig) {
    if (memcmp((const void *)(uintptr_t)at, B64_PRO, 5) || fnv1a((const unsigned char *)(uintptr_t)at, len) != fnv) return 0;
    if (!b64_near(code, at) || thread_inside(at, 5)) return -1;
    b64_relay(code, fn);
    uint8_t *t = code + 16;
    memcpy(t, B64_PRO, 5);
    b64_relay(t + 5, (const void *)(uintptr_t)(at + 5));
    *orig = t;
    uint8_t j[5] = {0xE9};
    int32_t rel = (int32_t)((int64_t)(uintptr_t)code - ((int64_t)at + 5));
    memcpy(j + 1, &rel, 4);
    return patch_code((void *)(uintptr_t)at, j, 5) ? 1 : -2;
}

// ---- install. bridge64_prepare: before the other threads are paused (files, log); install_bridge64: while they are
// (no allocation, CRT or log); bridge64_installed: after (log, the d3d64.c hooks).
static wchar_t b64_addon[MAX_PATH];
static int b64_hooks;   // install_bridge64's result
// the add-on's F8 panel (live, "did not start", or the fallback's after the bridge ran) replaces panel64's
int bridge64_owns_panel(void) { return g_live || g_out_hwnd; }

int bridge64_prepare(const wchar_t *home, void (*log)(const char *fmt, ...)) {
    g_log = log;
    wchar_t game[MAX_PATH], p[MAX_PATH];
    wcsncpy(game, home, MAX_PATH - 1); game[MAX_PATH - 1] = 0;
    wchar_t *sl = wcsrchr(game, L'\\');
    if (!sl) return 0;
    *sl = 0;   // the game folder: the attila64 folder's parent, where the 32-bit add-on lives too
    wcscpy(g_dir, game);
    wcscpy(g_logdir, game);
    _snwprintf(b64_addon, MAX_PATH, L"%ls\\FrameBridge", game);
    _snwprintf(p, MAX_PATH, L"%ls\\helper64.exe", b64_addon);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) { log("upscaling add-on: not installed (no FrameBridge\\helper64.exe in the game folder)"); return 0; }
    {   // addon=off in settings.txt (the F8 panel writes it; read past 1 KB): nothing of the add-on goes in
        wchar_t sp[MAX_PATH];
        _snwprintf(sp, MAX_PATH, L"%ls\\settings.txt", b64_addon);
        char text[4096] = {0};
        FILE *sf = _wfopen(sp, L"r");
        if (sf) { fread(text, 1, sizeof text - 1, sf); fclose(sf); }
        if (strstr(text, "\naddon=off") || !strncmp(text, "addon=off", 9)) {
            log("upscaling add-on switched off (addon=off in FrameBridge\\settings.txt): none of it is installed");
            return 0;
        }
    }
    if (GetEnvironmentVariableW(L"ATTILA_UPSCALE", NULL, 0)) {   // the loader's own upscaling test draws the interface into a frame of its own
        log("upscaling add-on: off while the loader's ATTILA_UPSCALE test is set");
        return 0;
    }
    // the 32-bit install's settings part, unchanged (the same settings.txt serves both games)
    wcscpy(g_helper, p);
    wcscpy(g_sl, b64_addon);
    wcscpy(g_logdir, b64_addon);
    _snwprintf(g_settings, MAX_PATH, L"%ls\\settings.txt", b64_addon);
    char text[4096] = {0};
    FILE *sf = _wfopen(g_settings, L"r");
    if (sf) { fread(text, 1, sizeof text - 1, sf); fclose(sf); }
    static const char *const MV_KEYS[2] = {"camera_only", "per_object"};
    g_sr = settings_value(text, "super_resolution=", SR_KEYS, 6, 2);
    g_fg = (unsigned)settings_value(text, "frame_generation=", FG_KEYS, 4, 1);
    g_motion_on = settings_value(text, "motion_vectors=", MV_KEYS, 2, 1);
    g_fcap = settings_value(text, "frame_cap=", CAP_KEYS, 5, 1);
    g_preset = settings_value(text, "dlss_preset=", PRESET_KEYS, 5, 0);
    g_sharp = settings_value(text, "sharpness=", SHARP_KEYS, 4, 2);
    g_vsync = settings_value(text, "vsync=", VSYNC_KEYS, 2, 1);
    g_fpsc = settings_value(text, "fps_counter=", FPSC_KEYS, 5, 0);
    g_pkey = settings_value(text, "panel_key=", KEY_KEYS, 18, 7);
    fgp_set_key(KEY_VK[g_pkey], KEY_NAMES[g_pkey]);
    g_tech = settings_value(text, "technology=", TECH_KEYS, 4, 0);
    fgb_set_tech(g_tech == 1 ? 2 : g_tech == 3 ? 3 : 1);
    int ver = !sf ? 5 : strstr(text, "version=5") ? 5 : strstr(text, "version=4") ? 4 : strstr(text, "version=3") ? 3 : strstr(text, "version=2") ? 2 : 1;
    if (ver < 4 && !g_motion_on) g_motion_on = 1;
    if (ver < 3 && g_sr == 1) g_sr = 2;
    g_sharp_auto = !strstr(text, "sharpness=") || (ver < 5 && g_sharp == 2);
    g_live = 1;
    g_motion = 1;
    log("upscaling add-on found: super resolution %s, frame generation %s, motion vectors %s, frame cap %s, preset %s, sharpness %s, V-Sync %s, technology %s (%ls)",
        SR_KEYS[g_sr], FG_KEYS[g_fg], MV_KEYS[g_motion_on], CAP_KEYS[g_fcap], PRESET_KEYS[g_preset], SHARP_KEYS[g_sharp], VSYNC_KEYS[g_vsync],
        TECH_KEYS[g_tech], sf ? L"settings.txt" : L"defaults");
    if (!sf || ver < 5) settings_save();
    return 1;
}

// code: LOW_BRIDGE_SIZE bytes below 2 GB. Bit 0 the stage hook, bit 1 the camera hook, bit 2 the record copies (all
// three), bit 3 the eight view classes are the expected ones; in bits 8.. the first failure's code of the two hooks.
int install_bridge64(uint8_t *code) {
    int r = 0, ok = 1;
    for (int i = 0; i < 8; i++) { const char *n = b64_class(B64_VT[i].vptr); ok &= n && !strcmp(n, B64_VT[i].mangled); }
    if (ok) r |= 8;
    if (!ok || !code) return r;
    int a = b64_entry(B64_PROCESS, 0x94, B64_PROCESS_FNV, code, (void *)b64_process, (void **)&b64_process_orig);
    int b = a == 1 ? b64_entry(B64_CAMERA, 0x40, B64_CAMERA_FNV, code + 0x30, (void *)b64_camera, (void **)&b64_camera_orig) : 0;
    int vp = !memcmp((const void *)(uintptr_t)B64_VIEWPROJ, B64_VP_PRO, 10) && fnv1a((const unsigned char *)(uintptr_t)B64_VIEWPROJ, 0x40) == B64_VIEWPROJ_FNV;
    r |= (a == 1) | (b == 1 && vp) << 1 | ((a != 1 ? a : b) & 0xff) << 8;
    if (a != 1 || b != 1 || !vp) return r;   // the stage hook without the camera hook is harmless: bp_enter only notes the stage
    // the record copies: every call must be E8 to memcpy@plt in functions whose bytes are the expected ones
    uint8_t *relay = code + 0x60;
    int copies = b64_near(relay, B64_COPY[0].call);
    for (int i = 0; i < 3 && copies; i++) {
        const uint8_t *c = (const uint8_t *)(uintptr_t)B64_COPY[i].call;
        int32_t rel; memcpy(&rel, c + 1, 4);
        copies = c[0] == 0xE8 && (int64_t)B64_COPY[i].call + 5 + rel == B64_MEMCPY_PLT &&
                 fnv1a((const unsigned char *)(uintptr_t)B64_COPY[i].fn, B64_COPY[i].len) == B64_COPY[i].fnv && !thread_inside(B64_COPY[i].call, 5);
    }
    if (!copies) return r;
    b64_relay(relay, (void *)fgm_copy64);
    for (int i = 0; i < 3 && copies; i++) {
        uint8_t j[5] = {0xE8};
        int32_t rel = (int32_t)((int64_t)(uintptr_t)relay - ((int64_t)B64_COPY[i].call + 5));
        memcpy(j + 1, &rel, 4);
        copies = patch_code((void *)(uintptr_t)B64_COPY[i].call, j, 5);
    }
    if (copies) r |= 4;
    return r;
}

ID3D11DeviceContext *ctxwrap_new(ID3D11DeviceContext *real);   // out/gen/ctxwrap64.c
extern ID3D11DeviceContextVtbl g_ctxwrap_vtbl;
static ID3D11DeviceContext *bridge64_device(ID3D11Device *dev, ID3D11DeviceContext *real);
static HRESULT STDMETHODCALLTYPE bridge64_swapchain(IDXGIFactory2 *f, IUnknown *dev, HWND wnd, const DXGI_SWAP_CHAIN_DESC1 *d,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *out, IDXGISwapChain1 **sc, d3d64_swap_fn real);
static HRESULT bridge64_present(IDXGISwapChain *sc, UINT sync, UINT flags, d3d64_present_fn real);

void bridge64_installed(int r) {
    b64_hooks = r;
    if (!g_live) return;
    static const char *const why[4] = {"a thread was inside its first bytes or no executable memory below 2 GB", "", "VirtualProtect failed", "the game's code differs"};
    if (!(r & 8)) { g_log("upscaling add-on: the game's render views are not the expected classes (another game build?) - the add-on stays off"); g_live = 0; return; }
    if ((r & 3) != 3) {
        int code = (signed char)(r >> 8);
        g_log("upscaling add-on: the %s hook is not in (%s) - the add-on stays off", r & 1 ? "camera" : "render stage",
              code == 0 ? why[3] : code == -1 ? why[0] : why[2]);
        g_live = 0;
        return;
    }
    g_copy64 = (r & 4) != 0;
    g_id_stage = 1; g_id_level = 2; g_id_scene = 3; g_id_view = 4; g_id_cam = 5;   // passtime's ids: only the stage's is used
    for (int i = 0; i < 8; i++) *B64_VT_SLOT[i] = B64_VT[i].vptr;
    g_d3d_wrap = bridge64_device;
    g_d3d_swap_hook = bridge64_swapchain;
    g_d3d_present_hook = bridge64_present;
    g_log("upscaling add-on: render stages through SCENE_VIEW::process, the main view's camera through set_shared_camera, %s; the "
          "game's DirectX 11 context goes through the add-on's own table (%u methods)",
          g_copy64 ? "unit and horse records through their three copies (per-object motion)" : "no per-object motion (the record copies are not the expected code)",
          ctxwrap_methods());
}

// ---- d3d64.c's hooks
// The loader's back buffer texture: the source of its CopyResource into the swap chain's buffer (dx_present)
static void(STDMETHODCALLTYPE *r_copyres)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
static void STDMETHODCALLTYPE w_copyres(ID3D11DeviceContext *c, ID3D11Resource *dst, ID3D11Resource *src) {
    r_copyres(c, dst, src);
    if (c != g_ctx || !g_sc || !src || src == g_game_bb || GetCurrentThreadId() != g_pt_main) return;
    ID3D11Resource *b = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(g_sc, 0, &iid_resource, (void **)&b)) || !b) return;
    if (b == dst) {
        if (g_game_bb) ID3D11Resource_Release(g_game_bb);
        g_game_bb = src;
        ID3D11Resource_AddRef(src);
        static int said;
        if (!said++) g_log("frame bridge: the game's back buffer found (the loader copies it into the swap chain at Present)");
    }
    ID3D11Resource_Release(b);
}

// D3D11CreateDevice: the context the loader gets is the wrapper; the first device only, as on 32-bit (a later one, after
// the loader closed the first, ends the bridge: that is also what a new swap chain does there)
static ID3D11DeviceContext *bridge64_device(ID3D11Device *dev, ID3D11DeviceContext *real) {
    if (!g_live) return NULL;
    if (g_ctx) {   // (a resolution change in the game's options: the loader makes a new device and swap chain)
        if (g_live_state == 1) { g_off_note = L"The game's graphics settings changed - restart the game to turn DLSS back on. Choices made here are saved."; bridge_off(); }
        g_log("frame bridge: the game made another DirectX 11 device - the add-on stays off on it until the game restarts%s",
              g_out_hwnd ? "; its swap chains go to a hidden window, the frames to the game's window through the add-on's own" : "");
        if (g_game_bb) { ID3D11Resource_Release(g_game_bb); g_game_bb = NULL; }   // the old device's
        g_live = 0;
        return NULL;
    }
    ID3D11DeviceContext *w = ctxwrap_new(real);
    if (!w) return NULL;
    g_dev = dev; ID3D11Device_AddRef(dev);
    g_ctx = w; ID3D11DeviceContext_AddRef(w);
    g_pt_main = GetCurrentThreadId();
    void **vt = (void **)&g_ctxwrap_vtbl;
    patch_slot(&vt[offsetof(ID3D11DeviceContextVtbl, CopyResource) / sizeof(void *)], (void *)w_copyres, (void **)&r_copyres);
    return w;
}

// CreateSwapChainForHwnd through the 32-bit w_create_sc: its DXGI_SWAP_CHAIN_DESC is made from the DESC1, and its call
// of the original comes back here (b64_create_hwnd) with the window and size it chose (the hidden window)
static d3d64_swap_fn b64_sc_real;
static DXGI_SWAP_CHAIN_DESC1 b64_d1;
static const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *b64_fs;
static IDXGIOutput *b64_out;
// (also the fallback's own swap chain, fgb_fallback_frame, whose factory comes as IDXGIFactory: asked for IDXGIFactory2)
static HRESULT STDMETHODCALLTYPE b64_create_hwnd(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out) {
    DXGI_SWAP_CHAIN_DESC1 d1 = b64_d1;
    d1.Width = d->BufferDesc.Width; d1.Height = d->BufferDesc.Height;
    d1.Format = d->BufferDesc.Format; d1.BufferCount = d->BufferCount; d1.SwapEffect = d->SwapEffect;
    IDXGIFactory2 *f2 = NULL;
    if (FAILED(IDXGIFactory_QueryInterface(f, &IID_IDXGIFactory2, (void **)&f2)) || !f2) return E_NOINTERFACE;
    HRESULT hr = b64_sc_real(f2, dev, d->OutputWindow, &d1, d->Windowed ? NULL : b64_fs, b64_out, (IDXGISwapChain1 **)(void *)out);
    IDXGIFactory2_Release(f2);
    return hr;
}
// Through w_create_sc: the first device's swap chain (the bridge starts there), and every later one of the game's once
// the bridge has run on its window (g_out_hwnd): a flip swap chain of the helper's has been on that window, so no other
// one may be made there (DXGI: 0x80070005, the game's resolution change failed, user report 2026-11-22). w_create_sc
// puts them on a hidden window and the fallback shows their frames through a child window of its own, as on 32-bit.
static HRESULT STDMETHODCALLTYPE bridge64_swapchain(IDXGIFactory2 *f, IUnknown *dev, HWND wnd, const DXGI_SWAP_CHAIN_DESC1 *d,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *out, IDXGISwapChain1 **sc, d3d64_swap_fn real) {
    if (!d || (!(g_live && (void *)dev == (void *)g_dev) && !g_out_hwnd)) return real(f, dev, wnd, d, fs, out, sc);
    b64_sc_real = real; b64_d1 = *d; b64_fs = fs; b64_out = out;
    r_create_sc = b64_create_hwnd;
    g_create_real = b64_create_hwnd;
    DXGI_SWAP_CHAIN_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.BufferDesc.Width = d->Width; dd.BufferDesc.Height = d->Height; dd.BufferDesc.Format = d->Format;
    dd.SampleDesc = d->SampleDesc; dd.BufferUsage = d->BufferUsage; dd.BufferCount = d->BufferCount;
    dd.OutputWindow = wnd; dd.Windowed = !fs || fs->Windowed; dd.SwapEffect = d->SwapEffect; dd.Flags = d->Flags;
    return w_create_sc((IDXGIFactory *)(void *)f, dev, &dd, (IDXGISwapChain **)(void *)sc);
}

// Present of the game's swap chain, after d3d64.c's frame point: the 32-bit frame end (bp_frame, passtime's), then its
// Present hook; the 30 s report as heapfix.c called it
static HRESULT bridge64_present(IDXGISwapChain *sc, UINT sync, UINT flags, d3d64_present_fn real) {
    if (!g_present_real) g_present_real = real;
    if (flags & DXGI_PRESENT_TEST) return real(sc, sync, flags);
    bp_frame();
    static ULONGLONG next;
    ULONGLONG now = GetTickCount64();
    if (!next) next = now + 30000;
    else if (now >= next) {
        next = now + 30000;
        bp_report(g_log);
        if (!b64_stages_seen) g_log("frame bridge: no render stage seen on the drawing thread yet");
    }
    return w_present(sc, sync, flags);
}
