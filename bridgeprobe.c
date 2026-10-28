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
#include <dxgi.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

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
void fgb_set_sr(int on, UINT render_w, UINT render_h);
void fgb_jitter(float *jx, float *jy);
static void jit_frame_start(void), jit_frame_end(void);   // DLAA viewport jitter (below)
// per-object motion (fgmotion.c, AGENT_LOG 99): fgbridge_on.txt line 4 contains "motion"
static int g_motion;              // the motion pass is set up (bridge mode); on / off: g_motion_on and the F12 panel
static int g_motion_on;
// the F12 panel (fgpanel.c)
typedef struct { void (*sr)(int mode); void (*fg)(int frames); void (*motion)(int on); } FgpApply;
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
int fgm_init(ID3D11Device *, ID3D11DeviceContext *, ID3D11Texture2D *mv, UINT w, UINT h, void (*log)(const char *fmt, ...));
void fgm_layout_created(const void *il, const D3D11_INPUT_ELEMENT_DESC *e, UINT n, unsigned long long sig);
void fgm_mapped(ID3D11Resource *r, const D3D11_MAPPED_SUBRESOURCE *m);
void *__cdecl fgm_copy(void *dst, const void *src, size_t n);
void fgm_draw(ID3D11DeviceContext *c, UINT idx, UINT inst, UINT start_idx, INT base_v, UINT start_inst);
void fgm_pass(ID3D11Resource *depth, float jx, float jy);
void fgm_frame_end(void);
void fgm_report(void (*log)(const char *fmt, ...));
void fgm_dump_next(const wchar_t *path);
static void mp_capture_start(void), mp_capture_end(FILE *f), mp_hook(const char *where);   // motion probe (below)
static int g_live, g_live_state;   // state: 0 not started, 1 running, 2 off for good
static wchar_t g_helper[MAX_PATH], g_sl[MAX_PATH], g_logdir[MAX_PATH];
static wchar_t g_settings[MAX_PATH];   // the add-on's settings.txt (the F12 panel saves to it); empty in dev runs
static unsigned g_fg, g_live_w, g_live_h, g_vp_frame = ~0u, g_hud_frame = ~0u, g_br_hud, g_br_novp, g_br_nodepth;
static float g_vp[16];

static void (*g_log)(const char *fmt, ...);

// Present statistics (reported every 30 s)
static unsigned g_presents, g_present_other_thread, g_sync[5];
static struct { unsigned ret, game_ret, n; } g_callers[8];
static HRESULT(STDMETHODCALLTYPE *r_present)(IDXGISwapChain *, UINT, UINT);
static HRESULT(STDMETHODCALLTYPE *r_create_sc)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);

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
        if (g_live && ok && vt == 0x1CCE4E4) { memcpy(g_vp, cam + 0x1B0, 64); g_vp_frame = g_frame; }
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
    if (g_live_state == 1 && id == g_id_stage && vt == 0x1C9A900 && g_hud_frame != g_frame && fgb_active()) {
        g_hud_frame = g_frame;
        jit_frame_end();
        ID3D11DepthStencilView *dsv = NULL;
        ID3D11Resource *depth = NULL, *bb = backbuffer();
        ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 0, NULL, &dsv);
        if (dsv) { ID3D11DepthStencilView_GetResource(dsv, &depth); ID3D11DepthStencilView_Release(dsv); }
        if (!depth) g_br_nodepth++;
        else if (g_vp_frame != g_frame) g_br_novp++;
        else if (bb) { fgb_hudless(bb, depth, g_vp); g_br_hud++; }
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
}

