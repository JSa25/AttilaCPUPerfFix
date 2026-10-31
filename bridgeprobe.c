// Dev-only (passtime_on.txt containing "bridge"): finds the four places the DLSS frame bridge needs (DLSS_PLAN,
// AGENT_LOG 93/94). Looking only: nothing that is drawn changes.
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
int fgp_move_device(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT bw, UINT bh);
IDXGISwapChain *fgb_fallback_sc(void);
HWND fgb_fallback_window(void);
void *fgb_vt_original(void **vt, size_t slot);
void *fgb_unhooked(void *fn);
void fgb_set_sr(int on, UINT render_w, UINT render_h);
void fgb_jitter(float *jx, float *jy);
static void jit_frame_start(void), jit_frame_end(void);   // DLAA viewport jitter (below)
// per-object motion (fgmotion.c, AGENT_LOG 99): fgbridge_on.txt line 4 contains "motion"
static int g_motion;              // the motion pass is set up (bridge mode); on / off: g_motion_on and the F11 panel
static int g_motion_on;
static int g_copy_patched;         // the game's instance record copy calls fgm_copy (stays patched for the session)
// the F11 panel (fgpanel.c)
typedef struct { void (*sr)(int mode); void (*fg)(int frames); void (*motion)(int on); void (*cap)(int choice); void (*preset)(int choice); void (*sharp)(int choice); void (*view)(int choice); void (*vsync)(int on); } FgpApply;
void fgp_set_cap(int choice, unsigned fps);
void fgp_set_preset(int choice);
void fgp_set_sharp(int choice);
void fgp_set_presets_available(int on);
void fgb_set_preset(unsigned preset);
int fgb_helper_has_presets(void);
void fgb_set_vsync(int on);
int fgb_helper_has_vsync(void);
void fgp_set_vsync(int on);
void fgp_set_vsync_available(int on);
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
void fgm_set_enabled(int on);
static HWND g_out_hwnd;           // the game's window (the helper presents into it)
static int g_sr;                 // DLAA (defined with the jitter below)
static void jit_install(void);
void fgb_set_motion(void (*pass)(ID3D11Resource *depth, float jx, float jy));
ID3D11Texture2D *fgb_mv_texture(void);
int fgb_make_vectors(ID3D11Resource *depth, const float vp[16]);
static ID3D11Resource *g_main_depth;   // the scene's depth buffer, seen at the HUD-less point
static unsigned g_mvx_calls, g_mvx_frames, g_mvx_none, g_mvx_last = ~0u;   // fgb_motion_vectors(): other mods' calls
int fgm_init(ID3D11Device *, ID3D11DeviceContext *, ID3D11Texture2D *mv, UINT w, UINT h, void (*log)(const char *fmt, ...));
void fgm_layout_created(const void *il, const D3D11_INPUT_ELEMENT_DESC *e, UINT n, unsigned long long sig);
void fgm_mapped(ID3D11Resource *r, D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE *m);
void *__cdecl fgm_copy(void *dst, const void *src, size_t n);
void fgm_draw(ID3D11DeviceContext *c, UINT idx, UINT inst, UINT start_idx, INT base_v, UINT start_inst);
void fgm_pass(ID3D11Resource *depth, float jx, float jy);
void fgm_frame_end(void);
void fgm_report(void (*log)(const char *fmt, ...));
void fgm_dump_next(const wchar_t *path);
static void mp_capture_start(void), mp_capture_end(FILE *f), mp_hook(const char *where);   // motion probe (below)
static int g_live, g_live_state;   // state: 0 not started, 1 running, 2 off for good
static wchar_t g_helper[MAX_PATH], g_sl[MAX_PATH], g_logdir[MAX_PATH];
static wchar_t g_settings[MAX_PATH];   // the add-on's settings.txt (the F11 panel saves to it); empty in dev runs
static unsigned g_fg, g_live_w, g_live_h, g_vp_frame = ~0u, g_hud_frame = ~0u, g_br_hud, g_br_novp, g_br_nodepth, g_br_native;
// Loading screens and movies have no 3D scene (no HUD-less point): they must not be shrunk for DLSS, or they show at the
// render size. A frame is shrunk only when the previous one had a scene; a scene frame that was not shrunk (the first
// after a loading screen) is shown at native size, without DLSS for that frame.
static int g_hud_this_frame, g_scene_last, g_frame_scaled;
static float g_vp[16];

static void (*g_log)(const char *fmt, ...);

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
// gave_up: fn could not be gone past; found once, not again with a file read and a log line every frame.
static void *unhooked(void *fn, void **cache, void **gave_up, const char *name) {
    if (*cache && *cache != fn) return *cache;
    if (*gave_up == fn) return fn;
    void *u = fgb_unhooked(fn);
    if (u && u != fn) g_log("frame bridge: DXGI's %s starts with a jump (another mod hooked it there) - the mod's own calls go past it", name);
    if (!u) {
        g_log("frame bridge: DXGI's %s starts with a jump the mod cannot go past - it calls the hooked function", name);
        *gave_up = fn;
    }
    return *cache = u ? u : fn;
}
static HRESULT STDMETHODCALLTYPE present_body(IDXGISwapChain *sc, UINT sync, UINT flags) {
    static void *cache, *gave_up;
    return ((HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT))unhooked((void *)g_present_real, &cache, &gave_up, "Present"))(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE create_body(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out) {
    static void *cache, *gave_up;
    return ((HRESULT(STDMETHODCALLTYPE *)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **))unhooked(
        (void *)g_create_real, &cache, &gave_up, "CreateSwapChain"))(f, dev, d, out);
}   // the game's swapchain on the hidden window (w_create_sc), until the game makes another

// capture
static int g_armed, g_capturing, g_captures, g_camera_left, g_dev_from_sc, g_creates;
static unsigned g_frame, g_scene_no, g_dumps;
static FILE *g_cap, *g_cam;
static ID3D11Texture2D *g_staging;
static const void *g_view_obj[16];
static unsigned g_view_sp;
static struct { unsigned id; const void *obj; } g_stk[64];
static unsigned g_sp;

static const GUID iid_resource = {0xdc8e63f3, 0xd12b, 0x4952, {0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d}};   // ID3D11Resource

static int in_game(unsigned a) { return a >= g_pt_base + 0x1000 && a < g_pt_base + g_mod_size; }

// the first return address inside empire.retail.dll above this frame (the UI mod may sit between the game and us)
static unsigned game_caller(const unsigned *sp) {
    for (int i = 0; i < 256; i++) {
        MEMORY_BASIC_INFORMATION m;
        if (i % 64 == 0 && (!VirtualQuery(sp + i, &m, sizeof m) || m.State != MEM_COMMIT)) return 0;
        if (in_game(sp[i])) return sp[i] - g_pt_base;
    }
    return 0;
}

static ID3D11Resource *backbuffer(void) {
    ID3D11Resource *bb = NULL;
    if (g_sc) IDXGISwapChain_GetBuffer(g_sc, 0, &iid_resource, (void **)&bb);
    return bb;
}

static void desc_of(ID3D11View *v, char *out, size_t len, ID3D11Resource *bb) {
    if (!v) { snprintf(out, len, "-"); return; }
    ID3D11Resource *r;
    ID3D11View_GetResource(v, &r);
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
        snprintf(out, len, "%s%ux%u f%u b%x a%u m%u @%p", r == bb ? "BACKBUFFER " : "", d.Width, d.Height, d.Format, d.BindFlags,
                 d.ArraySize, d.MipLevels, (void *)r);
    } else snprintf(out, len, "dim%d @%p", dim, (void *)r);
    ID3D11Resource_Release(r);
}

// targets bound right now: returns 1 if the back buffer is render target 0
static int targets(char *out, size_t len) {
    ID3D11RenderTargetView *rtv[2] = {0};
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 2, rtv, &dsv);
    ID3D11Resource *bb = backbuffer(), *r0 = NULL;
    char a[160], b[160], c[160];
    desc_of((ID3D11View *)rtv[0], a, sizeof a, bb);
    desc_of((ID3D11View *)rtv[1], b, sizeof b, bb);
    desc_of((ID3D11View *)dsv, c, sizeof c, bb);
    snprintf(out, len, "rt0 [%s] rt1 [%s] depth [%s]", a, b, c);
    if (rtv[0]) ID3D11RenderTargetView_GetResource(rtv[0], &r0);
    int is_bb = r0 && r0 == bb;
    if (r0) ID3D11Resource_Release(r0);
    for (int i = 0; i < 2; i++) if (rtv[i]) ID3D11RenderTargetView_Release(rtv[i]);
    if (dsv) ID3D11DepthStencilView_Release(dsv);
    if (bb) ID3D11Resource_Release(bb);
    return is_bb;
}

// the stage vtables the bridge tells apart (main 3D view, HUD, overlay), this game build's rvas (set at install)
static unsigned g_vt_main = 0x1CCE4E4, g_vt_hud = 0x1C9A900, g_vt_overlay = 0x1CA0858;
// half-size BGRA/RGBA dump of the back buffer: bridge_bb_<capture>_<k>.raw (header: w, h, format)
static void dump_bb(const char *where) {
    if (g_dumps >= 24) return;
    ID3D11Resource *bb = backbuffer();
    if (!bb) return;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)bb, &d);
    if (!g_staging) {
        D3D11_TEXTURE2D_DESC s = d;
        s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
        s.SampleDesc.Count = 1; s.SampleDesc.Quality = 0; s.MipLevels = 1; s.ArraySize = 1;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &s, NULL, &g_staging))) { ID3D11Resource_Release(bb); return; }
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_staging, bb);
    ID3D11Resource_Release(bb);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_staging, 0, D3D11_MAP_READ, 0, &m))) return;
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\bridge_bb_%d_%02u.raw", g_dir, g_captures, g_dumps);
    FILE *f = _wfopen(p, L"wb");
    if (f) {
        unsigned hw = d.Width / 2, hh = d.Height / 2, hdr[3] = {hw, hh, d.Format};
        fwrite(hdr, 4, 3, f);
        static unsigned row[4096];
        for (unsigned y = 0; y < hh && hw <= 4096; y++) {
            const unsigned *src = (const unsigned *)((const unsigned char *)m.pData + (size_t)y * 2 * m.RowPitch);
            for (unsigned x = 0; x < hw; x++) row[x] = src[x * 2];
            fwrite(row, 4, hw, f);
        }
        fclose(f);
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_staging, 0);
    fprintf(g_cap, "    -> back buffer dump %02u (%s)\n", g_dumps, where);
    g_dumps++;
}

__attribute__((force_align_arg_pointer)) static void bp_enter(unsigned id, const void *obj) {
    if (GetCurrentThreadId() != g_pt_main) return;
    if (g_sp < 64) { g_stk[g_sp].id = id; g_stk[g_sp].obj = obj; }
    g_sp++;
    if (id == g_id_view && g_view_sp < 16) g_view_obj[g_view_sp] = obj;
    if (id == g_id_cam && g_pt_args) {   // FUN_1177bb10(camera block, stage): every camera the renderer is given
        const unsigned char *cam = (const unsigned char *)(UINT_PTR)g_pt_args[0];
        const unsigned *stage = (const unsigned *)(UINT_PTR)g_pt_args[1];
        unsigned vt = stage ? stage[0] - g_pt_base : 0, caller = g_pt_args[-1] - g_pt_base;
        MEMORY_BASIC_INFORMATION mb;
        int ok = cam && VirtualQuery(cam, &mb, sizeof mb) && mb.State == MEM_COMMIT &&
                 (const unsigned char *)mb.BaseAddress + mb.RegionSize >= cam + 0x200;
        if (g_live && ok && vt == g_vt_main) { memcpy(g_vp, cam + 0x1B0, 64); g_vp_frame = g_frame; }
        if (g_camera_left && g_cam && ok) {
            unsigned rec[4] = {g_frame | 0x80000000u, vt, caller, 0x200};   // type 2 record: one camera block
            fwrite(rec, 4, 4, g_cam);
            fwrite(cam, 1, 0x200, g_cam);
        }
        if (g_capturing) fprintf(g_cap, "%*sset camera: stage vtable +0x%X, camera @%p, called from +0x%X\n", (int)(g_sp - 1) * 2, "",
                                 vt, (const void *)cam, caller);
        return;
    }
    if (id == g_id_view) g_view_sp++;
    if (id == g_id_stage && (g_capturing || g_motion)) mp_hook(g_capturing ? "stage entry" : NULL);
    if (id == g_id_stage && g_live_state == 1) jit_install();
    unsigned vt = obj ? *(const unsigned *)obj - g_pt_base : 0;
    if (g_live_state == 1 && id == g_id_stage && vt == g_vt_hud && g_hud_frame != g_frame && fgb_active()) {
        g_hud_frame = g_frame;
        jit_frame_end();
        ID3D11DepthStencilView *dsv = NULL;
        ID3D11Resource *depth = NULL, *bb = backbuffer();
        ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 0, NULL, &dsv);
        if (dsv) { ID3D11DepthStencilView_GetResource(dsv, &depth); ID3D11DepthStencilView_Release(dsv); }
        if (depth && depth != g_main_depth) {   // for fgb_motion_vectors(), called before this point in later frames
            if (g_main_depth) ID3D11Resource_Release(g_main_depth);
            g_main_depth = depth;
            ID3D11Resource_AddRef(depth);
        }
        // ReShade: its effects before the HUD-less copy (frame generation's two images both have them), or with DLSS SR
        // on DLSS's output after the round trip (full size, the depth back at full size too), then handed to the helper
        // as frame generation's HUD-less image
        int sr_trip = g_sr && g_frame_scaled && bb && depth && g_vp_frame == g_frame;   // DLSS SR runs in this frame's fgb_hudless
        if (g_rs && bb && !sr_trip) g_rs_frames += rsl_effects(g_ctx, bb, depth);
        if (!depth) g_br_nodepth++;
        else if (g_vp_frame != g_frame) g_br_novp++;
        else if (bb) {
            g_hud_this_frame = 1;
            if (!g_sr || g_frame_scaled) { fgb_hudless(bb, depth, g_vp); g_br_hud++; }
            else g_br_native++;
        }
        if (g_rs && sr_trip && rsl_effects(g_ctx, bb, depth)) { g_rs_frames++; fgb_hud_post(bb); }
        if (g_rs && g_vp_frame == g_frame) rsl_camera(g_vp);
        if (depth) ID3D11Resource_Release(depth);
        if (bb) ID3D11Resource_Release(bb);
    }
    if (!g_capturing || !g_ctx) return;
    char t[600];
    targets(t, sizeof t);
    if (id == g_id_stage) fprintf(g_cap, "%*senter stage vtable +0x%X: %s\n", (int)(g_sp - 1) * 2, "", vt, t);
    else if (id == g_id_scene) fprintf(g_cap, "%*senter scene call %u: %s\n", (int)(g_sp - 1) * 2, "", g_scene_no, t);
    else if (id == g_id_view) fprintf(g_cap, "%*senter camera loop (stage vtable +0x%X, %u blocks)\n", (int)(g_sp - 1) * 2, "", vt,
                                      obj ? *(const unsigned *)((const char *)obj + 0x98) : 0);
    else if (id == g_id_level) fprintf(g_cap, "%*senter render level: %s\n", (int)(g_sp - 1) * 2, "", t);
}

__attribute__((force_align_arg_pointer)) static void bp_exit(unsigned id) {
    if (GetCurrentThreadId() != g_pt_main) return;
    if (g_sp) g_sp--;
    if (id == g_id_view && g_view_sp) {
        g_view_sp--;
        const unsigned char *o = g_view_sp < 16 ? g_view_obj[g_view_sp] : NULL;
        if (o && g_camera_left && g_cam) {   // blocks are updated inside the loop: read them after it
            unsigned n = *(const unsigned *)(o + 0x98), rec[4] = {g_frame, *(const unsigned *)o - g_pt_base, n, 0x1C8};
            if (n <= 16) {
                fwrite(rec, 4, 4, g_cam);
                fwrite(o + 0x294, 0x1C8, n, g_cam);
                fwrite(o + 0xD4, 16, n, g_cam);   // the sub-viewport rectangles
            }
        }
    }
    if (id == g_id_scene) g_scene_no++;
    if (!g_capturing || !g_ctx) return;
    char t[600];
    int bb = targets(t, sizeof t);
    unsigned vt = g_sp < 64 && g_stk[g_sp].obj ? *(const unsigned *)g_stk[g_sp].obj - g_pt_base : 0;
    if (id == g_id_cam) return;
    const char *what = id == g_id_stage ? "stage" : id == g_id_scene ? "scene call" : id == g_id_view ? "camera loop" : "render level";
    fprintf(g_cap, "%*sexit %s (vtable +0x%X): %s\n", (int)g_sp * 2, "", what, vt, t);
    if (bb && id == g_id_stage) {
        char w[64];
        snprintf(w, sizeof w, "after stage vtable +0x%X", vt);
        dump_bb(w);
    }
}

