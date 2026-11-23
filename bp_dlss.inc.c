// Part of bridgeprobe.c (included there in this order, not compiled on its own): the DLSS glue on the game's D3D11 context: viewport jitter and scaling, the pixel shaders' camera copy,
// the texture mip bias, the render-target hooks, jit_install.
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
static float g_game_t;   // the game's clock (camera_VS_PS c26.x, the wind's time) and the frame it last moved: it stops while
static unsigned g_game_t_frame;   // the battle is paused (the settings sweep logs which)
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

#include "bp_campatch.inc.c"

static void cam_write(CamBuf *e, const void *src) {
    if (src != e->src) memcpy(e->src, src, sizeof e->src);
    if (e->src[104] != g_game_t) { g_game_t = e->src[104]; g_game_t_frame = g_frame; }
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

#define g_pscam_off 0

// below native size the copy from the frame start; at DLAA from the frame's first jittered draw (Rome 2's rule: passes
// before the scene keep the game's camera)
static void ps_cb_apply(ID3D11DeviceContext *c) {
    int scaled = g_rw != g_live_w || g_rh != g_live_h;
    CamBuf *e = !g_pscam_off && g_ps_cb0 && g_jit_on && (scaled || ((g_jx || g_jy) && g_jit_drawn)) && screen_part(g_rt_w, g_rt_h) && !overlay_stage()
                    ? cam_find((ID3D11Resource *)g_ps_cb0, 1) : NULL;
    ID3D11Buffer *want = e && e->valid ? e->ours : g_ps_cb0;
    if (want != g_ps_bound && r_pscb) { r_pscb(c, 0, 1, &want); g_ps_bound = want; }
}

extern volatile int g_fgm_in_pass;   // fgmotion.c: the motion pass sets its own state (not the game's: pass it on)
static void STDMETHODCALLTYPE w_pscb(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b) {
    g_hk[HK_PSCB]++;
    if (c != g_ctx || g_fgm_in_pass || start != 0 || !n || !b || n > 14) { r_pscb(c, start, n, b); return; }
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
    CamBuf *e = c == g_ctx && !g_pscam_off ? cam_find(r, 0) : NULL;
    if (e && e->mapped) { cam_write(e, e->mapped); e->mapped = NULL; }
    r_unmap(c, r, sub);
}
static void STDMETHODCALLTYPE w_updsub(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, const D3D11_BOX *box, const void *data, UINT rp, UINT dp) {
    g_hk[HK_UPDSUB]++;
    r_updsub(c, r, sub, box, data, rp, dp);
    if (c != g_ctx || g_live_state != 1 || box || !data || g_pscam_off) return;
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
    if (c != g_ctx || g_fgm_in_pass || n > 16) { r_rsvp(c, n, v); return; }
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

#include "bp_lodsampler.inc.c"

static void(STDMETHODCALLTYPE *r_pssamp)(ID3D11DeviceContext *, UINT, UINT, ID3D11SamplerState *const *);
static void STDMETHODCALLTYPE w_pssamp(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11SamplerState *const *s) {
    if (c != g_ctx || g_fgm_in_pass || g_live_state != 1 || !g_jit_on || g_rw >= g_live_w || g_lod_off || !s || !n || n > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT ||
        !screen_part(g_rt_w, g_rt_h) || overlay_stage()) { r_pssamp(c, start, n, s); return; }
    float bias = log2f((float)g_rw / g_live_w) + g_lod_extra;
    ID3D11SamplerState *b[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    for (UINT i = 0; i < n; i++) g_lod_swaps += (b[i] = lod_sampler(s[i], bias)) != s[i];
    r_pssamp(c, start, n, b);
}

// The game does not set a sampler again when it thinks the slot already holds it (RenoDX's notes, texture_lod_bias.hpp:
// the shadow pass's sampler stayed in the main view; AGENT_LOG 172-173): swapping only in PSSetSamplers can leave a
// material sampler unbiased in the scene, or our biased copy bound into the shadow maps. So the bound slots are fixed
// whenever the biased state starts or ends: the game's samplers -> our copies going in, ours -> the game's going out.
static unsigned g_lod_in, g_lod_out;   // slots fixed going in / going out (the 30 s report: the skip happens if > 0)
static void lod_slots(ID3D11DeviceContext *c) {
    static int active;
    int now = c == g_ctx && g_live_state == 1 && g_jit_on && g_rw < g_live_w && !g_lod_off && screen_part(g_rt_w, g_rt_h) &&
              !overlay_stage();
    if (now == active || (!now && !g_nlod)) { active = now; return; }
    active = now;
    ID3D11SamplerState *s[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT], *b[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    ID3D11DeviceContext_PSGetSamplers(c, 0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, s);
    int changed = 0;
    float bias = log2f((float)g_rw / g_live_w) + g_lod_extra;
    for (UINT i = 0; i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
        b[i] = s[i];
        if (!s[i]) continue;
        if (now) b[i] = lod_sampler(s[i], bias);
        else
            for (unsigned k = 0; k < g_nlod; k++)
                if (g_lod[k].ours == s[i]) { b[i] = g_lod[k].game; break; }
        if (b[i] != s[i]) { changed = 1; now ? g_lod_in++ : g_lod_out++; }
    }
    if (changed) r_pssamp(c, 0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, b);
    for (UINT i = 0; i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) if (s[i]) ID3D11SamplerState_Release(s[i]);
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
    if (c != g_ctx || g_fgm_in_pass) return;
    dsv_changed(dsv);
    target_changed(n, rtv, dsv);
    jit_apply(c, 0);
    lod_slots(c);
}

static void STDMETHODCALLTYPE w_omrtuav(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv,
                                        UINT us, UINT nu, ID3D11UnorderedAccessView *const *u, const UINT *ic) {
    g_hk[HK_OMRT]++;
    r_omrtuav(c, n, rtv, dsv, us, nu, u, ic);
    if (c != g_ctx || g_fgm_in_pass || n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) return;
    dsv_changed(dsv);
    target_changed(n, rtv, dsv);
    jit_apply(c, 0);
    lod_slots(c);
}

static void STDMETHODCALLTYPE w_omdss(ID3D11DeviceContext *c, ID3D11DepthStencilState *st, UINT ref) {
    g_hk[HK_OMDSS]++;
    r_omdss(c, st, ref);
    if (c != g_ctx || g_fgm_in_pass) return;
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
// One context slot, hooked again at every stage: d3d11 swaps its own functions back in (see mp_hook). A slot that another
// mod hooked after us (it calls ours as its original) is left to it: hooking again would make ours call it back until the
// stack overflows (as mp_hook and patch_once already guard; ultrareview 2026-11-02). Our own module's hooks are not foreign.
static void ctx_slot(int i, void **slot, void *to, void **old, const char *name) {
    static char foreign[10];
    if (*slot == to || foreign[i]) return;
    if (*old) {
        HMODULE owner = NULL, self = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)*slot, &owner);
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)ctx_slot, &self);
        if (owner != GetModuleHandleW(L"d3d11.dll") && owner != self) {
            wchar_t n[MAX_PATH] = L"?";
            if (owner) GetModuleFileNameW(owner, n, MAX_PATH);
            const wchar_t *b = wcsrchr(n, L'\\');
            g_log("frame bridge: %s on the game's D3D11 context is now hooked by %ls - left to it", name, b ? b + 1 : n);
            foreign[i] = 1;
            return;
        }
    }
    patch_slot(slot, to, old);
}
static void jit_install(void) {
    void **vt = *(void ***)g_ctx;
#define SLOT(m) &vt[offsetof(ID3D11DeviceContextVtbl, m) / sizeof(void *)]
    ctx_slot(0, SLOT(RSSetViewports), (void *)w_rsvp, (void **)&r_rsvp, "RSSetViewports");
    ctx_slot(1, SLOT(RSGetViewports), (void *)w_rsgetvp, (void **)&r_rsgetvp, "RSGetViewports");
    ctx_slot(2, SLOT(OMSetRenderTargets), (void *)w_omrt, (void **)&r_omrt, "OMSetRenderTargets");
    ctx_slot(3, SLOT(OMSetRenderTargetsAndUnorderedAccessViews), (void *)w_omrtuav, (void **)&r_omrtuav, "OMSetRenderTargetsAndUnorderedAccessViews");
    ctx_slot(4, SLOT(OMSetDepthStencilState), (void *)w_omdss, (void **)&r_omdss, "OMSetDepthStencilState");
    ctx_slot(5, SLOT(PSSetConstantBuffers), (void *)w_pscb, (void **)&r_pscb, "PSSetConstantBuffers");
    ctx_slot(6, SLOT(PSSetSamplers), (void *)w_pssamp, (void **)&r_pssamp, "PSSetSamplers");
    ctx_slot(7, SLOT(Map), (void *)w_map, (void **)&r_map, "Map");
    ctx_slot(8, SLOT(Unmap), (void *)w_unmap, (void **)&r_unmap, "Unmap");
    ctx_slot(9, SLOT(UpdateSubresource), (void *)w_updsub, (void **)&r_updsub, "UpdateSubresource");
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
    lod_slots(g_ctx);   // our biased samplers out of the slots before the next frame's shadow pass
}