static HRESULT STDMETHODCALLTYPE w_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    g_presents++;
    if (GetCurrentThreadId() != g_pt_main) g_present_other_thread++;
    g_sync[sync < 4 ? sync : 4]++;
    unsigned ret = (unsigned)(UINT_PTR)__builtin_return_address(0), game = game_caller((const unsigned *)__builtin_frame_address(0));
    for (int i = 0; i < 8; i++) {
        if (g_callers[i].n && (g_callers[i].ret != ret || g_callers[i].game_ret != game)) continue;
        g_callers[i].ret = ret; g_callers[i].game_ret = game; g_callers[i].n++;
        break;
    }
    if (g_capturing && sc == g_sc) {
        char t[600];
        targets(t, sizeof t);
        fprintf(g_cap, "Present(sync %u, flags 0x%x), called from %p (game +0x%X): %s\n", sync, flags, (void *)(UINT_PTR)ret, game, t);
        dump_bb("at Present");
    }
    if (g_live_state == 1 && sc == g_sc) {   // started when the swapchain was created (w_create_sc)
        DXGI_SWAP_CHAIN_DESC d;
        ID3D11Resource *bb = backbuffer();
        if (bb && SUCCEEDED(IDXGISwapChain_GetDesc(sc, &d))) {
            if (d.BufferDesc.Width != g_live_w || d.BufferDesc.Height != g_live_h || !fgb_active()) {
                // shortcut: a resize (or a helper that died) ends the bridge for the session; restart the game to get it back
                g_log("frame bridge: back buffer %ux%u (was %ux%u), helper %s - bridge off, game presents again", d.BufferDesc.Width,
                      d.BufferDesc.Height, g_live_w, g_live_h, fgb_active() ? "running" : "gone");
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
                if (g_motion) { fgm_frame_end(); mp_hook(NULL); }   // hooks back before the next frame's instance copies
                ID3D11Resource_Release(bb);
                jit_frame_start();
                return S_OK;
            }
        }
        if (bb) ID3D11Resource_Release(bb);
    }
    return r_present(sc, sync, flags);
}

static void patch_slot(void **slot, void *to, void **old) {
    DWORD prot;
    if (*slot == to || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &prot)) return;
    *old = *slot;
    *slot = to;
    VirtualProtect(slot, sizeof *slot, prot, &prot);
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
static int g_sr;                         // 0 off, 1 DLAA, 2..5 Quality..Ultra Performance (fgbridge_on.txt line 4, F12 panel)
static int g_jit_on;                     // this frame is jittered and the HUD-less point has not come yet
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