__attribute__((force_align_arg_pointer)) static void bp_frame(void) {
    g_frame++;
    g_scene_no = 0;
    if (g_capturing) {   // the captured frame is over
        g_capturing = 0;
        fprintf(g_cap, "== end of captured frame\n");
        mp_capture_end(g_cap);
        fclose(g_cap);
        g_cap = NULL;
    }
    if (g_camera_left && !--g_camera_left && g_cam) { fclose(g_cam); g_cam = NULL; }
    if (g_armed && g_sc) {   // a capture uses the device of the game's swapchain (whatever other devices were made since)
        static const GUID iid_dev = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};   // ID3D11Device
        ID3D11Device *d = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetDevice(g_sc, &iid_dev, (void **)&d))) {
            if (d != g_dev) {
                if (g_ctx) ID3D11DeviceContext_Release(g_ctx);
                if (g_dev) ID3D11Device_Release(g_dev);
                if (g_staging) ID3D11Texture2D_Release(g_staging);
                g_staging = NULL;
                g_dev = d;
                ID3D11Device_GetImmediateContext(g_dev, &g_ctx);
                g_dev_from_sc = 1;
            } else ID3D11Device_Release(d);
        }
    }
    if (g_armed && g_ctx && !g_camera_left) {
        g_armed = 0;
        g_captures++;
        wchar_t p[MAX_PATH];
        _snwprintf(p, MAX_PATH, L"%ls\\bridge_capture_%d.txt", g_dir, g_captures);
        g_cap = _wfopen(p, L"w");
        _snwprintf(p, MAX_PATH, L"%ls\\bridge_camera_%d.bin", g_dir, g_captures);
        g_cam = _wfopen(p, L"wb");
        if (g_cap) {
            g_capturing = 1;
            g_dumps = 0;
            mp_capture_start();
            if (g_motion) {   // and a picture of the motion vectors after this frame's motion pass
                _snwprintf(p, MAX_PATH, L"%ls\\motion_mv_%d.raw", g_dir, g_captures);
                fgm_dump_next(p);
            }
            g_camera_left = 31;
            fprintf(g_cap, "== bridge capture %d, frame %u; swapchain %ux%u format %u buffers %u swap effect %u windowed %d\n",
                    g_captures, g_frame, g_scd.BufferDesc.Width, g_scd.BufferDesc.Height, g_scd.BufferDesc.Format, g_scd.BufferCount,
                    g_scd.SwapEffect, g_scd.Windowed);
        }
    }
    if ((g_frame & 31) == 0) {   // bridge_go.txt (made when the user says "ready") arms the next capture
        wchar_t p[MAX_PATH];
        _snwprintf(p, MAX_PATH, L"%ls\\bridge_go.txt", g_dir);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES && DeleteFileW(p)) g_armed = 1;
    }
}

// the bridge off for the session: the helper stops, and the game's own viewports and camera block come back (a preset
// below native size left them scaled for good, with nothing upscaling the corner)
static void bridge_off(void) {
    fgb_stop();
    g_live_state = 2;
    jit_frame_end();
    if (g_motion) {   // the motion pass ends too: nothing would end its frames, and its draws hold the game's buffers
        fgm_set_enabled(0);
        fgm_frame_end();
        g_motion = 0;
    }
}

// ---- what the fallback does in game (6.3.2 test: the picture froze although the game kept presenting)
static unsigned g_fb_frames, g_fb_notok, g_nested[3];
static HRESULT g_fb_last;
// "module+0x..." for the first n values on the stack above sp that point into a loaded module (likely return addresses)
static void stack_text(char *out, int cap, const unsigned *sp, int n) {
    int len = 0;
    out[0] = 0;
    for (int i = 0; i < 2048 && n > 0 && len < cap - 80; i++) {
        MEMORY_BASIC_INFORMATION mb;
        HMODULE m = NULL;
        char name[MAX_PATH];
        if (!VirtualQuery(sp + i, &mb, sizeof mb) || mb.State != MEM_COMMIT) break;
        if (sp[i] < 0x10000 || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                   (LPCSTR)(UINT_PTR)sp[i], &m) || !GetModuleFileNameA(m, name, MAX_PATH))
            continue;
        const char *b = strrchr(name, '\\');
        len += snprintf(out + len, cap - len, " %s+0x%X", b ? b + 1 : name, sp[i] - (unsigned)(UINT_PTR)m);
        n--;
    }
}
static const char *module_name(const void *p, char *buf) {
    HMODULE m = NULL;
    strcpy(buf, "?");
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m))
        GetModuleFileNameA(m, buf, MAX_PATH);
    const char *b = strrchr(buf, '\\');
    return b ? b + 1 : buf;
}
static void fb_report_start(void) {
    char a[MAX_PATH], c[MAX_PATH];
    const unsigned char *p = (const unsigned char *)g_present_real;
    const void *jump = p[0] == 0xE9 ? (const void *)(p + 5 + *(const int *)(p + 1)) : p[0] == 0xFF && p[1] == 0x25 ? **(const void *const *const *)(p + 2) : NULL;
    HWND w = fgb_fallback_window();
    RECT r = {0}, pr = {0};
    if (w) GetWindowRect(w, &r);
    GetWindowRect(g_out_hwnd, &pr);
    g_log("frame bridge fallback: game window thread %lu, Present thread %lu, style %08lX ex %08lX, rect %ld,%ld-%ld,%ld; mod's window %s "
          "rect %ld,%ld-%ld,%ld; DXGI Present (%s) starts %02X %02X %02X %02X %02X%s%s",
          GetWindowThreadProcessId(g_out_hwnd, NULL), GetCurrentThreadId(), (unsigned long)GetWindowLongW(g_out_hwnd, GWL_STYLE),
          (unsigned long)GetWindowLongW(g_out_hwnd, GWL_EXSTYLE), pr.left, pr.top, pr.right, pr.bottom, w && IsWindowVisible(w) ? "visible" : "NOT visible",
          r.left, r.top, r.right, r.bottom, module_name(p, a), p[0], p[1], p[2], p[3], p[4], jump ? " - a jump to " : "", jump ? module_name(jump, c) : "");
}

// The F11 panel after the bridge (user: keep DLSS off after a settings change, but F11 still opens the panel, with a
// note to restart): drawn over the game's frame before the mod shows it, on that frame's device. Choices are saved for
// the next start.
static const wchar_t *g_off_note = L"DLSS is off - restart the game to turn it back on. Choices made here are saved.";
static void fallback_panel(ID3D11Resource *bb) {
    static ID3D11Device *pdev;
    static int ok;
    ID3D11Device *d = NULL;
    ID3D11Resource_GetDevice(bb, &d);
    if (!d) return;
    if (d != pdev) {
        ID3D11DeviceContext *c = NULL;
        D3D11_TEXTURE2D_DESC td;
        ID3D11Device_GetImmediateContext(d, &c);
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)bb, &td);
        ok = c && fgp_move_device(d, c, td.Width, td.Height);   // the panel keeps these references
        fgp_set_note(g_off_note);
        if (pdev) ID3D11Device_Release(pdev);
        pdev = d;   // GetDevice's reference
        g_log("F11 panel: %s over the frames the mod shows (DLSS off until the game restarts)", ok ? "available" : "NOT available");
    } else
        ID3D11Device_Release(d);
    if (ok) fgp_frame(bb, g_out_hwnd);
}

static HRESULT present_hooked(IDXGISwapChain *sc, UINT sync, UINT flags, unsigned ret, const unsigned *frame) {
    g_presents++;
    if (GetCurrentThreadId() != g_pt_main) g_present_other_thread++;
    g_sync[sync < 4 ? sync : 4]++;
    unsigned game = game_caller(frame);
    for (int i = 0; i < 8; i++) {
        if (g_callers[i].n && (g_callers[i].ret != ret || g_callers[i].game_ret != game)) continue;
        g_callers[i].ret = ret; g_callers[i].game_ret = game; g_callers[i].n++;
        break;
    }
    int game_sc = sc == g_sc || (g_rs_sc && sc == g_rs_sc);   // with ReShade: the real swapchain under its wrapper
    if (g_capturing && game_sc) {
        char t[600];
        targets(t, sizeof t);
        fprintf(g_cap, "Present(sync %u, flags 0x%x), called from %p (game +0x%X): %s\n", sync, flags, (void *)(UINT_PTR)ret, game, t);
        dump_bb("at Present");
    }
    if (g_live_state == 1 && game_sc) {   // started when the swapchain was created (w_create_sc)
        DXGI_SWAP_CHAIN_DESC d;
        ID3D11Resource *bb = backbuffer();
        if (bb && SUCCEEDED(IDXGISwapChain_GetDesc(sc, &d))) {
            if (d.BufferDesc.Width != g_live_w || d.BufferDesc.Height != g_live_h || !fgb_active()) {
                // shortcut: a resize (or a helper that died) ends the bridge for the session; restart the game to get it back
                g_log("frame bridge: back buffer %ux%u (was %ux%u), helper %s - bridge off, game presents again", d.BufferDesc.Width,
                      d.BufferDesc.Height, g_live_w, g_live_h, fgb_active() ? "running" : "gone");
                g_off_note = fgb_active() ? L"The game's size changed - restart the game to turn DLSS back on. Choices made here are saved."
                                          : L"DLSS stopped (its helper closed) - restart the game to turn it back on. Choices made here are saved.";
                bridge_off();
            }
            if (g_live_state == 1) {
                jit_frame_end();   // a frame without a HUD-less point
                {   // frame rates for the panel: the game's frames and the frames shown (generated ones included)
                    static LARGE_INTEGER t0, qf;
                    static unsigned frames, shown0;
                    LARGE_INTEGER t;
                    QueryPerformanceCounter(&t);
                    if (!qf.QuadPart) { QueryPerformanceFrequency(&qf); t0 = t; shown0 = fgb_shown(); }
                    frames++;
                    double dt = (double)(t.QuadPart - t0.QuadPart) / qf.QuadPart;
                    if (dt >= 0.5) {
                        unsigned sh = fgb_shown();
                        fgp_set_fps(frames / dt, (sh - shown0) / dt);
                        frames = 0; t0 = t; shown0 = sh;
                    }
                }
                fgp_frame(bb, g_out_hwnd);
                fgb_present(bb);
                // the game's own swapchain is presented too (hidden window, sync 0, past the overlay / UI mod hooks):
                // without its Presents the NVIDIA driver held the game's main thread ~30-40% of each frame on a battle
                // clump, the GPU half idle (AGENT_LOG 109: 120-135 fps without the add-on, 75 with it; 155 with this)
                present_body(sc, 0, 0);
                if (g_motion) { fgm_frame_end(); mp_hook(NULL); }   // hooks back before the next frame's instance copies
                ID3D11Resource_Release(bb);
                g_scene_last = g_hud_this_frame;
                g_hud_this_frame = 0;
                jit_frame_start();
                return S_OK;
            }
        }
        if (bb) ID3D11Resource_Release(bb);
    }
    if ((sc == g_hid_sc || (g_rs_sc && sc == g_rs_sc && g_hid_sc)) && g_live_state != 1) {   // the bridge ended, the game still presents on the hidden window: its
        ID3D11Resource *bb = NULL;               // frames go to the real window through the mod's own swapchain
        IDXGISwapChain *own = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetBuffer(sc, 0, &iid_resource, (void **)&bb))) {
            fallback_panel(bb);
            own = fgb_fallback_frame(bb, create_body);
        }
        if (bb) ID3D11Resource_Release(bb);
        if (own) {
            // with ReShade the mod's swapchain is ReShade's wrapper too (made through its factory): its own Present, which
            // comes back through the real one (the nested-Present path, expected here); DXGI's on the wrapper would crash
            HRESULT h;
            if (g_rs_sc) { g_rs_fb = 1; h = IDXGISwapChain_Present(own, sync, flags); g_rs_fb = 0; }
            else h = present_body(own, sync, flags);
            if (!g_fb_frames++) fb_report_start();
            if (h != S_OK && !g_fb_notok++ && h == DXGI_ERROR_DEVICE_REMOVED) {   // why, once
                static const GUID iid_d11 = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};
                ID3D11Device *d = NULL;
                if (SUCCEEDED(IDXGISwapChain_GetDevice(own, &iid_d11, (void **)&d))) {
                    g_log("frame bridge fallback: the D3D11 device was removed (reason 0x%08lX)", (unsigned long)ID3D11Device_GetDeviceRemovedReason(d));
                    ID3D11Device_Release(d);
                }
            }
            if (h != S_OK) g_fb_last = h;
            return h;
        }
    }
    return g_present_real(sc, sync, flags);
}

// A Present inside a Present on the same thread is skipped. 6.3.2, battle loading + alt-tab: the Steam overlay's Present
// hook, while running, got a window message to the UI mod, which presented again through ours, which called on into the
// overlay's hook, and so on until the stack overflowed (heapfix.log stack: GameOverlayRenderer -> dinput8 w_present ->
// version_v201a -> ... repeating). Nothing needs a nested Present; skipping it ends any such loop that passes through us.
static HRESULT STDMETHODCALLTYPE w_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    static __thread int depth;
    static LONG nested;
    unsigned ret = (unsigned)(UINT_PTR)__builtin_return_address(0);
    if (depth && g_rs_fb) return present_body(sc, sync, flags);   // the fallback's Present through ReShade's wrapper
    if (depth) {
        int kind = sc == g_hid_sc ? 0 : sc && sc == fgb_fallback_sc() ? 1 : 2;
        if (!g_nested[kind]++) {   // each kind once, with the stack: who presented inside whose Present
            char st[1200];
            stack_text(st, sizeof st, (const unsigned *)__builtin_frame_address(0), 24);
            g_log("frame bridge: nested Present of %s swapchain, stack:%s", kind == 0 ? "the game's" : kind == 1 ? "the mod's own" : "another", st);
        }
        if (InterlockedIncrement(&nested) == 1) {   // presented once, past the hooks that led back here
            char at[MAX_PATH] = "?";
            HMODULE m = NULL;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(UINT_PTR)ret, &m))
                GetModuleFileNameA(m, at, MAX_PATH);
            const char *b = strrchr(at, '\\');
            g_log("frame bridge: Present called again while a Present was running (from %s+0x%X) - it goes straight to DXGI (only logged once)",
                  b ? b + 1 : at, (unsigned)(ret - (unsigned)(UINT_PTR)m));
        }
        return present_body(sc, sync, flags);
    }
    depth = 1;
    HRESULT hr = present_hooked(sc, sync, flags, ret, (const unsigned *)__builtin_frame_address(0));
    depth = 0;
    return hr;
}

static void patch_slot(void **slot, void *to, void **old) {
    DWORD prot;
    if (*slot == to || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &prot)) return;
    *old = *slot;
    *slot = to;
    VirtualProtect(slot, sizeof *slot, prot, &prot);
}
// DXGI factory / swapchain slots: hooked once. Another mod (the 4K UI mod hooks Present) may have put its own hook in
// the slot after ours, calling ours as its original; hooking again would make ours call it back, forever (6.3: a new
// swapchain, e.g. switching to fullscreen in game, overflowed the stack). All objects of a class share the vtable.
static void patch_once(void **slot, void *to, void **old) {
    if (!*old) patch_slot(slot, to, old);
}

// saved (what the slot held when hooked) if it is DXGI's own function, else DXGI's original from the file
static void *dxgi_own(void **vt, size_t slot, void *saved, const char *name) {
    HMODULE m = NULL, sm = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)vt, &m);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)saved, &sm);
    if (m && sm == m) return saved;
    void *orig = fgb_vt_original(vt, slot);
    char who[MAX_PATH] = "?";
    if (sm) GetModuleFileNameA(sm, who, MAX_PATH);
    const char *bn = strrchr(who, '\\');
    g_log("frame bridge: %s was already hooked by %s when the mod hooked it - the mod calls %s", name, bn ? bn + 1 : who,
          orig ? "DXGI's own (read from the DLL file)" : "that hook (DXGI's own not found)");
    return orig ? orig : saved;
}

// The helper's flip-model swapchain needs the game's window to itself (in game, Streamline's CreateSwapChainForHwnd got
// "Access is denied" next to the game's swapchain; guess: Windows 11 turns windowed blit swapchains into flip ones). So the
// bridge starts here, before the game's swapchain exists, and the game's swapchain goes to a hidden window of the same size.
static HWND hidden_window(UINT w, UINT h) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"fgb_hidden";
    RegisterClassW(&wc);
    return CreateWindowExW(0, L"fgb_hidden", L"", WS_POPUP, 0, 0, w, h, NULL, NULL, wc.hInstance, NULL);
}

// ---- DLSS at native size (DLAA, AGENT_LOG 96): Attila's camera matrix is rebuilt from position / look-at / FOV before the
// shaders get it (FUN_117864c0 never reads +0x1B0), so the sub-pixel jitter goes into the viewport instead: a viewport
// moved by (jx, jy) shifts everything rasterised exactly like a jittered projection. Jittered: viewports of the back
// buffer's size while a depth buffer of that size is bound and depth testing is on, from the frame start to the HUD-less
// point (depth pre-pass, G-buffer, forward passes: they must line up with each other). Not jittered: shadow maps, half-
// size passes, full-screen passes without depth test, post-processing, the UI.
static int g_sr;                         // 0 off, 1 DLAA, 2..5 Quality..Ultra Performance (fgbridge_on.txt line 4, F11 panel)
static int g_jit_on;                     // this frame is jittered and the HUD-less point has not come yet
static int g_jit_cur, g_jit_drawn;       // a jittered viewport is applied now; this frame has drawn with one
static float g_jx, g_jy;
static D3D11_VIEWPORT g_gvp[16];         // the game's viewports as it set them
static UINT g_ngvp;
static int g_dsv_main, g_dss_depth = 1;
static ID3D11DepthStencilView *g_last_dsv;
static ID3D11DepthStencilState *g_last_dss;
static unsigned g_jit_vp_calls, g_jit_vp_jittered, g_jit_frames;
static void(STDMETHODCALLTYPE *r_rsvp)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
static void(STDMETHODCALLTYPE *r_rsgetvp)(ID3D11DeviceContext *, UINT *, D3D11_VIEWPORT *);
static void(STDMETHODCALLTYPE *r_omrt)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
static void(STDMETHODCALLTYPE *r_omrtuav)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *, UINT,
                                          UINT, ID3D11UnorderedAccessView *const *, const UINT *);
static void(STDMETHODCALLTYPE *r_omdss)(ID3D11DeviceContext *, ID3D11DepthStencilState *, UINT);

// DLSS below native size (AGENT_LOG 103): from the frame start to the HUD-less point, a viewport that covers a whole
// render target of the screen's size or a power-of-two fraction of it (1, 1/2, 1/4, 1/8: G-buffer, lighting, half-size
// passes, the scene's copy into the back buffer) is shrunk by the render scale, so the 3D scene ends up in the top-left
// render_w x render_h of every such target; DLSS upscales that corner and the UI is drawn at full size afterwards.
static UINT g_rw, g_rh;                  // render size (= the screen with DLAA or SR off)
static UINT g_rt_w, g_rt_h;              // the bound target's size (render target 0, else the depth buffer)
static ID3D11RenderTargetView *g_last_rtv;
static D3D11_VIEWPORT g_avp[16];         // what was applied last

static int screen_part(UINT tw, UINT th) {   // tw x th = screen / k for k = 1, 2, 4, 8
    for (UINT k = 1; k <= 8; k *= 2)
        if (g_live_w % k == 0 && g_live_h % k == 0 && tw == g_live_w / k && th == g_live_h / k) return (int)k;
    return 0;
}

// AGENT_LOG 128: in water battles stage +0x1CA0858 draws the selection markers and the wake foam into two screen-size
// targets that only the ocean shader reads, at full-screen coordinates from its vertex shader (game camera): shrunk,
// markers and foam showed at 2/3 size towards the top-left corner, away from the ships. That stage stays full size.
static unsigned mp_stage_vt(void);
static int overlay_stage(void) { return mp_stage_vt() == g_vt_overlay; }

// the viewports the game set, as they should be now: shrunk and / or jittered (see above), else as set
static int vp_adjust(D3D11_VIEWPORT *v) {
    int changed = 0;
    g_jit_cur = 0;
    memcpy(v, g_gvp, g_ngvp * sizeof *v);
    if (!g_jit_on || overlay_stage()) return 0;   // set from the frame start to the HUD-less point when DLSS is on
    int k = screen_part(g_rt_w, g_rt_h);
    for (UINT i = 0; i < g_ngvp; i++) {
        D3D11_VIEWPORT *x = &v[i];
        int whole = k && x->TopLeftX == 0 && x->TopLeftY == 0 && x->Width == (float)g_rt_w && x->Height == (float)g_rt_h;
        int jitter = whole && k == 1 && g_dsv_main && g_dss_depth;
        if (whole && (g_rw != g_live_w || g_rh != g_live_h)) {
            x->Width = (float)g_rw / k;
            x->Height = (float)g_rh / k;
            changed = 1;
        }
        if (jitter) { x->TopLeftX += g_jx; x->TopLeftY += g_jy; changed = g_jit_cur = 1; }
    }
    return changed;
}

// Pixel shaders at the lower presets (AGENT_LOG 103). The game's lighting, decal, point light, SSAO, fog and water passes
// turn a pixel position back into a world position: ndc = (SV_Position + g_vpos_texel_offset) / (g_viewport_dimensions or
// g_screen_size) * 2 - 1, then inv_view_projection (camera_VS_PS, cb0; disassembled from the game's own shaders in
// data.pack). Both sizes stay the screen's, because the same values also turn pixels into texture coordinates, which
// are right as they are (the scene is in the top-left corner of the full-size targets). So while the scene is scaled,
// pixel shaders get a copy of the camera block whose matrices see the full-size target as the screen: the inverse ones
// take that "full-screen" ndc to the real one (M * S), the forward ones the other way (S^-1 * M), x and y only. Vertex
// shaders keep the game's block: geometry is placed by the shrunk viewport.
// game calls through the hooks below, per 30 s report (AGENT_LOG 109: what the add-on adds per call, times this many)
enum { HK_MAP, HK_UNMAP, HK_UPDSUB, HK_PSCB, HK_OMRT, HK_OMDSS, HK_RSVP, HK_DRAW, HK_N };
static unsigned g_hk[HK_N];
static unsigned g_cam_calls;   // the set-camera jump hook (cam_seen), any thread
// Where the game's main thread blocks (AGENT_LOG 109: in Map and OMSetRenderTargets, 3x as long with the add-on, the GPU
// half idle): time in the real calls, and the Maps that took > 0.2 ms by what was mapped
static unsigned long long g_map_ticks, g_omrt_ticks;
typedef struct { UINT type, dim, usage, bind, cpu, size, flags; unsigned n; unsigned long long ticks; } SlowMap;
static SlowMap g_slow[32];
static unsigned g_nslow;
static unsigned long long g_tsc_ms;   // TSC ticks per ms, measured at the reports
static void slow_map(ID3D11Resource *r, D3D11_MAP t, UINT f, unsigned long long ticks) {
    SlowMap k = {(UINT)t, 0, 0, 0, 0, 0, f, 0, 0};
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    k.dim = dim;
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d;
        ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &d);
        k.usage = d.Usage; k.bind = d.BindFlags; k.cpu = d.CPUAccessFlags; k.size = d.ByteWidth;
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
        k.usage = d.Usage; k.bind = d.BindFlags; k.cpu = d.CPUAccessFlags; k.size = d.Width << 16 | (d.Height & 0xFFFF);
    }
    for (unsigned i = 0; i < g_nslow; i++)
        if (!memcmp(&g_slow[i], &k, offsetof(SlowMap, n))) { g_slow[i].n++; g_slow[i].ticks += ticks; return; }
    if (g_nslow < 32) { k.n = 1; k.ticks = ticks; g_slow[g_nslow++] = k; }
}
typedef struct { ID3D11Buffer *game, *ours; void *mapped; int valid; float src[144]; } CamBuf;   // src: the game's last block
static CamBuf g_cbuf[8];
static unsigned g_ncbuf, g_cam_writes;
static ID3D11Buffer *g_ps_cb0, *g_ps_bound;   // the game's pixel shader constant buffer 0 (one reference held), what is bound
static void (STDMETHODCALLTYPE *r_pscb)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
static void (STDMETHODCALLTYPE *r_unmap)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
static void (STDMETHODCALLTYPE *r_updsub)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT);

// The table holds a reference on each game buffer: a freed buffer's address could otherwise come back as another, smaller
// buffer that would then be read as 576 bytes. When the table is full, an entry that only we still hold is reused.
static CamBuf *cam_find(ID3D11Resource *r, int add) {   // the game's 576-byte constant buffers (camera_VS_PS)
    for (unsigned i = 0; i < g_ncbuf; i++)
        if ((ID3D11Resource *)g_cbuf[i].game == r) return &g_cbuf[i];
    if (!add) return NULL;
    for (unsigned i = 0; i < g_ncbuf; i++)
        if ((ID3D11Resource *)g_cbuf[i].ours == r) return NULL;
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return NULL;
    D3D11_BUFFER_DESC d;
    ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &d);
    if (d.ByteWidth != 576 || !(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return NULL;
    CamBuf *e = NULL;
    if (g_ncbuf < 8) e = &g_cbuf[g_ncbuf];
    else
        for (unsigned i = 0; i < 8 && !e; i++) {
            ID3D11Buffer_AddRef(g_cbuf[i].game);
            if (ID3D11Buffer_Release(g_cbuf[i].game) == 1 && g_cbuf[i].game != g_ps_cb0) {   // only ours left
                ID3D11Buffer_Release(g_cbuf[i].game);
                e = &g_cbuf[i];
            }
        }
    if (!e) return NULL;
    if (!e->ours) {
        D3D11_BUFFER_DESC od = {576, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        if (FAILED(ID3D11Device_CreateBuffer(g_dev, &od, NULL, &e->ours))) return NULL;
    }
    ID3D11Buffer_AddRef((ID3D11Buffer *)r);
    e->game = (ID3D11Buffer *)r; e->mapped = NULL; e->valid = 0;
    if (e == &g_cbuf[g_ncbuf]) g_ncbuf++;
    return e;
}

// cam_patch: the camera block f as the pixel shaders need it, the scene drawn at rw x rh into the top-left of the w x h
// targets with the sub-pixel jitter (jx, jy) in pixels (x right, y down: vp_adjust's viewport offset). A pixel the shader
// turns into ndc with the screen's size is, in the real view, x = sx ndc + sx - 1 - 2 jx / rw, y = sy ndc + 1 - sy +
// 2 jy / rh. The jitter (Rome 2 DLSS_LOG 44, also at DLAA): lighting, shadows, fog, SSAO and decals turn pixels back into
// positions with these matrices; without it they saw the geometry half a pixel from where the jittered viewport drew it,
// in another direction each frame, and DLSS blurred them (Rome 2: "DLAA is very blurry"). A native integration jitters
// the projection, so both agree.
static void cam_patch(float *f, UINT w, UINT h, UINT rw, UINT rh, float jx, float jy) {
    float sx = (float)w / rw, sy = (float)h / rh, ox = sx - 1 - 2 * jx / rw, oy = 1 - sy + 2 * jy / rh;
    for (int m = 0; m < 2; m++) {   // inverse_projection (byte 272), inv_view_projection (336): rows r_i -> r_i * S
        float *r = f + (m ? 84 : 68);
        for (int i = 0; i < 4; i++, r += 4) { r[3] += r[0] * ox + r[1] * oy; r[0] *= sx; r[1] *= sy; }
    }
    for (int m = 0; m < 2; m++) {   // projection (80), view_projection (144): rows x, y -> S^-1 * M
        float *x = f + (m ? 36 : 20), *y = x + 4, *w4 = x + 12;
        for (int j = 0; j < 4; j++) { x[j] = (x[j] - ox * w4[j]) / sx; y[j] = (y[j] - oy * w4[j]) / sy; }
    }
}

static void cam_write(CamBuf *e, const void *src) {
    if (src != e->src) memcpy(e->src, src, sizeof e->src);
    float f[144];
    memcpy(f, e->src, sizeof f);
    if (g_rw && g_rh) cam_patch(f, g_live_w, g_live_h, g_rw, g_rh, g_jx, g_jy);
    r_updsub(g_ctx, (ID3D11Resource *)e->ours, 0, NULL, f, 0, 0);
    e->valid = 1;
    g_cam_writes++;
}
static void cam_refresh(void) {   // the frame's new jitter into the copies: the game may write its camera before the frame starts
    for (unsigned i = 0; i < g_ncbuf; i++)
        if (g_cbuf[i].valid) cam_write(&g_cbuf[i], g_cbuf[i].src);
}

// below native size the copy from the frame start; at DLAA from the frame's first jittered draw (Rome 2's rule: passes
// before the scene keep the game's camera)
static void ps_cb_apply(ID3D11DeviceContext *c) {
    int scaled = g_rw != g_live_w || g_rh != g_live_h;
    CamBuf *e = g_ps_cb0 && g_jit_on && (scaled || ((g_jx || g_jy) && g_jit_drawn)) && screen_part(g_rt_w, g_rt_h) && !overlay_stage()
                    ? cam_find((ID3D11Resource *)g_ps_cb0, 1) : NULL;
    ID3D11Buffer *want = e && e->valid ? e->ours : g_ps_cb0;
    if (want != g_ps_bound && r_pscb) { r_pscb(c, 0, 1, &want); g_ps_bound = want; }
}

static void STDMETHODCALLTYPE w_pscb(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b) {
    g_hk[HK_PSCB]++;
    if (c != g_ctx || start != 0 || !n || !b || n > 14) { r_pscb(c, start, n, b); return; }
    ID3D11Buffer *g = b[0];
    for (unsigned i = 0; i < g_ncbuf; i++)
        if (g == g_cbuf[i].ours) g = g_cbuf[i].game;   // our copy, read back by someone saving state: it stands for the game's
    if (g) ID3D11Buffer_AddRef(g);
    if (g_ps_cb0) ID3D11Buffer_Release(g_ps_cb0);
    g_ps_cb0 = g;
    ID3D11Buffer *v[14];
    memcpy(v, b, n * sizeof *v);
    v[0] = g;
    g_ps_bound = (ID3D11Buffer *)-1;   // set below
    r_pscb(c, 0, n, v);
    g_ps_bound = g;
    ps_cb_apply(c);
}

static void cam_mapped(ID3D11DeviceContext *c, ID3D11Resource *r, D3D11_MAP t, const D3D11_MAPPED_SUBRESOURCE *m) {
    if (c != g_ctx || g_live_state != 1 || (t != D3D11_MAP_WRITE_DISCARD && t != D3D11_MAP_WRITE)) return;
    CamBuf *e = cam_find(r, 1);
    if (e) e->mapped = m->pData;
}
static void STDMETHODCALLTYPE w_unmap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub) {
    g_hk[HK_UNMAP]++;
    CamBuf *e = c == g_ctx ? cam_find(r, 0) : NULL;
    if (e && e->mapped) { cam_write(e, e->mapped); e->mapped = NULL; }
    r_unmap(c, r, sub);
}
static void STDMETHODCALLTYPE w_updsub(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, const D3D11_BOX *box, const void *data, UINT rp, UINT dp) {
    g_hk[HK_UPDSUB]++;
    r_updsub(c, r, sub, box, data, rp, dp);
    if (c != g_ctx || g_live_state != 1 || box || !data) return;
    CamBuf *e = cam_find(r, 1);
    if (e) cam_write(e, data);
}

static void jit_apply(ID3D11DeviceContext *c, int force) {
    ps_cb_apply(c);
    D3D11_VIEWPORT v[16];
    int changed = vp_adjust(v);
    if (!g_ngvp || (!force && !memcmp(v, g_avp, g_ngvp * sizeof *v))) return;
    r_rsvp(c, g_ngvp, v);
    memcpy(g_avp, v, g_ngvp * sizeof *v);
    g_jit_vp_jittered += changed;
}

static void STDMETHODCALLTYPE w_rsvp(ID3D11DeviceContext *c, UINT n, const D3D11_VIEWPORT *v) {
    g_hk[HK_RSVP]++;
    if (c != g_ctx || n > 16) { r_rsvp(c, n, v); return; }
    g_ngvp = n;
    if (n) memcpy(g_gvp, v, n * sizeof *v);
    g_jit_vp_calls++;
    jit_apply(c, 1);
}

static void STDMETHODCALLTYPE w_rsgetvp(ID3D11DeviceContext *c, UINT *n, D3D11_VIEWPORT *v) {   // the game reads back what it set
    if (c != g_ctx || !n) { r_rsgetvp(c, n, v); return; }
    if (v) memcpy(v, g_gvp, (*n < g_ngvp ? *n : g_ngvp) * sizeof *v);
    *n = g_ngvp;
}

// ---- texture mip bias below native size (AGENT_LOG 136, Rome 2 DLSS_LOG 39 / 46): the game picks mip levels for the
// render size, so ground and unit textures come out softer than at native. Bias log2(render width / screen width)
// (Quality -0.58, Performance -1), none at DLAA: NVIDIA's further -1.0 lightened the shadows in Rome 2. Material samplers
// only (anisotropic or wrapping; not shadow comparison, min / max or clamped post-process ones), swapped for biased copies
// while the scene is drawn small into the screen's targets (not the shadow maps: alpha-tested leaves cast lighter shadows
// with it, Rome 2). The game's samplers are held (AddRef) so an address is never reused for another; copies are made again
// when the render size changes.
static struct { ID3D11SamplerState *game, *ours; } g_lod[256];
static unsigned g_nlod, g_lod_swaps, g_lod_full;
static int g_lod_off;      // F11 Debug View (test builds): the game's samplers as they are, to compare
static float g_lod_bias;   // the bias the copies were made with
static struct { ID3D11SamplerState *in, *out; } g_lodc[256];   // the last answers, by address (cleared with the copies)
static ID3D11SamplerState *lod_sampler(ID3D11SamplerState *s, float bias) {
    if (!s) return s;
    if (bias != g_lod_bias) {   // a new render size: new copies
        for (unsigned i = 0; i < g_nlod; i++) {
            ID3D11SamplerState_Release(g_lod[i].game);
            if (g_lod[i].ours) ID3D11SamplerState_Release(g_lod[i].ours);
        }
        g_nlod = 0;
        g_lod_bias = bias;
        memset(g_lodc, 0, sizeof g_lodc);
    }
    unsigned ch = ((unsigned)(UINT_PTR)s >> 4) & 255;
    if (g_lodc[ch].in == s) return g_lodc[ch].out;
    for (unsigned i = 0; i < g_nlod; i++) {
        if (g_lod[i].game == s) { g_lodc[ch].in = s; return g_lodc[ch].out = g_lod[i].ours ? g_lod[i].ours : s; }
        if (g_lod[i].ours == s) { g_lodc[ch].in = s; return g_lodc[ch].out = s; }   // our copy, read back and set again by someone saving state
    }
    if (g_nlod == 256) { g_lod_full++; return s; }   // shortcut: 256 game samplers at most, the rest keep their bias
    D3D11_SAMPLER_DESC d;
    ID3D11SamplerState_GetDesc(s, &d);
    ID3D11SamplerState *o = NULL;
    if (d.Filter < D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT && (d.Filter == D3D11_FILTER_ANISOTROPIC || d.AddressU == D3D11_TEXTURE_ADDRESS_WRAP)) {
        d.MipLODBias = fmaxf(-16.0f, d.MipLODBias + bias);
        if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &d, &o))) o = NULL;
    }
    ID3D11SamplerState_AddRef(s);
    g_lod[g_nlod].game = s;
    g_lod[g_nlod++].ours = o;
    return o ? o : s;
}