// the viewports the game set, as they should be now: shrunk and / or jittered (see above), else as set
static int vp_adjust(D3D11_VIEWPORT *v) {
    int changed = 0;
    memcpy(v, g_gvp, g_ngvp * sizeof *v);
    if (!g_jit_on) return 0;   // set from the frame start to the HUD-less point when DLSS is on
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
        if (jitter) { x->TopLeftX += g_jx; x->TopLeftY += g_jy; changed = 1; }
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
typedef struct { ID3D11Buffer *game, *ours; void *mapped; int valid; } CamBuf;
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

static void cam_write(CamBuf *e, const void *src) {
    float f[144];
    memcpy(f, src, sizeof f);
    float sx = (float)g_live_w / g_rw, sy = (float)g_live_h / g_rh;
    if (g_rw && g_rh && (sx != 1 || sy != 1)) {
        for (int m = 0; m < 2; m++) {   // inverse_projection (byte 272), inv_view_projection (336): rows r_i -> r_i * S
            float *r = f + (m ? 84 : 68);
            for (int i = 0; i < 4; i++, r += 4) { r[3] += r[0] * (sx - 1) + r[1] * (1 - sy); r[0] *= sx; r[1] *= sy; }
        }
        for (int m = 0; m < 2; m++) {   // projection (80), view_projection (144): rows x, y -> S^-1 * M
            float *x = f + (m ? 36 : 20), *y = x + 4, *w = x + 12;
            for (int j = 0; j < 4; j++) { x[j] = (x[j] - (sx - 1) * w[j]) / sx; y[j] = (y[j] - (1 - sy) * w[j]) / sy; }
        }
    }
    r_updsub(g_ctx, (ID3D11Resource *)e->ours, 0, NULL, f, 0, 0);
    e->valid = 1;
    g_cam_writes++;
}

static void ps_cb_apply(ID3D11DeviceContext *c) {
    CamBuf *e = g_ps_cb0 && g_jit_on && (g_rw != g_live_w || g_rh != g_live_h) && screen_part(g_rt_w, g_rt_h)
                    ? cam_find((ID3D11Resource *)g_ps_cb0, 1) : NULL;
    ID3D11Buffer *want = e && e->valid ? e->ours : g_ps_cb0;
    if (want != g_ps_bound && r_pscb) { r_pscb(c, 0, 1, &want); g_ps_bound = want; }
}

static void STDMETHODCALLTYPE w_pscb(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b) {
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
    CamBuf *e = c == g_ctx ? cam_find(r, 0) : NULL;
    if (e && e->mapped) { cam_write(e, e->mapped); e->mapped = NULL; }
    r_unmap(c, r, sub);
}
static void STDMETHODCALLTYPE w_updsub(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, const D3D11_BOX *box, const void *data, UINT rp, UINT dp) {
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
    r_omrt(c, n, rtv, dsv);
    if (c != g_ctx) return;
    dsv_changed(dsv);
    target_changed(n, rtv, dsv);
    jit_apply(c, 0);
}

static void STDMETHODCALLTYPE w_omrtuav(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv,
                                        UINT us, UINT nu, ID3D11UnorderedAccessView *const *u, const UINT *ic) {
    r_omrtuav(c, n, rtv, dsv, us, nu, u, ic);
    if (c != g_ctx || n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) return;
    dsv_changed(dsv);
    target_changed(n, rtv, dsv);
    jit_apply(c, 0);
}

static void STDMETHODCALLTYPE w_omdss(ID3D11DeviceContext *c, ID3D11DepthStencilState *st, UINT ref) {
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
    patch_slot(SLOT(Map), (void *)w_map, (void **)&r_map);
    patch_slot(SLOT(Unmap), (void *)w_unmap, (void **)&r_unmap);
    patch_slot(SLOT(UpdateSubresource), (void *)w_updsub, (void **)&r_updsub);
#undef SLOT
}

// frame start (after Present): this frame's jitter; HUD-less point: back to the game's own viewports
static void jit_frame_start(void) {
    if (!g_sr || g_live_state != 1) return;
    fgb_jitter(&g_jx, &g_jy);
    g_jit_on = g_jx != 0 || g_jy != 0 || g_rw != g_live_w || g_rh != g_live_h;
    g_jit_frames += g_jit_on;
    jit_apply(g_ctx, 1);
}
static void jit_frame_end(void) {
    if (!g_jit_on) return;   // also after DLAA was switched off (F12 panel)
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
    if (!g_capturing || !g_cap || c != g_ctx || GetCurrentThreadId() != g_pt_main) return;
    g_mp_draws++;
    ID3D11VertexShader *vs = NULL;
    ID3D11InputLayout *il = NULL;
    ID3D11Buffer *vb[4] = {0}, *cb[4] = {0};
    UINT st[4] = {0}, of[4] = {0};
    ID3D11ShaderResourceView *srv[8] = {0};
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
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
    char line[1024];
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
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (dsv) ID3D11DepthStencilView_Release(dsv);
}

static void STDMETHODCALLTYPE w_dii(ID3D11DeviceContext *c, UINT ic, UINT inst, UINT si, INT bv, UINT sinst) {
    mp_draw(c, "DrawIndexedInstanced", ic, inst, sinst);
    if (g_motion && c == g_ctx && GetCurrentThreadId() == g_pt_main && mp_stage_vt() == 0x1CCE4E4) fgm_draw(c, ic, inst, si, bv, sinst);
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
static void STDMETHODCALLTYPE w_draw(ID3D11DeviceContext *c, UINT vc, UINT sv) {
    mp_draw(c, "Draw", vc, 1, 0);
    r_draw(c, vc, sv);
}
static HRESULT(STDMETHODCALLTYPE *r_map)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
static HRESULT STDMETHODCALLTYPE w_map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE *m) {
    HRESULT hr = r_map(c, r, sub, t, f, m);
    if (SUCCEEDED(hr) && g_motion && c == g_ctx) fgm_mapped(r, m);
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
    for (int i = 0; i < 5; i++) {
        void **slot = &vt[slots[i].off / sizeof(void *)];
        if (*slot == ours[i]) continue;
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

// the F12 panel's switches (take effect from the next frame)
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
// the add-on's settings.txt: one key=value per line, written back whenever the F12 panel changes something
static const char *const SR_KEYS[6] = {"off", "dlaa", "quality", "balanced", "performance", "ultra_performance"};
static const char *const FG_KEYS[4] = {"off", "2x", "3x", "4x"};
static void settings_save(void) {
    if (!g_settings[0]) return;
    FILE *f = _wfopen(g_settings, L"w");
    if (!f) { g_log("DLSS settings: %ls not writable - choices not saved", g_settings); return; }
    fprintf(f, "# DLSS add-on settings. The F12 panel in game changes and saves them.\n"
               "# super_resolution: off, dlaa, quality, balanced, performance, ultra_performance\n"
               "# frame_generation: off, 2x, 3x, 4x (limited to what your GPU allows)\n"
               "# motion_vectors: camera_only, per_object\n"
               "super_resolution=%s\nframe_generation=%s\nmotion_vectors=%s\n",
            SR_KEYS[g_sr >= 0 && g_sr < 6 ? g_sr : 1], FG_KEYS[g_fg < 4 ? g_fg : 0], g_motion_on ? "per_object" : "camera_only");
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
    g_log("F12 panel: super resolution %s, render %ux%u", names[mode >= 0 && mode < 6 ? mode : 0], g_rw, g_rh);
    settings_save();
}
static void panel_fg(int n) {
    g_fg = (unsigned)n;
    fgb_set_fg((unsigned)n);
    g_log("F12 panel: frame generation %s", n ? (n == 1 ? "2x" : n == 2 ? "3x" : "4x") : "off");
    settings_save();
}
static void panel_motion(int on) {
    g_motion_on = on;
    if (g_motion) fgm_set_enabled(on);
    g_log("F12 panel: per-object motion %s", on ? "on" : "off");
    settings_save();
}

static HRESULT STDMETHODCALLTYPE w_create_sc(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out) {
    mp_device(dev);
    DXGI_SWAP_CHAIN_DESC dd = *d;
    if (g_live && g_live_state == 1) {   // shortcut: a second swapchain (settings change) ends the bridge; restart the game to get it back
        g_log("frame bridge: the game makes a new swapchain (%ux%u) - bridge off, game presents again", d->BufferDesc.Width, d->BufferDesc.Height);
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
            g_out_hwnd = d->OutputWindow;
            if (g_fg > fgb_fg_max()) g_fg = fgb_fg_max();   // NVIDIA's rules for this GPU (fgb_start logged it)
            sr_set(g_sr);
            jit_install();   // passes viewports through unless DLAA is on (F12 panel)
            if (g_motion) {
                // the game's instance record copy (FUN_117dab80): call memcpy at +0x17DABAF -> fgm_copy
                unsigned char *call = (unsigned char *)(UINT_PTR)(g_pt_base + 0x17DABAF);
                unsigned target = g_pt_base + 0x1A27CA0;
                DWORD prot;
                if (call[0] != 0xE8 || (unsigned)(UINT_PTR)call + 5 + *(int *)(call + 1) != target) {
                    g_log("motion pass: the record copy call at +0x17DABAF is not the expected one - off");
                } else if (fgm_init(g_dev, g_ctx, fgb_mv_texture(), g_live_w, g_live_h, g_log) &&
                           VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &prot)) {
                    *(int *)(call + 1) = (int)((UINT_PTR)fgm_copy - ((UINT_PTR)call + 5));
                    VirtualProtect(call, 5, prot, &prot);
                    FlushInstructionCache(GetCurrentProcess(), call, 5);
                    fgb_set_motion(fgm_pass);
                    fgm_set_enabled(g_motion_on);
                    mp_hook(NULL);
                } else g_motion = 0;
            }
            FgpApply ap = {panel_sr, panel_fg, panel_motion};
            if (!fgp_init(g_dev, g_ctx, g_live_w, g_live_h, d->BufferDesc.Format, g_sr, (int)g_fg, g_motion ? g_motion_on : -1, ap))
                g_log("frame bridge: F12 panel not available (back buffer format %u)", d->BufferDesc.Format);
        }
        else if (hid) DestroyWindow(hid);
    }
    HRESULT hr = r_create_sc(f, dev, &dd, out);
    if (FAILED(hr) && g_live_state == 1) { g_log("frame bridge: game swapchain on the hidden window failed (0x%08lx) - bridge off", hr); bridge_off(); }
    if (FAILED(hr) && dd.OutputWindow != d->OutputWindow) hr = r_create_sc(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out) {
        g_sc = *out;
        g_scd = *d;
        void **vt = *(void ***)*out;
        patch_slot(&vt[offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *)], (void *)w_present, (void **)&r_present);
    }
    return hr;
}