static void(STDMETHODCALLTYPE *r_pssamp)(ID3D11DeviceContext *, UINT, UINT, ID3D11SamplerState *const *);
static void STDMETHODCALLTYPE w_pssamp(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11SamplerState *const *s) {
    if (c != g_ctx || g_live_state != 1 || !g_jit_on || g_rw >= g_live_w || g_lod_off || !s || !n || n > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT ||
        !screen_part(g_rt_w, g_rt_h) || overlay_stage()) { r_pssamp(c, start, n, s); return; }
    float bias = log2f((float)g_rw / g_live_w);
    ID3D11SamplerState *b[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    for (UINT i = 0; i < n; i++) g_lod_swaps += (b[i] = lod_sampler(s[i], bias)) != s[i];
    r_pssamp(c, start, n, b);
}

static void dsv_changed(ID3D11DepthStencilView *dsv) {
    if (dsv == g_last_dsv) return;
    g_last_dsv = dsv;
    g_dsv_main = 0;
    if (!dsv) return;
    ID3D11Resource *r = NULL;
    ID3D11DepthStencilView_GetResource(dsv, &r);
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
        g_dsv_main = d.Width == g_live_w && d.Height == g_live_h;
    }
    ID3D11Resource_Release(r);
}

static void target_changed(UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv) {
    ID3D11RenderTargetView *r0 = n && rtv ? rtv[0] : NULL;
    if (r0 == g_last_rtv && r0) return;
    g_last_rtv = r0;
    g_rt_w = g_rt_h = 0;
    ID3D11Resource *r = NULL;
    if (r0) ID3D11RenderTargetView_GetResource(r0, &r);
    else if (dsv) ID3D11DepthStencilView_GetResource(dsv, &r);
    if (!r) return;
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
        g_rt_w = d.Width; g_rt_h = d.Height;
    }
    ID3D11Resource_Release(r);
}

static void STDMETHODCALLTYPE w_omrt(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv) {
    g_hk[HK_OMRT]++;
    unsigned long long t0 = __rdtsc();
    r_omrt(c, n, rtv, dsv);
    g_omrt_ticks += __rdtsc() - t0;
    if (c != g_ctx) return;
    dsv_changed(dsv);
    target_changed(n, rtv, dsv);
    jit_apply(c, 0);
}

static void STDMETHODCALLTYPE w_omrtuav(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv,
                                        UINT us, UINT nu, ID3D11UnorderedAccessView *const *u, const UINT *ic) {
    g_hk[HK_OMRT]++;
    r_omrtuav(c, n, rtv, dsv, us, nu, u, ic);
    if (c != g_ctx || n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) return;
    dsv_changed(dsv);
    target_changed(n, rtv, dsv);
    jit_apply(c, 0);
}

static void STDMETHODCALLTYPE w_omdss(ID3D11DeviceContext *c, ID3D11DepthStencilState *st, UINT ref) {
    g_hk[HK_OMDSS]++;
    r_omdss(c, st, ref);
    if (c != g_ctx) return;
    if (st != g_last_dss) {
        g_last_dss = st;
        D3D11_DEPTH_STENCIL_DESC d = {0};
        d.DepthEnable = TRUE;   // NULL state = default: depth test on
        if (st) ID3D11DepthStencilState_GetDesc(st, &d);
        g_dss_depth = d.DepthEnable;
    }
    jit_apply(c, 0);
}

static void patch_slot(void **slot, void *to, void **old);
static HRESULT(STDMETHODCALLTYPE *r_map)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
static HRESULT STDMETHODCALLTYPE w_map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE *m);
static void jit_install(void) {
    void **vt = *(void ***)g_ctx;
#define SLOT(m) &vt[offsetof(ID3D11DeviceContextVtbl, m) / sizeof(void *)]
    patch_slot(SLOT(RSSetViewports), (void *)w_rsvp, (void **)&r_rsvp);
    patch_slot(SLOT(RSGetViewports), (void *)w_rsgetvp, (void **)&r_rsgetvp);
    patch_slot(SLOT(OMSetRenderTargets), (void *)w_omrt, (void **)&r_omrt);
    patch_slot(SLOT(OMSetRenderTargetsAndUnorderedAccessViews), (void *)w_omrtuav, (void **)&r_omrtuav);
    patch_slot(SLOT(OMSetDepthStencilState), (void *)w_omdss, (void **)&r_omdss);
    patch_slot(SLOT(PSSetConstantBuffers), (void *)w_pscb, (void **)&r_pscb);
    patch_slot(SLOT(PSSetSamplers), (void *)w_pssamp, (void **)&r_pssamp);
    patch_slot(SLOT(Map), (void *)w_map, (void **)&r_map);
    patch_slot(SLOT(Unmap), (void *)w_unmap, (void **)&r_unmap);
    patch_slot(SLOT(UpdateSubresource), (void *)w_updsub, (void **)&r_updsub);
#undef SLOT
}

// frame start (after Present): this frame's jitter; HUD-less point: back to the game's own viewports
static void jit_frame_start(void) {
    if (!g_sr || g_live_state != 1) return;
    fgb_jitter(&g_jx, &g_jy);
    g_jit_on = g_scene_last && (g_jx != 0 || g_jy != 0 || g_rw != g_live_w || g_rh != g_live_h);
    g_frame_scaled = g_jit_on;
    g_jit_frames += g_jit_on;
    g_jit_drawn = 0;
    if (g_jit_on) cam_refresh();
    jit_apply(g_ctx, 1);
}
static void jit_frame_end(void) {
    if (!g_jit_on) return;   // also after DLAA was switched off (F11 panel)
    g_jit_on = 0;
    jit_apply(g_ctx, 0);
}

// ---- Motion probe (dev, AGENT_LOG 97): what a per-object motion pass needs to know. With motion_on.txt in the game
// folder at start, every vertex shader the game creates is appended once (by hash) to motion_vs.bin and every input
// layout's elements are kept. In a bridge capture (bridge_go.txt) every draw is logged with its stage, VS hash, input
// layout, vertex buffers, VS textures / constant buffers and targets; for the first draws that read the animation
// texture (768x512 R32G32B32A32_FLOAT) the instance records and VS constant buffers are copied out at draw time.
static int g_mp_on;
static CRITICAL_SECTION g_mp_lock;
static FILE *g_mp_vsf;
static struct { const void *vs; unsigned long long h; } g_mp_vs[32768];          // VS object -> bytecode hash
static unsigned long long g_mp_written[16384];                                     // hashes already in motion_vs.bin
static unsigned g_mp_nwritten, g_mp_vs_unknown;
#define MP_MAX_LAYOUTS 4096
static struct { const void *il; unsigned n; unsigned long long sig; D3D11_INPUT_ELEMENT_DESC e[16]; char names[16][24]; int used; } *g_mp_il;
static unsigned g_mp_nil;
static HRESULT(STDMETHODCALLTYPE *r_cvs)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11VertexShader **);
static HRESULT(STDMETHODCALLTYPE *r_cil)(ID3D11Device *, const D3D11_INPUT_ELEMENT_DESC *, UINT, const void *, SIZE_T, ID3D11InputLayout **);

static unsigned long long fnv64(const void *p, size_t n) {
    const unsigned char *b = p;
    unsigned long long h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static HRESULT STDMETHODCALLTYPE w_cvs(ID3D11Device *d, const void *bc, SIZE_T len, ID3D11ClassLinkage *cl, ID3D11VertexShader **out) {
    HRESULT hr = r_cvs(d, bc, len, cl, out);
    if (FAILED(hr) || !out || !*out) return hr;
    unsigned long long h = fnv64(bc, len);
    EnterCriticalSection(&g_mp_lock);
    unsigned i = (unsigned)(((UINT_PTR)*out >> 4) * 2654435761u) & 32767;
    for (unsigned k = 0; k < 32768; k++, i = (i + 1) & 32767)
        if (!g_mp_vs[i].vs || g_mp_vs[i].vs == *out) { g_mp_vs[i].vs = *out; g_mp_vs[i].h = h; break; }
    int seen = 0;
    for (unsigned k = 0; k < g_mp_nwritten && !seen; k++) seen = g_mp_written[k] == h;
    if (!seen && g_mp_vsf && g_mp_nwritten < 16384) {
        unsigned sz = (unsigned)len;
        fwrite(&h, 8, 1, g_mp_vsf);
        fwrite(&sz, 4, 1, g_mp_vsf);
        fwrite(bc, 1, len, g_mp_vsf);
        fflush(g_mp_vsf);
        g_mp_written[g_mp_nwritten++] = h;
    }
    LeaveCriticalSection(&g_mp_lock);
    return hr;
}

// Pixel shaders the game creates (AGENT_LOG 139): their DXBC checksum (blob bytes 4..11, as Rome 2's audit scripts in
// tools/shader_audit/rome2 key them; shader_index.py maps it to the fxc\ name), so a capture names every draw's pixel shader
static struct { const void *ps; unsigned char sum[8]; } g_ps[16384];
static SRWLOCK g_ps_lock = SRWLOCK_INIT;   // created on loading threads, looked up on the main thread
static const unsigned char BLOOM_DS_SUM[8] = {0x64, 0x23, 0x2a, 0x1e, 0xc0, 0x0e, 0x69, 0xc2};   // the bloom source pass (below)
static const void *g_ps_bloom_ds;          // its shader object
static HRESULT(STDMETHODCALLTYPE *r_cps)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11PixelShader **);
static HRESULT STDMETHODCALLTYPE w_cps(ID3D11Device *d, const void *bc, SIZE_T len, ID3D11ClassLinkage *cl, ID3D11PixelShader **out) {
    HRESULT hr = r_cps(d, bc, len, cl, out);
    if (FAILED(hr) || !out || !*out || len < 12) return hr;
    AcquireSRWLockExclusive(&g_ps_lock);
    unsigned i = (unsigned)(((UINT_PTR)*out >> 4) * 2654435761u) & 16383;
    for (unsigned k = 0; k < 16384; k++, i = (i + 1) & 16383)   // shortcut: full after 16384 shaders, the rest log as unknown
        if (!g_ps[i].ps || g_ps[i].ps == *out) { g_ps[i].ps = *out; memcpy(g_ps[i].sum, (const char *)bc + 4, 8); break; }
    ReleaseSRWLockExclusive(&g_ps_lock);
    if (!memcmp((const char *)bc + 4, BLOOM_DS_SUM, 8)) g_ps_bloom_ds = *out;   // the bloom source pass (AGENT_LOG 141)
    return hr;
}
static void ps_sum(const void *ps, char out[17]) {   // "?" for one made before the hook
    strcpy(out, "?");
    AcquireSRWLockShared(&g_ps_lock);
    unsigned i = (unsigned)(((UINT_PTR)ps >> 4) * 2654435761u) & 16383;
    for (unsigned k = 0; k < 16384 && g_ps[i].ps; k++, i = (i + 1) & 16383)
        if (g_ps[i].ps == ps) { for (int j = 0; j < 8; j++) sprintf(out + 2 * j, "%02x", g_ps[i].sum[j]); break; }
    ReleaseSRWLockShared(&g_ps_lock);
}

static unsigned long long mp_vs_hash(const void *vs) {
    unsigned i = (unsigned)(((UINT_PTR)vs >> 4) * 2654435761u) & 32767;
    for (unsigned k = 0; k < 32768 && g_mp_vs[i].vs; k++, i = (i + 1) & 32767)
        if (g_mp_vs[i].vs == vs) return g_mp_vs[i].h;
    return 0;
}

static HRESULT STDMETHODCALLTYPE w_cil(ID3D11Device *d, const D3D11_INPUT_ELEMENT_DESC *e, UINT n, const void *bc, SIZE_T len,
                                       ID3D11InputLayout **out) {
    HRESULT hr = r_cil(d, e, n, bc, len, out);
    if (FAILED(hr) || !out || !*out) return hr;
    if (g_motion) fgm_layout_created(*out, e, n, fnv64(bc, len));
    if (!g_mp_on) return hr;
    EnterCriticalSection(&g_mp_lock);
    if (g_mp_nil < MP_MAX_LAYOUTS) {
        unsigned k = g_mp_nil++;
        g_mp_il[k].il = *out;
        g_mp_il[k].n = n < 16 ? n : 16;
        g_mp_il[k].sig = fnv64(bc, len);
        for (unsigned j = 0; j < g_mp_il[k].n; j++) {
            g_mp_il[k].e[j] = e[j];
            snprintf(g_mp_il[k].names[j], 24, "%s", e[j].SemanticName ? e[j].SemanticName : "?");
        }
    }
    LeaveCriticalSection(&g_mp_lock);
    return hr;
}

static int mp_layout(const void *il) {
    for (unsigned k = g_mp_nil; k-- > 0;)   // the newest object at an address
        if (g_mp_il[k].il == il) return (int)k;
    return -1;
}

static void patch_slot(void **slot, void *to, void **old);
static void mp_device(IUnknown *dev) {
    static int done;
    if (!(g_mp_on || g_motion) || done || !dev) return;
    static const GUID iid_dev = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};   // ID3D11Device
    ID3D11Device *d = NULL;
    if (FAILED(IUnknown_QueryInterface(dev, &iid_dev, (void **)&d))) return;
    void **vt = *(void ***)d;
    if (g_mp_on) patch_slot(&vt[offsetof(ID3D11DeviceVtbl, CreateVertexShader) / sizeof(void *)], (void *)w_cvs, (void **)&r_cvs);
    patch_slot(&vt[offsetof(ID3D11DeviceVtbl, CreateInputLayout) / sizeof(void *)], (void *)w_cil, (void **)&r_cil);
    patch_slot(&vt[offsetof(ID3D11DeviceVtbl, CreatePixelShader) / sizeof(void *)], (void *)w_cps, (void **)&r_cps);
    ID3D11Device_Release(d);
    done = 1;
}

// per-draw logging in the captured frame
static void(STDMETHODCALLTYPE *r_dii)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
static void(STDMETHODCALLTYPE *r_di)(ID3D11DeviceContext *, UINT, UINT, INT);
static void(STDMETHODCALLTYPE *r_dinst)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
static void(STDMETHODCALLTYPE *r_draw)(ID3D11DeviceContext *, UINT, UINT);
static unsigned g_mp_draws, g_mp_anim_draws;
static struct { ID3D11Buffer *stg; unsigned bytes, stride, what; char tag[96]; } g_mp_copy[24];
static unsigned g_mp_ncopy;

static unsigned mp_stage_vt(void) {
    for (unsigned i = g_sp < 64 ? g_sp : 64; i-- > 0;)
        if (g_stk[i].id == g_id_stage && g_stk[i].obj) return *(const unsigned *)g_stk[i].obj - g_pt_base;
    return 0;
}

static void mp_copy_buffer(ID3D11Buffer *src, unsigned off, unsigned bytes, unsigned stride, unsigned what, const char *tag) {
    if (g_mp_ncopy >= 24 || !src || !bytes) return;
    D3D11_BUFFER_DESC bd = {bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ID3D11Buffer *stg = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &stg))) return;
    D3D11_BOX box = {off, 0, 0, off + bytes, 1, 1};
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, (ID3D11Resource *)src, 0, &box);
    g_mp_copy[g_mp_ncopy].stg = stg; g_mp_copy[g_mp_ncopy].bytes = bytes; g_mp_copy[g_mp_ncopy].stride = stride;
    g_mp_copy[g_mp_ncopy].what = what;
    snprintf(g_mp_copy[g_mp_ncopy].tag, 96, "%s", tag);
    g_mp_ncopy++;
}

static void mp_draw(ID3D11DeviceContext *c, const char *kind, UINT count, UINT inst, UINT start_inst) {
    if (g_jit_cur && !g_jit_drawn && c == g_ctx) { g_jit_drawn = 1; ps_cb_apply(c); }   // the pixel shaders see the jitter from now on
    if (!g_capturing || !g_cap || c != g_ctx || GetCurrentThreadId() != g_pt_main) return;
    g_mp_draws++;
    ID3D11VertexShader *vs = NULL;
    ID3D11InputLayout *il = NULL;
    ID3D11Buffer *vb[4] = {0}, *cb[4] = {0};
    UINT st[4] = {0}, of[4] = {0};
    ID3D11ShaderResourceView *srv[8] = {0}, *psrv[16] = {0};
    ID3D11SamplerState *psmp[16] = {0};
    ID3D11PixelShader *ps = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 16, psrv);
    ID3D11DeviceContext_PSGetSamplers(c, 0, 16, psmp);
    ID3D11DeviceContext_VSGetShader(c, &vs, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(c, &il);
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 4, vb, st, of);
    ID3D11DeviceContext_VSGetShaderResources(c, 0, 8, srv);
    ID3D11DeviceContext_VSGetConstantBuffers(c, 0, 4, cb);
    ID3D11DeviceContext_OMGetRenderTargets(c, 1, &rtv, &dsv);
    unsigned long long h = vs ? mp_vs_hash(vs) : 0;
    if (vs && !h) g_mp_vs_unknown++;
    int li = il ? mp_layout(il) : -1;
    if (li >= 0) g_mp_il[li].used = 1;
    char line[2048];
    int n = snprintf(line, sizeof line, "  draw %u stage +0x%X %s count %u inst %u start %u vs %016llx il %d", g_mp_draws, mp_stage_vt(), kind,
                     count, inst, start_inst, h, li);
#define snprintf(b, l, ...) (n < (int)sizeof line ? snprintf(b, l, __VA_ARGS__) : 0)   // full: the rest is dropped
    for (int i = 0; i < 4; i++)
        if (vb[i]) {
            D3D11_BUFFER_DESC bd;
            ID3D11Buffer_GetDesc(vb[i], &bd);
            n += snprintf(line + n, sizeof line - n, " vb%d[stride %u off %u size %u u%u]", i, st[i], of[i], bd.ByteWidth, bd.Usage);
        }
    int anim = -1;
    for (int i = 0; i < 8; i++)
        if (srv[i]) {
            ID3D11Resource *r;
            D3D11_RESOURCE_DIMENSION dim;
            ID3D11ShaderResourceView_GetResource(srv[i], &r);
            ID3D11Resource_GetType(r, &dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC td;
                ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
                n += snprintf(line + n, sizeof line - n, " t%d[%ux%u f%u]", i, td.Width, td.Height, td.Format);
                if (td.Width == 768 && td.Height == 512 && td.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) anim = i;
            } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
                D3D11_BUFFER_DESC bd;
                ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &bd);
                n += snprintf(line + n, sizeof line - n, " t%d[buf %u]", i, bd.ByteWidth);
            } else n += snprintf(line + n, sizeof line - n, " t%d[dim %d]", i, dim);
            ID3D11Resource_Release(r);
        }
    for (int i = 0; i < 4; i++)
        if (cb[i]) {
            D3D11_BUFFER_DESC bd;
            ID3D11Buffer_GetDesc(cb[i], &bd);
            n += snprintf(line + n, sizeof line - n, " cb%d[%u]", i, bd.ByteWidth);
        }
    if (rtv || dsv) {
        char t[200];
        ID3D11Resource *bbr = backbuffer();
        desc_of((ID3D11View *)rtv, t, sizeof t, bbr);
        n += snprintf(line + n, sizeof line - n, " rt0[%s]", t);
        desc_of((ID3D11View *)dsv, t, sizeof t, bbr);
        n += snprintf(line + n, sizeof line - n, " ds[%s]", t);
        if (bbr) ID3D11Resource_Release(bbr);
    }
    if (r_rsgetvp) {   // the viewport the GPU uses (shrunk / jittered by the render scale) and the one the game set
        D3D11_VIEWPORT v;
        UINT nv = 1;
        r_rsgetvp(c, &nv, &v);
        if (nv) n += snprintf(line + n, sizeof line - n, " vp[%.2f,%.2f %.0fx%.0f game %.0fx%.0f%s]", v.TopLeftX, v.TopLeftY, v.Width,
                              v.Height, g_ngvp ? g_gvp[0].Width : 0.f, g_ngvp ? g_gvp[0].Height : 0.f, g_jit_on ? " scaled" : "");
    }
    // the pixel shader, the textures it reads and its samplers (AGENT_LOG 139: the audit of every pass below native size)
    if (ps) {
        char sum[17];
        ps_sum(ps, sum);
        n += snprintf(line + n, sizeof line - n, " ps %s reads:", sum);
        for (int i = 0; i < 16; i++)
            if (psrv[i]) {
                ID3D11Resource *r;
                D3D11_RESOURCE_DIMENSION dim;
                ID3D11ShaderResourceView_GetResource(psrv[i], &r);
                ID3D11Resource_GetType(r, &dim);
                if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                    D3D11_TEXTURE2D_DESC td;
                    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
                    n += snprintf(line + n, sizeof line - n, " t%d [%ux%u f%u%s]", i, td.Width, td.Height, td.Format,
                                  td.MipLevels > 1 ? " mips" : "");
                } else n += snprintf(line + n, sizeof line - n, " t%d [dim %d]", i, dim);
                ID3D11Resource_Release(r);
            }
        n += snprintf(line + n, sizeof line - n, " samp:");
        for (int i = 0; i < 16; i++)
            if (psmp[i]) {
                D3D11_SAMPLER_DESC sd;
                ID3D11SamplerState_GetDesc(psmp[i], &sd);
                n += snprintf(line + n, sizeof line - n, " s%d [f0x%X a%u/%u b%.2f]", i, sd.Filter, sd.AddressU, sd.AddressV, sd.MipLODBias);
            }
    }
#undef snprintf
    fprintf(g_cap, "%s%s\n", line, anim >= 0 ? " ANIM" : "");
    // the first draws that read the animation texture: their instance records and VS constant buffers
    if (anim >= 0) {
        g_mp_anim_draws++;
        if (g_mp_anim_draws <= 6 && li >= 0) {
            for (unsigned j = 0; j < g_mp_il[li].n; j++) {
                const D3D11_INPUT_ELEMENT_DESC *e = &g_mp_il[li].e[j];
                if (e->InputSlotClass != D3D11_INPUT_PER_INSTANCE_DATA || e->InputSlot >= 4 || !vb[e->InputSlot]) continue;
                unsigned s = e->InputSlot, rec = st[s] ? st[s] : 16, k = inst < 4 ? inst : 4;
                char tag[96];
                snprintf(tag, sizeof tag, "draw %u vb%u instance records %u..%u", g_mp_draws, s, start_inst, start_inst + k - 1);
                mp_copy_buffer(vb[s], of[s] + start_inst * rec, k * rec, rec, 1, tag);
                break;
            }
            for (int i = 0; i < 4 && g_mp_anim_draws <= 2; i++)
                if (cb[i]) {
                    D3D11_BUFFER_DESC bd;
                    ID3D11Buffer_GetDesc(cb[i], &bd);
                    char tag[96];
                    snprintf(tag, sizeof tag, "draw %u VS cb%d", g_mp_draws, i);
                    mp_copy_buffer(cb[i], 0, bd.ByteWidth < 4096 ? bd.ByteWidth : 4096, 16, 2, tag);
                }
        }
    }
    if (vs) ID3D11VertexShader_Release(vs);
    if (il) ID3D11InputLayout_Release(il);
    for (int i = 0; i < 4; i++) { if (vb[i]) ID3D11Buffer_Release(vb[i]); if (cb[i]) ID3D11Buffer_Release(cb[i]); }
    for (int i = 0; i < 8; i++) if (srv[i]) ID3D11ShaderResourceView_Release(srv[i]);
    for (int i = 0; i < 16; i++) { if (psrv[i]) ID3D11ShaderResourceView_Release(psrv[i]); if (psmp[i]) ID3D11SamplerState_Release(psmp[i]); }
    if (ps) ID3D11PixelShader_Release(ps);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (dsv) ID3D11DepthStencilView_Release(dsv);
}

static void STDMETHODCALLTYPE w_dii(ID3D11DeviceContext *c, UINT ic, UINT inst, UINT si, INT bv, UINT sinst) {
    g_hk[HK_DRAW]++;
    mp_draw(c, "DrawIndexedInstanced", ic, inst, sinst);
    if (g_motion && c == g_ctx && GetCurrentThreadId() == g_pt_main && mp_stage_vt() == g_vt_main) fgm_draw(c, ic, inst, si, bv, sinst);
    r_dii(c, ic, inst, si, bv, sinst);
}
static void STDMETHODCALLTYPE w_di(ID3D11DeviceContext *c, UINT ic, UINT si, INT bv) {
    mp_draw(c, "DrawIndexed", ic, 1, 0);
    r_di(c, ic, si, bv);
}
static void STDMETHODCALLTYPE w_dinst(ID3D11DeviceContext *c, UINT vc, UINT inst, UINT sv, UINT sinst) {
    mp_draw(c, "DrawInstanced", vc, inst, sinst);
    r_dinst(c, vc, inst, sv, sinst);
}
// ---- Bloom with DLSS (AGENT_LOG 141): the tone mapper's bloom source (hdr_tone_mapper.hlsl
// ps_downsample_image_store_luma, DXBC checksum 64232a1ec00e69c2) takes 3 x 3 bilinear samples a quarter texel apart per
// 256 x 256 texel from the HDR image, which DLSS jitters every frame: a small bright spot (a standard's gold) falls between
// and onto the samples in turn and its glow pulsed (user: around a Roman banner at Quality). At its draw the same
// shader runs with the samples moved by the frame's jitter, so it reads what the game reads without DLSS. Below native
// size the frame fills only the image's top-left render size: samples past its last pixel take that pixel, as the
// screen's edge does at native size (the empty rest was blurred into the bloom: the bottom-right of the view 2-3 %
// darker at Quality / Balanced / Performance in game, AGENT_LOG 141).
static ID3D11PixelShader *g_bloom_ps;
static ID3D11Buffer *g_bloom_cb;
static int g_bloom_off;              // 1 F11 Debug View (test builds), 2 ours not built
static unsigned g_bloom_draws;
// bloom_src: the game's shader (disassembled from data.pack) with j added (the jitter in the source's uv) and the samples
// kept inside the frame (maxuv: its last pixel's centre)
static const char bloom_src[] =
    "Texture2D<float4> t : register(t0);\n"
    "SamplerState s : register(s0);\n"
    "cbuffer cam : register(b0) { float4 cam[29]; }\n"   // camera_VS_PS: g_vpos_texel_offset at [28].x
    "cbuffer dims : register(b1) { float2 recip; }\n"    // render_target_dims_buffer: 1 / the target's size
    "cbuffer jit : register(b2) { float2 j, maxuv; }\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 uv = (pos.xy + cam[28].x) * recip + j, d = recip * 0.25;\n"
    "  float3 a = 0;\n"
    "  [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) a += t.Sample(s, min(uv + float2(x, y) * d, maxuv)).rgb;\n"
    "  a *= 0.111111;\n"
    "  return float4(a, dot(a, float3(0.2126, 0.7152, 0.0722)));\n"
    "}\n";
typedef HRESULT(WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                    ID3DBlob **, ID3DBlob **);
static int bloom_init(void) {
    compile_fn comp = (compile_fn)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    ID3DBlob *b = NULL;
    D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    int ok = comp && SUCCEEDED(comp(bloom_src, strlen(bloom_src), "fgb_bloom", NULL, NULL, "ps", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, NULL)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &g_bloom_ps)) &&
             SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_bloom_cb));
    if (b) ID3D10Blob_Release(b);
    if (!ok) { g_bloom_off = 2; g_log("frame bridge: the bloom pass with the jitter taken out not built - the game's runs as it is"); }
    return ok;
}
static int bloom_draw(ID3D11DeviceContext *c, UINT vc, UINT sv) {   // 1: drawn here
    ID3D11PixelShader *ps = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    if (ps && (const void *)ps == g_ps_bloom_ds) ID3D11DeviceContext_PSGetShaderResources(c, 0, 1, &srv);
    int done = 0;
    if (srv && (g_bloom_ps || bloom_init())) {
        ID3D11Resource *r;
        D3D11_TEXTURE2D_DESC td = {0};
        ID3D11ShaderResourceView_GetResource(srv, &r);
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
        ID3D11Resource_Release(r);
        float k[4] = {g_jx / td.Width, g_jy / td.Height, (g_rw - 0.5f) / td.Width, (g_rh - 0.5f) / td.Height};   // the scene 1:1 (render pixel = texel)
        ID3D11Buffer *old = NULL;
        r_updsub(c, (ID3D11Resource *)g_bloom_cb, 0, NULL, k, 0, 0);
        ID3D11DeviceContext_PSGetConstantBuffers(c, 2, 1, &old);
        ID3D11DeviceContext_PSSetShader(c, g_bloom_ps, NULL, 0);
        r_pscb(c, 2, 1, &g_bloom_cb);
        r_draw(c, vc, sv);
        ID3D11DeviceContext_PSSetShader(c, ps, NULL, 0);
        r_pscb(c, 2, 1, &old);
        if (old) ID3D11Buffer_Release(old);
        mp_hook(NULL);   // d3d11 can put its own Draw back during PSSetShader (ToB)
        g_bloom_draws++;
        done = 1;
    }
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (ps) ID3D11PixelShader_Release(ps);
    return done;
}
static void STDMETHODCALLTYPE w_draw(ID3D11DeviceContext *c, UINT vc, UINT sv) {
    mp_draw(c, "Draw", vc, 1, 0);
    if (g_ps_bloom_ds && !g_bloom_off && c == g_ctx && g_jit_on && (g_jx || g_jy) && bloom_draw(c, vc, sv)) return;
    r_draw(c, vc, sv);
}
static HRESULT(STDMETHODCALLTYPE *r_map)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
static HRESULT STDMETHODCALLTYPE w_map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE *m) {
    unsigned long long t0 = __rdtsc();
    HRESULT hr = r_map(c, r, sub, t, f, m);
    unsigned long long dt = __rdtsc() - t0;
    g_map_ticks += dt;
    if (g_tsc_ms && dt * 5 > g_tsc_ms && c == g_ctx) slow_map(r, t, f, dt);
    g_hk[HK_MAP]++;
    if (SUCCEEDED(hr) && g_motion && c == g_ctx) fgm_mapped(r, t, m);
    if (SUCCEEDED(hr)) cam_mapped(c, r, t, m);
    return hr;
}