typedef HRESULT(WINAPI *factory_fn)(REFIID, void **);
static factory_fn r_factory;
static HRESULT WINAPI w_factory(REFIID r, void **out) {
    HRESULT hr = r_factory(r, out);
    if (SUCCEEDED(hr) && out && *out) {
        void **vt = *(void ***)*out;
        patch_slot(&vt[offsetof(IDXGIFactoryVtbl, CreateSwapChain) / sizeof(void *)], (void *)w_create_sc, (void **)&r_create_sc);
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
static FARPROC WINAPI w_gpa(HMODULE m, LPCSTR name) {
    FARPROC f = r_gpa(m, name);
    if (!f || (UINT_PTR)name <= 0xFFFF) return f;
    if (!strcmp(name, "D3D11CreateDevice")) { r_create = (create_fn)(void *)f; return (FARPROC)(void *)w_create; }
    if (!strcmp(name, "CreateDXGIFactory1")) { r_factory = (factory_fn)(void *)f; return (FARPROC)(void *)w_factory; }
    return f;
}

static void bp_report(void (*log)(const char *fmt, ...)) {
    log("bridge probe: %u presents (%u not on the main thread), sync interval 0/1/2/3/more: %u/%u/%u/%u/%u, captures %d%s",
        g_presents, g_present_other_thread, g_sync[0], g_sync[1], g_sync[2], g_sync[3], g_sync[4], g_captures,
        g_sc ? "" : " - no swapchain seen");
    log("  D3D11CreateDevice calls seen %d, device %s", g_creates, !g_ctx ? "not known yet" : g_dev_from_sc ? "taken from the swapchain" : "from D3D11CreateDevice");
    if (g_live)
        log("  frame bridge %s: %u frames handed over, %u pacing timeouts, HUD-less copies %u, no camera %u, no depth %u",
            g_live_state == 1 ? "running" : g_live_state == 2 ? "off" : "not started", fgb_frames(), fgb_pace_timeouts(), g_br_hud,
            g_br_novp, g_br_nodepth);
    if (g_sr) {
        unsigned sf, st; double sa, sm;
        void fgb_sr_stats(unsigned *, unsigned *, double *, double *);
        fgb_sr_stats(&sf, &st, &sa, &sm);
        log("  DLSS render %ux%u: %u frames jittered / scaled, viewport calls seen %u (%u changed); camera blocks %u, %u pixel shader "
            "copies written; SR round trips %u, game CPU wait avg %.3f ms max %.3f ms, %u timeouts",
            g_rw, g_rh, g_jit_frames, g_jit_vp_calls, g_jit_vp_jittered, g_ncbuf, g_cam_writes, sf, sa, sm, st);
        g_jit_frames = g_jit_vp_calls = g_jit_vp_jittered = g_cam_writes = 0;
    }
    if (g_live_state == 1) {
        void fgb_cpu_stats(double *, double *, double *, unsigned *);
        double hm, pm, wm;
        unsigned fr;
        fgb_cpu_stats(&hm, &pm, &wm, &fr);
        if (fr) log("  frame bridge main-thread time per frame: HUD-less step %.3f ms (incl. the DLSS pickup and the motion pass), "
                    "Present step %.3f ms, of which waiting for the helper %.3f ms", hm / fr, pm / fr, wm / fr);
    }
    if (g_motion) fgm_report(log);
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
    if (!g_live || !cam || !stage || GetCurrentThreadId() != g_pt_main) return;
    if (stage[0] - g_pt_base != 0x1CCE4E4) return;
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
    unsigned char *f = (unsigned char *)m + 0x177BB10;
    static const unsigned char expect[5] = {0x56, 0x8B, 0x74, 0x24, 0x0C};   // push esi; mov esi, [esp+0Ch]
    if (memcmp(f, expect, 5)) { log("frame bridge: set-camera FUN_1177bb10 is not the expected code - no camera"); return 0; }
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

extern void *gt_iat_hook_any(HMODULE m, const char *name, void *to);   // gputime.c
int install_bridgeprobe(HMODULE m, const wchar_t *dir, void (*log)(const char *fmt, ...)) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((unsigned char *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    g_mod_size = nt->OptionalHeader.SizeOfImage;
    wcsncpy(g_dir, dir, MAX_PATH - 1);
    g_id_stage = pt_id_of(0x176A450); g_id_level = pt_id_of(0x1778E90); g_id_scene = pt_id_of(0x17FA310); g_id_view = pt_id_of(0x173EED0); g_id_cam = pt_id_of(0x177BB10);
    void *old = gt_iat_hook_any(m, "GetProcAddress", (void *)w_gpa);
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
        char text[1024] = {0};
        FILE *sf = _wfopen(g_settings, L"r");
        if (sf) { fread(text, 1, sizeof text - 1, sf); fclose(sf); }
        static const char *const MV_KEYS[2] = {"camera_only", "per_object"};
        g_sr = settings_value(text, "super_resolution=", SR_KEYS, 6, 1);       // default DLAA
        g_fg = (unsigned)settings_value(text, "frame_generation=", FG_KEYS, 4, 1);   // default 2x (limited to the GPU later)
        g_motion_on = settings_value(text, "motion_vectors=", MV_KEYS, 2, 1);  // default per object
        g_live = 1;
        g_motion = 1;
        log("DLSS add-on found: super resolution %s, frame generation %s, motion vectors %s (%ls)", SR_KEYS[g_sr], FG_KEYS[g_fg],
            MV_KEYS[g_motion_on], sf ? L"settings.txt" : L"defaults");
        if (!sf) settings_save();
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
             : strstr(l[3], "dlaa") ? 1 : 0;   // the F12 panel switches it
        g_motion_on = strstr(l[3], "motion") != NULL;
        // dev test of NVIDIA's GPU rules on this PC: treat the GPU as an RTX 20 / 30 (no frame generation) or an RTX 40 (2x)
        if (strstr(l[3], "fakegpu20")) { fgb_set_fg_test_limit(1); log("  test: GPU treated as an RTX 20 / 30 (no frame generation)"); }
        else if (strstr(l[3], "fakegpu40")) { fgb_set_fg_test_limit(2); log("  test: GPU treated as an RTX 40 (frame generation 2x at most)"); }
        if (strstr(l[3], "fakeamd")) { fgb_set_test_not_nvidia(1); log("  test: GPU treated as not NVIDIA (bridge stays off)"); }
        g_motion = g_live;   // set up whenever the bridge runs, so the F12 panel can switch it on
        log("frame bridge %s: helper %s, Streamline %s, frame generation %ux, DLSS %s", g_live ? "armed" : "NOT armed (bad fgbridge_on.txt)",
            l[0], l[1], g_fg + 1, (const char *[]){"off", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"}[g_sr]);
        log("  per-object motion vectors: %s (F12 panel switches it)", g_motion_on ? "on" : "off");
        if (g_live) g_dev_di8_created = fgp_di8_created;   // the panel hides the game's mouse buttons while the cursor is over it
    }
    if (g_live && cam_hook(m, log)) log("frame bridge: camera read through a jump hook on FUN_1177bb10");
    log("bridge probe installed: Present, render targets per stage, back buffer dumps and camera blocks on bridge_go.txt");
    return 1;
}