// the draw slots of the context's vtable: hooked at capture start and checked at every stage of the captured frame,
// because something replaced them (campaign run 2026-10-28: draws were seen in the shadow stage only, then none). A slot
// found replaced is logged with the module that owns the new pointer and hooked again on top of it.
static void mp_hook(const char *where) {
    if (!(g_mp_on || g_motion) || !g_ctx) return;
    void **vt = *(void ***)g_ctx;
    static const struct { size_t off; const char *name; } slots[5] = {
        {offsetof(ID3D11DeviceContextVtbl, DrawIndexedInstanced), "DrawIndexedInstanced"},
        {offsetof(ID3D11DeviceContextVtbl, DrawIndexed), "DrawIndexed"},
        {offsetof(ID3D11DeviceContextVtbl, DrawInstanced), "DrawInstanced"},
        {offsetof(ID3D11DeviceContextVtbl, Draw), "Draw"},
        {offsetof(ID3D11DeviceContextVtbl, Map), "Map"}};
    void *const ours[5] = {(void *)w_dii, (void *)w_di, (void *)w_dinst, (void *)w_draw, (void *)w_map};
    void **const orig[5] = {(void **)&r_dii, (void **)&r_di, (void **)&r_dinst, (void **)&r_draw, (void **)&r_map};
    static HMODULE d3d;
    static int foreign[5];
    if (!d3d) d3d = GetModuleHandleW(L"d3d11.dll");
    for (int i = 0; i < 5; i++) {
        void **slot = &vt[slots[i].off / sizeof(void *)];
        if (*slot == ours[i]) continue;
        HMODULE owner = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)*slot, &owner);
        if (*orig[i] && owner != d3d) {
            // Not d3d11 swapping its own functions (it does, see above): another mod hooked the slot after us and calls
            // ours as its original. Hooking again would make ours call it back, forever (stack overflow). Left to it.
            if (!foreign[i]) {
                wchar_t name[MAX_PATH] = L"?";
                if (owner) GetModuleFileNameW(owner, name, MAX_PATH);
                const wchar_t *b = wcsrchr(name, L'\\');
                g_log("motion pass: %s on the game's D3D11 context is now hooked by %ls - left to it", slots[i].name, b ? b + 1 : name);
                foreign[i] = 1;
            }
            continue;
        }
        if (where && g_cap) {
            HMODULE mod = NULL;
            char name[MAX_PATH] = "?";
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)*slot, &mod);
            if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
            const char *b = strrchr(name, '\\');
            fprintf(g_cap, "  !! %s slot replaced (%s): now %s+0x%X, hooked again\n", slots[i].name, where, b ? b + 1 : name,
                    (unsigned)((UINT_PTR)*slot - (UINT_PTR)mod));
        }
        patch_slot(slot, ours[i], orig[i]);
    }
}

static void mp_capture_start(void) {
    g_mp_draws = g_mp_anim_draws = g_mp_ncopy = g_mp_vs_unknown = 0;
    for (unsigned k = 0; k < g_mp_nil; k++) g_mp_il[k].used = 0;
    mp_hook(NULL);
}

static void mp_capture_end(FILE *f) {
    if (!g_mp_on || !f) return;
    fprintf(f, "== motion probe: %u draws, %u with the animation texture, %u with a vertex shader created before the hooks\n", g_mp_draws,
            g_mp_anim_draws, g_mp_vs_unknown);
    for (unsigned k = 0; k < g_mp_nil; k++) {
        if (!g_mp_il[k].used) continue;
        fprintf(f, "  layout %u (signature %016llx):", k, g_mp_il[k].sig);
        for (unsigned j = 0; j < g_mp_il[k].n; j++) {
            const D3D11_INPUT_ELEMENT_DESC *e = &g_mp_il[k].e[j];
            fprintf(f, " %s%u f%u slot %u off %u %s%u;", g_mp_il[k].names[j], e->SemanticIndex, e->Format, e->InputSlot, e->AlignedByteOffset,
                    e->InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA ? "inst/" : "vert", e->InputSlotClass ? e->InstanceDataStepRate : 0);
        }
        fprintf(f, "\n");
    }
    for (unsigned i = 0; i < g_mp_ncopy; i++) {
        D3D11_MAPPED_SUBRESOURCE m;
        fprintf(f, "  %s (%u bytes, %u per record):\n", g_mp_copy[i].tag, g_mp_copy[i].bytes, g_mp_copy[i].stride);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_mp_copy[i].stg, 0, D3D11_MAP_READ, 0, &m))) {
            const float *v = m.pData;
            const unsigned *u = m.pData;
            for (unsigned w = 0; w < g_mp_copy[i].bytes / 4; w++) {
                if (w % 4 == 0) fprintf(f, "    +%04X:", w * 4);
                fprintf(f, " %12.5g(%08x)", v[w], u[w]);
                if (w % 4 == 3) fprintf(f, "\n");
            }
            if ((g_mp_copy[i].bytes / 4) % 4) fprintf(f, "\n");
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_mp_copy[i].stg, 0);
        }
        ID3D11Buffer_Release(g_mp_copy[i].stg);
    }
    g_mp_ncopy = 0;
}

// the F11 panel's switches (take effect from the next frame)
// render size per SR mode: off and DLAA native, then NVIDIA's preset scales (Quality 2/3, Balanced 0.58, Performance 1/2,
// Ultra Performance 1/3), rounded down to even
static void sr_set(int mode) {
    static const float scale[6] = {1, 1, 2.0f / 3, 0.58f, 0.5f, 1.0f / 3};
    float k = scale[mode >= 0 && mode < 6 ? mode : 0];
    g_sr = mode;
    g_rw = mode > 1 ? (UINT)(g_live_w * k) & ~1u : g_live_w;
    g_rh = mode > 1 ? (UINT)(g_live_h * k) & ~1u : g_live_h;
    fgb_set_sr(mode != 0, g_rw, g_rh);
}
// the add-on's settings.txt: one key=value per line, written back whenever the F11 panel changes something
static const char *const SR_KEYS[6] = {"off", "dlaa", "quality", "balanced", "performance", "ultra_performance"};
static const char *const FG_KEYS[4] = {"off", "2x", "3x", "4x"};
// the frame cap: off, auto (default), or a fixed rendered-frame cap
static const char *const CAP_KEYS[5] = {"off", "auto", "60", "90", "120"};
static const unsigned CAP_FPS[5] = {0, 0, 60, 90, 120};
static int g_fcap = 1;
// the DLSS preset (NVIDIA's model, Rome 2 DLSS_LOG 38): default (NVIDIA's pick per mode: K for DLAA / Quality / Balanced,
// M Performance, L Ultra Performance) or one for every mode; handed to the helper as NVIDIA's preset number
static const char *const PRESET_KEYS[5] = {"default", "j", "k", "l", "m"};
static const unsigned PRESET_NGX[5] = {0, 10, 11, 12, 13};
static int g_preset;
// sharpening of the DLSS output (fgbridge.c: AMD's RCAS): off, or RCAS's con at 1.25 / 0.6 / 0.2 stops (AMD's default
// 0.2). Attila in game (AGENT_LOG 141, 4K battle, detail at half size against native): sharpness off / low / medium / high
// 84 / 90 / 96 / 106 % at DLAA, low / medium 90 / 97 % at Quality and Balanced: medium is the default (Rome 2: low)
static const char *const SHARP_KEYS[4] = {"off", "low", "medium", "high"};
static const float SHARP_CON[4] = {0, 0.42f, 0.66f, 0.87f};
static int g_sharp = 2;
// V-Sync of the add-on's frames (6.9.2, players with fps held at their screen's refresh rate): on (default, no tearing) or off
static const char *const VSYNC_KEYS[2] = {"off", "on"};
static int g_vsync = 1;
static void settings_save(void) {
    if (!g_settings[0]) return;
    FILE *f = _wfopen(g_settings, L"w");
    if (!f) { g_log("DLSS settings: %ls not writable - choices not saved", g_settings); return; }
    fprintf(f, "# DLSS add-on settings. The F11 panel in game changes and saves them.\n"
               "# super_resolution: off, dlaa, quality, balanced, performance, ultra_performance\n"
               "# frame_generation: off, 2x, 3x, 4x (limited to what your GPU allows)\n"
               "# motion_vectors: camera_only, per_object (cleaner frame generation on moving units, costs fps in big battles)\n"
               "# frame_cap: off, auto, 60, 90, 120 (rendered frames; auto: the screen's refresh rate / the frames shown per\n"
               "#            rendered frame with frame generation on, e.g. 120 at 2x on a 240 Hz screen)\n"
               "# dlss_preset: default, j, k, l, m (NVIDIA's DLSS model; default: K for DLAA / Quality / Balanced, M for\n"
               "#              Performance, L for Ultra Performance)\n"
               "# sharpness: off, low, medium, high (sharpening of the DLSS output, as games with DLSS built in have)\n"
               "# vsync: on, off (on: frames are shown at the screen's refresh rate, no tearing; off: no cap, but they may tear)\n"
               "# addon: on, off (off: the add-on does nothing at all from the next game start; set on again to use it)\n"
               "super_resolution=%s\nframe_generation=%s\nmotion_vectors=%s\nframe_cap=%s\ndlss_preset=%s\nsharpness=%s\nvsync=%s\naddon=on\nversion=3\n",
            SR_KEYS[g_sr >= 0 && g_sr < 6 ? g_sr : 2], FG_KEYS[g_fg < 4 ? g_fg : 0], g_motion_on ? "per_object" : "camera_only",
            CAP_KEYS[g_fcap >= 0 && g_fcap < 5 ? g_fcap : 1], PRESET_KEYS[g_preset >= 0 && g_preset < 5 ? g_preset : 0],
            SHARP_KEYS[g_sharp >= 0 && g_sharp < 4 ? g_sharp : 2], VSYNC_KEYS[g_vsync != 0]);
    fclose(f);
}
static int settings_value(const char *text, const char *key, const char *const *names, int n, int fallback) {
    const char *v = strstr(text, key);
    if (!v) return fallback;
    v += strlen(key);
    for (int i = n; i-- > 0;)   // longest names first is not needed: values are compared up to the line end
        if (!strncmp(v, names[i], strlen(names[i])) && (v[strlen(names[i])] == 0 || strchr("\r\n \t", v[strlen(names[i])]))) return i;
    return fallback;
}

static void panel_sr(int mode) {
    static const char *const names[6] = {"off", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"};
    sr_set(mode);
    g_log("F11 panel: super resolution %s, render %ux%u", names[mode >= 0 && mode < 6 ? mode : 0], g_rw, g_rh);
    settings_save();
}
// The frame cap on rendered frames, through NVIDIA Reflex's limiter in the helper (from Rome 2's add-on). Auto (the
// default): with frame generation on, the screen's refresh rate / the frames shown per rendered frame (240 Hz, 2x: 120
// rendered, 240 shown), so every rendered frame gets its generated frames instead of the helper dropping them past the
// screen's rate (Rome 2: 166 rendered fps at 2x showed 1.29 frames per present, capped 121 showed 2.00); no cap without
// frame generation (the helper's V-Sync holds the screen's rate). No G-Sync margin: the helper presents from another
// process than the window's, and NVIDIA's driver gives windowed G-Sync only to the window's own process (AGENT_LOG 140),
// so the screen runs at its fixed rate here. Reflex counts shown frames.
static unsigned g_refresh;   // the game window's screen, Hz (0 unknown: Auto does nothing)
static unsigned g_cap_fps;   // the rendered-frame cap in effect (0 none)
static unsigned refresh_hz(HWND w) {
    MONITORINFOEXW mi;
    DEVMODEW dm;
    memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    HMONITOR m = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    if (!m || !GetMonitorInfoW(m, (MONITORINFO *)&mi) || !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;   // 0 / 1: "the hardware's default"
}
static void cap_apply(void) {
    unsigned per = g_fg + 1;
    g_cap_fps = g_fcap == 1 ? (g_fg && g_refresh ? g_refresh / per : 0) : CAP_FPS[g_fcap];
    // microseconds per shown frame, rounded up: never above the screen's rate (4167 us = 239.98 fps at 240 Hz)
    fgb_set_frame_limit(g_cap_fps ? (1000000 + g_cap_fps * per - 1) / (g_cap_fps * per) : 0);
    fgp_set_cap(g_fcap, g_cap_fps);
    if (g_cap_fps) g_log("frame cap %s: %u rendered fps, %u shown (screen %u Hz, frame generation %ux)", CAP_KEYS[g_fcap], g_cap_fps,
                         g_cap_fps * per, g_refresh, per);
    else g_log("frame cap %s: none%s", CAP_KEYS[g_fcap], g_fcap == 1 ? (g_refresh ? " (frame generation off)" : " (screen rate unknown)") : "");
}
static void panel_cap(int choice) {
    g_fcap = choice >= 0 && choice < 5 ? choice : 1;
    cap_apply();
    settings_save();
}
static void panel_fg(int n) {
    g_fg = (unsigned)n;
    fgb_set_fg((unsigned)n);
    g_log("F11 panel: frame generation %s", n ? (n == 1 ? "2x" : n == 2 ? "3x" : "4x") : "off");
    cap_apply();   // Auto and the fixed caps depend on the frames shown per rendered frame
    settings_save();
}
static void panel_motion(int on) {
    g_motion_on = on;
    if (g_motion) fgm_set_enabled(on);
    g_log("F11 panel: per-object motion %s", on ? "on" : "off");
    settings_save();
}
#define PANEL_VIEW NULL
static void panel_preset(int choice) {
    g_preset = choice >= 0 && choice < 5 ? choice : 0;
    fgb_set_preset(PRESET_NGX[g_preset]);
    g_log("F11 panel: DLSS preset %s", PRESET_KEYS[g_preset]);
    settings_save();
}
static void panel_sharp(int choice) {
    g_sharp = choice >= 0 && choice < 4 ? choice : 2;
    fgb_set_sharpness(SHARP_CON[g_sharp]);
    g_log("F11 panel: sharpness %s", SHARP_KEYS[g_sharp]);
    settings_save();
}
static void panel_vsync(int on) {
    g_vsync = on != 0;
    fgb_set_vsync(g_vsync);
    g_log("F11 panel: V-Sync %s", VSYNC_KEYS[g_vsync]);
    settings_save();
}

// Exclusive fullscreen (6.3.1, players' reports: with gfx_fullscreen true the game froze at its start screen): the game's
// swapchain sits on the hidden window, so exclusive fullscreen there covered the screen with a picture nobody draws,
// over the window the helper presents into. While the bridge runs, the game's swapchain stays windowed: its requests to
// go fullscreen are answered without switching, and it is told it is in the state it asked for (its window already has
// the fullscreen size), so the game runs as a borderless window.
static BOOL g_fs_wanted;
static HRESULT(STDMETHODCALLTYPE *r_setfs)(IDXGISwapChain *, BOOL, IDXGIOutput *);
static HRESULT(STDMETHODCALLTYPE *r_getfs)(IDXGISwapChain *, BOOL *, IDXGIOutput **);
static HRESULT STDMETHODCALLTYPE w_setfs(IDXGISwapChain *sc, BOOL fs, IDXGIOutput *target) {
    if (sc != g_hid_sc) return r_setfs(sc, fs, target);   // also after the bridge: still the hidden window
    if (fs != g_fs_wanted) g_log("frame bridge: the game switches to %s - kept as a borderless window", fs ? "fullscreen" : "windowed");
    g_fs_wanted = fs;
    return S_OK;
}
// Other mods find the game's window through its swapchain (the 4K UI mod's F10 menu showed but took no clicks: it
// subclassed the hidden window, which gets no input). The hidden swapchain says the window the game gave it.
static HRESULT(STDMETHODCALLTYPE *r_getdesc)(IDXGISwapChain *, DXGI_SWAP_CHAIN_DESC *);
static HRESULT(STDMETHODCALLTYPE *r_gethwnd)(IDXGISwapChain1 *, HWND *);
static HRESULT STDMETHODCALLTYPE w_getdesc(IDXGISwapChain *sc, DXGI_SWAP_CHAIN_DESC *d) {
    HRESULT hr = r_getdesc(sc, d);
    if (SUCCEEDED(hr) && d && sc == g_hid_sc && g_out_hwnd) d->OutputWindow = g_out_hwnd;
    return hr;
}
static HRESULT STDMETHODCALLTYPE w_gethwnd(IDXGISwapChain1 *sc, HWND *h) {
    HRESULT hr = r_gethwnd(sc, h);
    if (SUCCEEDED(hr) && h && (IDXGISwapChain *)sc == g_hid_sc && g_out_hwnd) *h = g_out_hwnd;
    return hr;
}
static HRESULT STDMETHODCALLTYPE w_getfs(IDXGISwapChain *sc, BOOL *fs, IDXGIOutput **target) {
    if (sc != g_hid_sc) return r_getfs(sc, fs, target);
    if (fs) *fs = g_fs_wanted;
    if (target) {   // the monitor the window is on, as a fullscreen swapchain would report it
        *target = NULL;
        if (g_fs_wanted && FAILED(IDXGISwapChain_GetContainingOutput(sc, target))) *target = NULL;
    }
    return S_OK;
}

// ReShade's wrapper reads the real swapchain's window for its menu's keys (its runtime: get_hwnd at start): the hidden
// window gets none, so the real GetDesc says the game's window for it, like the wrapper's does for the game and other mods
static HRESULT(STDMETHODCALLTYPE *r_getdesc_real)(IDXGISwapChain *, DXGI_SWAP_CHAIN_DESC *);
static HRESULT STDMETHODCALLTYPE w_getdesc_real(IDXGISwapChain *sc, DXGI_SWAP_CHAIN_DESC *d) {
    HRESULT hr = r_getdesc_real(sc, d);
    if (SUCCEEDED(hr) && d && g_hid_hwnd && d->OutputWindow == g_hid_hwnd && g_out_hwnd) d->OutputWindow = g_out_hwnd;
    return hr;
}
// rslink.cpp, from ReShade's init_swapchain event (inside the game's CreateSwapChain, before ReShade starts its runtime):
// the real swapchain; Present and GetDesc hooked on its table (all real swapchains share it), once
static int rs_swapchain(void *real) {
    if (!g_rs_creating || !real) return 0;
    g_rs_new = (IDXGISwapChain *)real;
    void **vt = *(void ***)real;
    if (!r_present) {
        patch_once(&vt[offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *)], (void *)w_present, (void **)&r_present);
        if (!g_present_real) g_present_real = dxgi_own(vt, offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *), (void *)r_present, "Present");
    }
    patch_once(&vt[offsetof(IDXGISwapChainVtbl, GetDesc) / sizeof(void *)], (void *)w_getdesc_real, (void **)&r_getdesc_real);
    return 1;
}

static HRESULT STDMETHODCALLTYPE w_create_sc(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out) {
    mp_device(dev);
    DXGI_SWAP_CHAIN_DESC dd = *d;
    if (g_dev) {   // a settings change may come with a new D3D11 device
        static const GUID iid_d11 = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};
        ID3D11Device *nd = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface(dev, &iid_d11, (void **)&nd))) {
            if (nd != g_dev) g_log("frame bridge: the game's new swapchain is on another D3D11 device than its first");
            ID3D11Device_Release(nd);
        }
    }
    if (g_live && g_live_state == 1) {   // shortcut: a second swapchain (settings change) ends the bridge; restart the game to get it back
        g_log("frame bridge: the game makes a new swapchain (%ux%u) - DLSS off until the game restarts", d->BufferDesc.Width, d->BufferDesc.Height);
        g_off_note = L"Graphics settings changed - restart the game to turn DLSS back on. Choices made here are saved.";
        bridge_off();
    }
    if (g_live && g_live_state == 0) {
        static const GUID iid_dev = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};   // ID3D11Device
        RECT rc = {0};
        GetClientRect(d->OutputWindow, &rc);
        g_live_w = d->BufferDesc.Width ? d->BufferDesc.Width : (UINT)rc.right;
        g_live_h = d->BufferDesc.Height ? d->BufferDesc.Height : (UINT)rc.bottom;
        HWND hid = NULL;
        if (!g_ctx && SUCCEEDED(IUnknown_QueryInterface(dev, &iid_dev, (void **)&g_dev))) ID3D11Device_GetImmediateContext(g_dev, &g_ctx);
        g_live_state = g_ctx && (hid = hidden_window(g_live_w, g_live_h)) &&
                               fgb_start(g_dev, g_ctx, d->OutputWindow, g_live_w, g_live_h, d->BufferDesc.Format, g_helper, g_sl, g_logdir, g_fg, g_log)
                           ? 1 : 2;
        if (g_live_state != 1) g_motion = 0;   // bridge off (e.g. not an NVIDIA GPU): no per-stage draw hooks either
        if (g_live_state == 1) {
            dd.OutputWindow = hid; dd.BufferDesc.Width = g_live_w; dd.BufferDesc.Height = g_live_h;
            if (!dd.Windowed) {   // see w_setfs: exclusive fullscreen on the hidden window would cover the screen
                dd.Windowed = TRUE;
                g_fs_wanted = TRUE;
                g_log("frame bridge: the game asked for exclusive fullscreen - it runs as a borderless window while the add-on is on");
            }
            g_out_hwnd = d->OutputWindow;
            if (g_fg > fgb_fg_max()) g_fg = fgb_fg_max();   // NVIDIA's rules for this GPU (fgb_start logged it)
            g_refresh = refresh_hz(d->OutputWindow);   // the frame cap's Auto (the screen the game window is on)
            cap_apply();
            sr_set(g_sr);
            fgb_set_preset(PRESET_NGX[g_preset]);
            fgb_set_sharpness(SHARP_CON[g_sharp]);
            fgb_set_vsync(g_vsync);
            if (!g_vsync && !fgb_helper_has_vsync()) g_log("DLSS add-on: V-Sync off needs add-on version 7 - its helper keeps V-Sync on");
            if (!fgb_helper_has_presets())   // the engine fix updated, the add-on not (or a hand install of an older one)
                g_log("DLSS add-on: its helper is older than this engine fix (add-on version 4 or older) - the DLSS preset choice "
                      "needs the add-on updated; everything else works");
            jit_install();   // passes viewports through unless DLAA is on (F11 panel)
            int no_fg = g_motion && !fgb_fg_max();
            if (no_fg) {   // RTX 20 / 30: per-object motion mostly serves frame generation; it would only cost fps there
                g_motion = 0;
                g_log("motion pass: off - this GPU has no frame generation, which per-object motion is for");
            }
            if (g_motion) {
                // the game's instance record copy (FUN_117dab80): call memcpy at +0x17DABAF -> fgm_copy
                unsigned char *call = (unsigned char *)(UINT_PTR)(g_pt_base + game_rva((void *)(UINT_PTR)g_pt_base, 0x17DABAF));
                unsigned target = g_pt_base + game_rva((void *)(UINT_PTR)g_pt_base, 0x1A27CA0);
                DWORD prot;
                if (call[0] != 0xE8 || (unsigned)(UINT_PTR)call + 5 + *(int *)(call + 1) != target) {
                    g_log("motion pass: the record copy call at +0x17DABAF is not the expected one - off");
                } else if (fgm_init(g_dev, g_ctx, fgb_mv_texture(), g_live_w, g_live_h, g_log) &&
                           VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &prot)) {
                    *(int *)(call + 1) = (int)((UINT_PTR)fgm_copy - ((UINT_PTR)call + 5));
                    VirtualProtect(call, 5, prot, &prot);
                    FlushInstructionCache(GetCurrentProcess(), call, 5);
                    fgb_set_motion(fgm_pass);
                    g_copy_patched = 1;
                    fgm_set_enabled(g_motion_on);
                    mp_hook(NULL);
                } else g_motion = 0;
            }
            FgpApply ap = {panel_sr, panel_fg, panel_motion, panel_cap, panel_preset, panel_sharp, PANEL_VIEW, panel_vsync};
            if (!fgp_init(g_dev, g_ctx, g_live_w, g_live_h, d->BufferDesc.Format, g_sr, (int)g_fg, g_motion ? g_motion_on : no_fg ? -2 : -1, ap))
                g_log("frame bridge: F11 panel not available (back buffer format %u)", d->BufferDesc.Format);
            else {
                fgp_set_preset(g_preset);
                fgp_set_sharp(g_sharp);
                fgp_set_presets_available(fgb_helper_has_presets());
                fgp_set_vsync(g_vsync);
                fgp_set_vsync_available(fgb_helper_has_vsync());
            }
        }
        else if (hid) DestroyWindow(hid);
    }
    // After the bridge: a swapchain on the game's window after the helper's is never shown (fgbridge.c, fallback), so the
    // game's new one (settings change) goes to a hidden window too and its frames reach the window as after a helper crash
    if (g_live_state == 2 && g_out_hwnd && d->OutputWindow == g_out_hwnd) {
        RECT rc = {0};
        GetClientRect(d->OutputWindow, &rc);
        UINT w = d->BufferDesc.Width ? d->BufferDesc.Width : (UINT)rc.right, h = d->BufferDesc.Height ? d->BufferDesc.Height : (UINT)rc.bottom;
        HWND hid = w && h ? hidden_window(w, h) : NULL;
        if (hid) {
            dd.OutputWindow = hid; dd.BufferDesc.Width = w; dd.BufferDesc.Height = h;
            g_fs_wanted = !d->Windowed;   // as with the bridge: fullscreen is kept as a borderless window
            dd.Windowed = TRUE;
        }
    }
    fgb_fallback_close();   // ours belongs to the previous swapchain
    g_hid_sc = NULL;
    g_hid_hwnd = dd.OutputWindow != d->OutputWindow ? dd.OutputWindow : NULL;
    g_rs_creating = g_rs; g_rs_new = NULL;   // ReShade tells the real swapchain under its wrapper (rs_swapchain) while making it
    HRESULT hr = r_create_sc(f, dev, &dd, out);
    g_rs_creating = 0;
    int hidden = SUCCEEDED(hr) && dd.OutputWindow != d->OutputWindow;
    if (FAILED(hr) && g_live_state == 1) { g_log("frame bridge: game swapchain on the hidden window failed (0x%08lx) - bridge off", hr); bridge_off(); }
    if (FAILED(hr) && dd.OutputWindow != d->OutputWindow) hr = r_create_sc(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out) {
        g_sc = *out;
        g_scd = *d;
        if (hidden) g_hid_sc = *out;
        if (g_rs_new && (void *)g_rs_new != (void *)*out) {
            g_rs_sc = g_rs_new;
            g_log("ReShade: the game's swapchain is ReShade's wrapper - the bridge presents from under it (ReShade's menu and effects first)");
        }
        if (hidden && g_live_state == 2) g_log("frame bridge: the new swapchain is on a hidden window too; the mod shows its frames");
        void **vt = *(void ***)*out;
        // with ReShade: Present is already hooked on the real swapchain (rs_swapchain), so patch_once leaves the wrapper's
        patch_once(&vt[offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *)], (void *)w_present, (void **)&r_present);
        if (!g_present_real) g_present_real = dxgi_own(vt, offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *), (void *)r_present, "Present");
        if (hidden) {
            patch_once(&vt[offsetof(IDXGISwapChainVtbl, SetFullscreenState) / sizeof(void *)], (void *)w_setfs, (void **)&r_setfs);
            patch_once(&vt[offsetof(IDXGISwapChainVtbl, GetFullscreenState) / sizeof(void *)], (void *)w_getfs, (void **)&r_getfs);
            patch_once(&vt[offsetof(IDXGISwapChainVtbl, GetDesc) / sizeof(void *)], (void *)w_getdesc, (void **)&r_getdesc);
            static const GUID iid_sc1 = {0x790a45f7, 0x0d42, 0x4876, {0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa}};
            IDXGISwapChain1 *s1 = NULL;   // GetHwnd: only where IDXGISwapChain1 is the same object (same table, one more slot)
            if (SUCCEEDED(IDXGISwapChain_QueryInterface(*out, &iid_sc1, (void **)&s1))) {
                if ((void *)s1 == (void *)*out)
                    patch_once(&vt[offsetof(IDXGISwapChain1Vtbl, GetHwnd) / sizeof(void *)], (void *)w_gethwnd, (void **)&r_gethwnd);
                IDXGISwapChain1_Release(s1);
            }
        }
    }
    return hr;
}

typedef HRESULT(WINAPI *factory_fn)(REFIID, void **);
static factory_fn r_factory;
static HRESULT WINAPI w_factory(REFIID r, void **out) {
    HRESULT hr = r_factory(r, out);
    if (SUCCEEDED(hr) && out && *out) {
        void **vt = *(void ***)*out;
        patch_once(&vt[offsetof(IDXGIFactoryVtbl, CreateSwapChain) / sizeof(void *)], (void *)w_create_sc, (void **)&r_create_sc);
        if (!g_create_real) g_create_real = dxgi_own(vt, offsetof(IDXGIFactoryVtbl, CreateSwapChain) / sizeof(void *), (void *)r_create_sc, "CreateSwapChain");
    }
    return hr;
}

typedef HRESULT(WINAPI *create_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                   ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
static create_fn r_create;
static HRESULT WINAPI w_create(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL *fl, UINT nfl,
                               UINT sdk, ID3D11Device **dev, D3D_FEATURE_LEVEL *got, ID3D11DeviceContext **ctx) {
    HRESULT hr = r_create(a, t, sw, flags, fl, nfl, sdk, dev, got, ctx);
    g_creates++;
    // the first device only: the game (or a video player / overlay, 2026-10-28 campaign run: 4 more devices after the
    // battle) creates more later, and taking those made every context check miss the game's real context
    if (SUCCEEDED(hr) && dev && *dev && !(flags & D3D11_CREATE_DEVICE_SINGLETHREADED) && !g_ctx) {
        if (g_ctx) ID3D11DeviceContext_Release(g_ctx);
        if (g_dev) ID3D11Device_Release(g_dev);
        if (g_staging) ID3D11Texture2D_Release(g_staging);
        g_staging = NULL;
        g_dev = *dev;
        ID3D11Device_AddRef(g_dev);
        ID3D11Device_GetImmediateContext(g_dev, &g_ctx);
    }
    return hr;
}

static FARPROC(WINAPI *r_gpa)(HMODULE, LPCSTR);
// ReShade in the game folder (as dxgi.dll or d3d11.dll): the game's first look-up in it links the mod as a ReShade add-on
// (DLSS add-on on only: without the bridge ReShade works as it always did)
static void rs_find(HMODULE m) {
    if (g_rs_tried || !g_live || !r_gpa(m, "ReShadeRegisterAddon")) return;
    g_rs_tried = 1;
    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)rs_find, &self);
    wchar_t state[MAX_PATH];   // the far plane the mod wrote last (ReShade saves it in the player's preset)
    _snwprintf(state, MAX_PATH, L"%ls\\reshade_link.txt", g_logdir);
    g_rs = rsl_attach(self, m, g_log, rs_swapchain, state);
}
static FARPROC WINAPI w_gpa(HMODULE m, LPCSTR name) {
    FARPROC f = r_gpa(m, name);
    if (!f || (UINT_PTR)name <= 0xFFFF) return f;
    if (!strcmp(name, "D3D11CreateDevice") || !strcmp(name, "CreateDXGIFactory1")) rs_find(m);
    if (!strcmp(name, "D3D11CreateDevice")) { r_create = (create_fn)(void *)f; return (FARPROC)(void *)w_create; }
    if (!strcmp(name, "CreateDXGIFactory1")) { r_factory = (factory_fn)(void *)f; return (FARPROC)(void *)w_factory; }
    return f;
}

// AGENT_LOG 109: an experiment that put d3d11's functions back in the hooked slots found most of them still calling
// ours: another hook sits on top and calls ours as its original. Once per run: who holds each slot.
static void hook_owners(void (*log)(const char *fmt, ...)) {
    if (!g_ctx || !r_omrt) return;
    void **vt = *(void ***)g_ctx;
#define SL(m) &vt[offsetof(ID3D11DeviceContextVtbl, m) / sizeof(void *)], #m
    struct { void **slot; const char *name; void *ours; } h[] = {
        {SL(RSSetViewports), (void *)w_rsvp}, {SL(RSGetViewports), (void *)w_rsgetvp}, {SL(OMSetRenderTargets), (void *)w_omrt},
        {SL(OMSetRenderTargetsAndUnorderedAccessViews), (void *)w_omrtuav}, {SL(OMSetDepthStencilState), (void *)w_omdss},
        {SL(PSSetConstantBuffers), (void *)w_pscb}, {SL(Map), (void *)w_map}, {SL(Unmap), (void *)w_unmap},
        {SL(UpdateSubresource), (void *)w_updsub}, {SL(PSSetSamplers), (void *)w_pssamp}, {SL(DrawIndexedInstanced), (void *)w_dii}, {SL(DrawIndexed), (void *)w_di},
        {SL(DrawInstanced), (void *)w_dinst}, {SL(Draw), (void *)w_draw}};
#undef SL
    char line[1500];
    int len = 0;
    for (unsigned i = 0; i < sizeof h / sizeof *h; i++) {
        char buf[MAX_PATH];
        len += snprintf(line + len, sizeof line - len, "%s %s: %s", i ? "," : "", h[i].name, *h[i].slot == h[i].ours ? "ours" : module_name(*h[i].slot, buf));
    }
    log("  D3D11 context slots the add-on hooks, who is first in each now:%s", line);
    int __cdecl fgb_state(FgbState *s);
    void *__cdecl fgb_d3d11_original(unsigned slot);
    FgbState st = {0};
    st.size = sizeof st;
    fgb_state(&st);
    unsigned ok = 0;
    for (unsigned i = 10; i < sizeof h / sizeof *h; i++) ok += fgb_d3d11_original((unsigned)(h[i].slot - vt)) != NULL;   // the 4 draws
    log("  for other mods: fgb_state running %u, mode %u, render %ux%u of %ux%u, frame generation %u; fgb_d3d11_original: "
        "d3d11's own function for %u of 4 draw slots", st.running, st.sr_mode, st.render_w, st.render_h, st.tex_w, st.tex_h,
        st.frame_generation, ok);
}

// For other mods (fgb_motion_api.h; a UI / effects mod's temporal effects, so players do not pay for a second
// per-object pass): this frame's motion vectors, made at the first call. NULL when the bridge is not running, this
// frame has no 3D scene camera yet, or it is a native-size frame while DLSS SR is on (no vectors made for it).
ID3D11Texture2D *__cdecl fgb_motion_vectors(FgbMotionInfo *info) {
    if (!info || info->size < sizeof *info) return NULL;
    info->version = FGB_MOTION_API_VERSION;
    info->copy_hook = g_copy_patched;
    if (GetCurrentThreadId() != g_pt_main) return NULL;
    g_mvx_calls++;
    if (g_mvx_last != g_frame) { g_mvx_last = g_frame; g_mvx_frames++; }
    ID3D11Texture2D *t = fgb_mv_texture();
    if (g_live_state != 1 || !fgb_active() || !g_main_depth || !t || g_vp_frame != g_frame || (g_sr && !g_frame_scaled) ||
        !fgb_make_vectors(g_main_depth, g_vp)) { g_mvx_none++; return NULL; }
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(t, &d);
    info->frame = fgb_frames();
    info->per_object = g_motion && g_motion_on;   // the player's choice in the F11 panel decides, for both mods
    info->render_w = g_sr ? g_rw : d.Width; info->render_h = g_sr ? g_rh : d.Height;
    info->tex_w = d.Width; info->tex_h = d.Height;
    info->jitter_x = g_sr ? g_jx : 0; info->jitter_y = g_sr ? g_jy : 0;
    ID3D11Texture2D_AddRef(t);
    return t;
}

// For other mods (fgb_motion_api.h): the state above without making vectors (the 4K UI mod's passes need the render size)
int __cdecl fgb_state(FgbState *s) {
    if (!s || s->size < sizeof *s) return 0;
    s->version = FGB_STATE_VERSION;
    s->running = g_live_state == 1 && fgb_active();
    s->sr_mode = s->running ? (unsigned)g_sr : 0;
    s->tex_w = g_live_w; s->tex_h = g_live_h;
    s->render_w = s->running && g_sr ? g_rw : g_live_w; s->render_h = s->running && g_sr ? g_rh : g_live_h;
    s->scaled = s->running && g_jit_on && (g_rw != g_live_w || g_rh != g_live_h);
    s->jitter_x = s->running && g_sr ? g_jx : 0; s->jitter_y = s->running && g_sr ? g_jy : 0;
    s->frame_generation = s->running ? g_fg : 0;
    s->per_object = g_motion && g_motion_on;
    s->frame = fgb_frames();
    return 1;
}

// For other mods (fgb_motion_api.h): d3d11's own function for a context vtable slot - what the add-on's hook there calls
void *__cdecl fgb_d3d11_original(unsigned slot) {
    if (!g_ctx || slot >= sizeof(ID3D11DeviceContextVtbl) / sizeof(void *)) return NULL;
#define HK(m, w, r) {offsetof(ID3D11DeviceContextVtbl, m) / sizeof(void *), (void *)w, (void **)&r}
    static const struct { size_t slot; void *ours; void **orig; } hooked[] = {
        HK(RSSetViewports, w_rsvp, r_rsvp), HK(RSGetViewports, w_rsgetvp, r_rsgetvp), HK(OMSetRenderTargets, w_omrt, r_omrt),
        HK(OMSetRenderTargetsAndUnorderedAccessViews, w_omrtuav, r_omrtuav), HK(OMSetDepthStencilState, w_omdss, r_omdss),
        HK(PSSetConstantBuffers, w_pscb, r_pscb), HK(Map, w_map, r_map), HK(Unmap, w_unmap, r_unmap),
        HK(UpdateSubresource, w_updsub, r_updsub), HK(PSSetSamplers, w_pssamp, r_pssamp), HK(DrawIndexedInstanced, w_dii, r_dii), HK(DrawIndexed, w_di, r_di),
        HK(DrawInstanced, w_dinst, r_dinst), HK(Draw, w_draw, r_draw)};
#undef HK
    void *fn = (*(void ***)g_ctx)[slot];
    for (unsigned i = 0; i < sizeof hooked / sizeof *hooked; i++)
        if (hooked[i].slot == slot && fn == hooked[i].ours) fn = *hooked[i].orig;
    HMODULE owner = NULL, d3d = GetModuleHandleW(L"d3d11.dll");
    if (!fn || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)fn, &owner))
        return NULL;
    return owner == d3d ? fn : NULL;
}

static void bp_report(void (*log)(const char *fmt, ...)) {
    log("bridge probe: %u presents (%u not on the main thread), sync interval 0/1/2/3/more: %u/%u/%u/%u/%u, captures %d%s",
        g_presents, g_present_other_thread, g_sync[0], g_sync[1], g_sync[2], g_sync[3], g_sync[4], g_captures,
        g_sc ? "" : " - no swapchain seen");
    log("  D3D11CreateDevice calls seen %d, device %s", g_creates, !g_ctx ? "not known yet" : g_dev_from_sc ? "taken from the swapchain" : "from D3D11CreateDevice");
    if (g_live)
        log("  frame bridge %s: %u frames handed over, %u pacing timeouts, HUD-less copies %u, native frames %u, no camera %u, no depth %u",
            g_live_state == 1 ? "running" : g_live_state == 2 ? "off" : "not started", fgb_frames(), fgb_pace_timeouts(), g_br_hud,
            g_br_native, g_br_novp, g_br_nodepth);
    if (g_fb_frames || g_nested[0] || g_nested[1] || g_nested[2])
        log("  fallback: %u frames presented through the mod's own swapchain, %u not S_OK (last 0x%08lX); nested Presents: "
            "game's %u, mod's own %u, other %u (each sent straight to DXGI)", g_fb_frames, g_fb_notok, (unsigned long)g_fb_last, g_nested[0], g_nested[1], g_nested[2]);
    if (g_rs) log("  ReShade: effects before the UI in %u frames%s", g_rs_frames, g_rs_sc ? "" : " (the game's swapchain was not ReShade's wrapper)");
    if (g_sr) {
        unsigned sf, st; double sa, sm;
        void fgb_sr_stats(unsigned *, unsigned *, double *, double *);
        fgb_sr_stats(&sf, &st, &sa, &sm);
        log("  DLSS render %ux%u: %u frames jittered / scaled, viewport calls seen %u (%u changed); camera blocks %u, %u pixel shader "
            "copies written; SR round trips %u, game CPU wait avg %.3f ms max %.3f ms, %u timeouts",
            g_rw, g_rh, g_jit_frames, g_jit_vp_calls, g_jit_vp_jittered, g_ncbuf, g_cam_writes, sf, sa, sm, st);
        g_jit_frames = g_jit_vp_calls = g_jit_vp_jittered = g_cam_writes = 0;
        unsigned nb = 0;
        for (unsigned i = 0; i < g_nlod; i++) nb += g_lod[i].ours != NULL;
        if (g_nlod) log("  texture mip bias %.2f: %u of %u game samplers biased, %u sampler binds swapped%s", g_lod_bias, nb, g_nlod,
                        g_lod_swaps, g_lod_full ? ", table full" : "");
        g_lod_swaps = 0;
        if (g_ps_bloom_ds) log("  bloom source pass with the jitter taken out: %u draws%s", g_bloom_draws, g_bloom_off ? " (off)" : "");
        g_bloom_draws = 0;
        unsigned fgb_sharp_frames(void);
        static unsigned sharp0;
        unsigned sh = fgb_sharp_frames();
        log("  sharpness %s: %u frames sharpened; DLSS preset %s%s; V-Sync %s%s", SHARP_KEYS[g_sharp], sh - sharp0, PRESET_KEYS[g_preset],
            fgb_helper_has_presets() ? "" : " (the add-on's helper is too old to apply it)", VSYNC_KEYS[g_vsync],
            g_vsync || fgb_helper_has_vsync() ? "" : " (the add-on's helper keeps it on)");
        sharp0 = sh;
    }
    if (g_live_state == 1) {
        void fgb_cpu_stats(double *, double *, double *, unsigned *);
        double hm, pm, wm;
        unsigned fr;
        fgb_cpu_stats(&hm, &pm, &wm, &fr);
        if (fr) log("  frame bridge main-thread time per frame: HUD-less step %.3f ms (incl. the DLSS pickup and the motion pass), "
                    "Present step %.3f ms, of which waiting for the helper %.3f ms", hm / fr, pm / fr, wm / fr);
        void fgb_gpu_stats(double ms[4], unsigned *frames, unsigned *lost);
        double g4[4];
        unsigned gf, gl;
        fgb_gpu_stats(g4, &gf, &gl);
        void fgb_wait_stats(unsigned *queued, unsigned *frames);
        unsigned wq, wf;
        fgb_wait_stats(&wq, &wf);
        if (gf) log("  frame bridge GPU per frame (%u frames timed, %u not read): frame interval %.3f ms, a wait for the helper queued in %u of %u "
                    "frames (stalled %.3f ms each), HUD-less step %.3f ms (copies, motion vectors, motion pass, DLSS pickup), Present step %.3f ms",
                    gf, gl, g4[0], wq, wf, g4[1], g4[2], g4[3]);
        if (fr) log("  game calls through the add-on's hooks per frame: Map %.0f, Unmap %.0f, UpdateSubresource %.0f, PSSetConstantBuffers %.0f, "
                    "OMSetRenderTargets %.0f, OMSetDepthStencilState %.0f, RSSetViewports %.0f, DrawIndexedInstanced %.0f",
                    (double)g_hk[HK_MAP] / fr, (double)g_hk[HK_UNMAP] / fr, (double)g_hk[HK_UPDSUB] / fr, (double)g_hk[HK_PSCB] / fr,
                    (double)g_hk[HK_OMRT] / fr, (double)g_hk[HK_OMDSS] / fr, (double)g_hk[HK_RSVP] / fr, (double)g_hk[HK_DRAW] / fr);
        memset(g_hk, 0, sizeof g_hk);
        {   // TSC rate from QPC between reports, then the time in the real Map / OMSetRenderTargets
            static LARGE_INTEGER q0; static unsigned long long c0;
            LARGE_INTEGER q, qf; unsigned long long cn = __rdtsc();
            QueryPerformanceCounter(&q); QueryPerformanceFrequency(&qf);
            if (q0.QuadPart && q.QuadPart > q0.QuadPart) g_tsc_ms = (cn - c0) * (unsigned long long)qf.QuadPart / 1000 / (unsigned long long)(q.QuadPart - q0.QuadPart);
            q0 = q; c0 = cn;
        }
        if (fr && g_tsc_ms) {
            char sl[900];
            int len = 0;
            for (int t = 0; t < 5; t++) {
                int best = -1;
                for (unsigned i = 0; i < g_nslow; i++) if (g_slow[i].n && (best < 0 || g_slow[i].ticks > g_slow[best].ticks)) best = (int)i;
                if (best < 0) break;
                SlowMap *k = &g_slow[best];
                len += snprintf(sl + len, sizeof sl - len, "%s map type %u flags 0x%X on %s usage %u bind 0x%X cpu 0x%X %s %u: %u calls %.3f ms per frame",
                                t ? ";" : "", k->type, k->flags, k->dim == 1 ? "buffer" : k->dim == 3 ? "texture2d" : "other", k->usage, k->bind, k->cpu,
                                k->dim == 3 ? "size (w<<16|h)" : "bytes", k->size, k->n, (double)k->ticks / g_tsc_ms / fr);
                k->n = 0;
            }
            log("  main thread in the game's D3D11 calls per frame: Map %.3f ms, OMSetRenderTargets %.3f ms; Maps over 0.2 ms:%s",
                (double)g_map_ticks / g_tsc_ms / fr, (double)g_omrt_ticks / g_tsc_ms / fr, len ? sl : " none");
        }
        g_map_ticks = g_omrt_ticks = 0;
        g_nslow = 0;
        static int owners_done;
        if (!owners_done++) hook_owners(log);
        log("  set-camera hook calls per frame: %.0f", (double)g_cam_calls / fr);
        g_cam_calls = 0;
    }
    if (g_motion) fgm_report(log);
    if (g_mvx_calls) {
        log("  motion vectors for other mods: asked in %u frames (%u calls), none given %u times", g_mvx_frames, g_mvx_calls, g_mvx_none);
        g_mvx_frames = g_mvx_calls = g_mvx_none = 0;
    }
    for (int i = 0; i < 8 && g_callers[i].n; i++) {
        HMODULE mod = NULL;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(UINT_PTR)g_callers[i].ret, &mod);
        GetModuleFileNameA(mod, name, MAX_PATH);
        const char *b = strrchr(name, '\\');
        log("  Present called from %s+0x%X, first game return address on the stack +0x%X: %u times", b ? b + 1 : name,
            g_callers[i].ret - (unsigned)(UINT_PTR)mod, g_callers[i].game_ret, g_callers[i].n);
    }
}

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
    memcpy(g_cam_tramp, f, 5);
    g_cam_tramp[5] = 0xE9;
    *(int *)(g_cam_tramp + 6) = (int)((f + 5) - (g_cam_tramp + 10));
    DWORD prot;
    if (!VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &prot)) return 0;
    f[0] = 0xE9;
    *(int *)(f + 1) = (int)((unsigned char *)cam_stub - (f + 5));
    VirtualProtect(f, 5, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), f, 5);
    return 1;
}

int install_bridgeprobe(HMODULE m, const wchar_t *dir, void (*log)(const char *fmt, ...)) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((unsigned char *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    g_mod_size = nt->OptionalHeader.SizeOfImage;
    wcsncpy(g_dir, dir, MAX_PATH - 1);
    g_vt_main = game_rva(m, 0x1CCE4E4); g_vt_hud = game_rva(m, 0x1C9A900); g_vt_overlay = game_rva(m, 0x1CA0858);
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
    g_log = log;
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\fgbridge_on.txt", g_dir);
    wcscpy(g_logdir, g_dir);
    FILE *f = _wfopen(p, L"r");
    wchar_t addon[MAX_PATH];
    _snwprintf(addon, MAX_PATH, L"%ls\\FrameBridge", g_dir);
    _snwprintf(p, MAX_PATH, L"%ls\\helper64.exe", addon);
    if (!f && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
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
        g_motion_on = settings_value(text, "motion_vectors=", MV_KEYS, 2, 0);  // default camera only
        g_fcap = settings_value(text, "frame_cap=", CAP_KEYS, 5, 1);   // default auto
        g_preset = settings_value(text, "dlss_preset=", PRESET_KEYS, 5, 0);   // default: NVIDIA's per mode
        g_sharp = settings_value(text, "sharpness=", SHARP_KEYS, 4, 2);       // default medium (files without the key too)
        g_vsync = settings_value(text, "vsync=", VSYNC_KEYS, 2, 1);           // default on (files without the key too)
        // Every first start saved the defaults, so a changed default is applied once to files from before it: per_object
        // before version=2 (players' reports: half the fps in big battles on 6-core CPUs; it mostly helps frame
        // generation), DLAA before version=3 (Quality renders smaller: faster big battles)
        int ver = !sf ? 3 : strstr(text, "version=3") ? 3 : strstr(text, "version=2") ? 2 : 1;
        int reset_mv = ver < 2 && g_motion_on, reset_sr = ver < 3 && g_sr == 1;
        if (reset_mv) g_motion_on = 0;
        if (reset_sr) g_sr = 2;
        g_live = 1;
        g_motion = 1;
        log("DLSS add-on found: super resolution %s, frame generation %s, motion vectors %s, frame cap %s, preset %s, sharpness %s, V-Sync %s (%ls%s%s)",
            SR_KEYS[g_sr], FG_KEYS[g_fg], MV_KEYS[g_motion_on], CAP_KEYS[g_fcap], PRESET_KEYS[g_preset], SHARP_KEYS[g_sharp], VSYNC_KEYS[g_vsync],
            sf ? L"settings.txt" : L"defaults",
            reset_mv ? "; per_object was the old default, now camera_only" : "", reset_sr ? "; dlaa was the old default, now quality" : "");
        if (!sf || ver < 3) settings_save();
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
        g_motion = g_live;   // set up whenever the bridge runs, so the F11 panel can switch it on
        log("frame bridge %s: helper %s, Streamline %s, frame generation %ux, DLSS %s", g_live ? "armed" : "NOT armed (bad fgbridge_on.txt)",
            l[0], l[1], g_fg + 1, (const char *[]){"off", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"}[g_sr]);
        log("  per-object motion vectors: %s (F11 panel switches it)", g_motion_on ? "on" : "off");
        if (g_live) g_dev_di8_created = fgp_di8_created;   // the panel hides the game's mouse buttons while the cursor is over it
    }
    if (g_live && cam_hook(m, log)) log("frame bridge: camera read through a jump hook on FUN_1177bb10");
    log("bridge probe installed: Present, render targets per stage, back buffer dumps and camera blocks on bridge_go.txt");
    return 1;
}
