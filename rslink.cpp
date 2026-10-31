// ReShade link (DLSS add-on on, ReShade in the game folder): the mod registers itself as a ReShade add-on and runs the
// effects at its HUD-less point (after DLSS, before the UI), with the scene depth as ReShade's DEPTH. ReShade's add-on
// API (src/reshade/, v6.8.0 headers, API version 20); the calls used are open in ReShade's standard download too (its
// limited add-on build blocks only the per-draw events, which the mod does not use: it hooks D3D11 itself).
// Why it is needed: ReShade wraps the game's device and swapchain (Rome 2's probe: their vtables are ReShade's), so the
// bridge's Present ran before ReShade's: the frame went to the helper without effects or ReShade's menu, which landed on
// the hidden window. The glue (bridgeprobe.c) hooks the real swapchain under ReShade's wrapper (rsl_swapchain_cb), so
// ReShade draws its menu first, and gives ReShade the game's window as the swapchain's (its menu's keys).
// Ported from Rome 2's engine fix (AGENT_LOG "ReShade link"), where it was first checked in game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <reshade.hpp>

using namespace reshade::api;

static void (*s_log)(const char *, ...);
static int (*s_swapchain_cb)(void *real_swapchain);   // 1: the game's swapchain
static uint64_t s_game_sc;              // the real swapchain under the game's wrapper
static effect_runtime *s_rt;
static ID3D11Texture2D *s_depth;          // our copy of the scene depth (the game's is the depth target while effects run)
static ID3D11ShaderResourceView *s_depth_srv;
static ID3D11RenderTargetView *s_rtv;
static ID3D11Resource *s_rtv_of;
static unsigned s_presents, s_effect_frames, s_bound, s_other;
static int s_reversed = -1;               // the depth's direction from the camera (-1 unknown)
// the far plane the mod wrote last (reshade_link.txt in the add-on's folder): ReShade saves it in the player's preset, so
// at the next start it must not be taken for the player's own value (it would then stay at the last battle's for good)
static wchar_t s_state[MAX_PATH];
static char s_far_mine[24];

static void on_init_swapchain(swapchain *sc, bool) {
    if (s_swapchain_cb && s_swapchain_cb((void *)(uintptr_t)sc->get_native())) s_game_sc = sc->get_native();
}
static void on_init_runtime(effect_runtime *rt) {
    // another swapchain (the bridge's fallback, which shows the game's frames after the bridge stopped): they already
    // have the effects (and ReShade's menu) from the game's swapchain, so none here
    if (rt->get_native() != s_game_sc) {
        rt->set_effects_state(false);
        if (s_log) s_log("ReShade: a second swapchain (the mod's own) - ReShade's effects off on it (they are in the frames it shows)");
        return;
    }
    s_rt = rt;
    if (s_log) s_log("ReShade: effect runtime up (window %p) - its effects run at the mod's HUD-less point (after DLSS, before the UI)", rt->get_hwnd());
    if (s_reversed >= 0) {   // a runtime made again (resize): the depth direction the mod found
        char v[8];
        if (!rt->get_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", v)) rt->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", s_reversed ? "1" : "0");
    }
}
static void on_destroy_runtime(effect_runtime *rt) {
    if (rt == s_rt) s_rt = nullptr;
}
// the depth: ours, bound after ReShade's own depth add-on (registered before us) has bound its guess
static void on_begin_effects(effect_runtime *rt, command_list *, resource_view, resource_view) {
    if (rt != s_rt) { s_other++; return; }
    if (!s_depth_srv) return;
    resource_view v = {(uint64_t)(uintptr_t)s_depth_srv};
    rt->update_texture_bindings("DEPTH", v, v);
    s_bound++;
}
static void on_present(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *) {
    s_presents++;
}

extern "C" {

// once, when the game looks up DXGI / D3D11 in a module that is ReShade (exports ReShadeRegisterAddon); state: the file
// holding the far plane the mod wrote last
int rsl_attach(HMODULE self, HMODULE reshade, void (*log)(const char *, ...), int (*swapchain_cb)(void *), const wchar_t *state) {
    s_log = log;
    s_swapchain_cb = swapchain_cb;
    if (state) {
        lstrcpynW(s_state, state, MAX_PATH);
        FILE *f = _wfopen(s_state, L"r");
        char line[64];
        if (f) {
            while (fgets(line, sizeof line, f))
                if (!strncmp(line, "far_plane=", 10)) {
                    line[strcspn(line, "\r\n")] = 0;
                    lstrcpynA(s_far_mine, line + 10, sizeof s_far_mine);
                }
            fclose(f);
        }
    }
    if (!reshade::register_addon(self, reshade)) {
        log("ReShade: found, but it did not take the mod as an add-on (API %d) - ReShade runs as it would without the mod's link", RESHADE_API_VERSION);
        return 0;
    }
    reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_runtime);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_runtime);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
    reshade::register_event<reshade::addon_event::present>(on_present);
    log("ReShade: the mod is linked as a ReShade add-on (API %d)", RESHADE_API_VERSION);
    return 1;
}

int rsl_ready(void) { return s_rt != nullptr; }

// at the HUD-less point: ReShade's effects into the back buffer (the game's state is saved and put back by ReShade), with
// a copy of the scene depth as DEPTH; ReShade then skips its own effect pass at Present for this frame
int rsl_effects(ID3D11DeviceContext *ctx, ID3D11Resource *bb, ID3D11Resource *depth) {
    if (!s_rt) return 0;
    ID3D11Device *dev = nullptr;
    ctx->GetDevice(&dev);
    if (bb != s_rtv_of) {
        if (s_rtv) s_rtv->Release();
        s_rtv = nullptr; s_rtv_of = nullptr;
        if (SUCCEEDED(dev->CreateRenderTargetView(bb, nullptr, &s_rtv))) s_rtv_of = bb;
    }
    D3D11_TEXTURE2D_DESC dd = {}, od = {};
    if (depth) static_cast<ID3D11Texture2D *>(depth)->GetDesc(&dd);
    if (s_depth) s_depth->GetDesc(&od);
    if (depth && (!s_depth || od.Width != dd.Width || od.Height != dd.Height || od.Format != dd.Format)) {
        if (s_depth_srv) s_depth_srv->Release();
        if (s_depth) s_depth->Release();
        s_depth = nullptr; s_depth_srv = nullptr;
        D3D11_TEXTURE2D_DESC cd = dd;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.MiscFlags = 0; cd.CPUAccessFlags = 0; cd.Usage = D3D11_USAGE_DEFAULT;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        if (dd.Format != DXGI_FORMAT_R24G8_TYPELESS || dd.SampleDesc.Count != 1 || FAILED(dev->CreateTexture2D(&cd, nullptr, &s_depth)) ||
            FAILED(dev->CreateShaderResourceView(s_depth, &sd, &s_depth_srv))) {
            if (s_depth) s_depth->Release();
            s_depth = nullptr;
            if (s_log) s_log("ReShade: scene depth format %u (%u samples) not handed over - ReShade keeps its own depth", dd.Format, dd.SampleDesc.Count);
        }
    }
    if (s_depth && depth) ctx->CopyResource(s_depth, depth);
    dev->Release();
    if (!s_rtv) return 0;
    resource_view v = {(uint64_t)(uintptr_t)s_rtv};
    s_rt->render_effects(s_rt->get_command_queue()->get_immediate_command_list(), v, v);
    s_effect_frames++;
    return 1;
}

// the depth's direction from the game's camera, once: ReShade's default is reversed (near = 1); the two clip points at
// depth 0 and 1 on the view axis, taken back to the world: the one with the smaller w is the near one. The player's own
// RESHADE_DEPTH_INPUT_IS_REVERSED wins.
static int inverse4(const float *m, float *o) {
    float a[4][8];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabsf(a[r][c]) > fabsf(a[p][c])) p = r;
        if (fabsf(a[p][c]) < 1e-12f) return 0;
        for (int k = 0; k < 8; k++) { float t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            float f = a[r][c] / a[c][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) o[r * 4 + c] = a[r][c + 4] / a[r][r];
    return 1;
}
// the camera's view distance (w) at depth 0 and depth 1: which one is near (reversed or not), and their ratio = ReShade's
// RESHADE_DEPTH_LINEARIZATION_FAR_PLANE (ReShade.fxh: depth / (F - depth * (F - 1)) is (z - near) / (far - near) for
// F = far / near). Returns 0 when the camera tells neither (no inverse, a point at infinity).
int rsl_depth_planes(const float vp[16], int *reversed, float *ratio) {
    float inv[16], w[2];
    if (!inverse4(vp, inv)) return 0;
    for (int k = 0; k < 2; k++) {
        float q[4], c[4] = {0, 0, (float)k, 1};
        for (int r = 0; r < 4; r++) q[r] = inv[r * 4] * c[0] + inv[r * 4 + 1] * c[1] + inv[r * 4 + 2] * c[2] + inv[r * 4 + 3] * c[3];
        if (fabsf(q[3]) < 1e-12f) return 0;
        for (int r = 0; r < 3; r++) q[r] /= q[3];
        w[k] = vp[12] * q[0] + vp[13] * q[1] + vp[14] * q[2] + vp[15];
    }
    if (!(w[0] > 0 && w[1] > 0) || w[0] == w[1]) return 0;
    *reversed = w[0] > w[1];
    *ratio = *reversed ? w[0] / w[1] : w[1] / w[0];
    return 1;
}
// the player's own value wins; ReShade's default (its installer writes 1000.0) and the mod's last one (this session's or
// the one saved in reshade_link.txt) do not count as theirs
static char s_far_set[24];
static float s_far_ratio, s_far_cand;
static unsigned s_far_steady;
static int s_far_kept;
static void far_plane(float ratio) {
    // a new value makes ReShade compile its effects again (a hitch): only after the camera kept it for 60 frames, and only
    // for a change over 25 % (battle 13000 / campaign map 500 in the captures; a zoom does not count)
    if (s_far_ratio > 0 && fabsf(ratio - s_far_ratio) < 0.25f * s_far_ratio) { s_far_steady = 0; return; }
    if (s_far_cand <= 0 || fabsf(ratio - s_far_cand) > 0.05f * s_far_cand) { s_far_cand = ratio; s_far_steady = 0; return; }
    if (++s_far_steady < 60) return;
    s_far_steady = 0;
    char v[24];
    if (s_rt->get_preprocessor_definition("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", v) && strcmp(v, "1000.0") && strcmp(v, "1000") && strcmp(v, s_far_mine)) {
        if (!s_far_kept++ && s_log) s_log("ReShade: RESHADE_DEPTH_LINEARIZATION_FAR_PLANE is %s in the player's settings (the game's camera: %.1f) - kept", v, ratio);
        s_far_ratio = ratio;
        return;
    }
    s_far_ratio = ratio;
    snprintf(s_far_set, sizeof s_far_set, "%.1f", ratio);
    memcpy(s_far_mine, s_far_set, sizeof s_far_mine);
    s_rt->set_preprocessor_definition("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", s_far_set);
    FILE *f = s_state[0] ? _wfopen(s_state, L"w") : nullptr;
    if (f) { fprintf(f, "far_plane=%s\n", s_far_set); fclose(f); }
    if (s_log) s_log("ReShade: RESHADE_DEPTH_LINEARIZATION_FAR_PLANE set to %s (the game's camera: far / near)", s_far_set);
}
void rsl_camera(const float vp[16]) {
    int rev;
    float ratio;
    if (!s_rt || !rsl_depth_planes(vp, &rev, &ratio)) return;
    far_plane(ratio);
    if (s_reversed >= 0) return;
    s_reversed = rev;
    char v[8];
    if (s_rt->get_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", v)) {
        if (s_log) s_log("ReShade: RESHADE_DEPTH_INPUT_IS_REVERSED is %s in the player's settings (the game's depth: %s) - kept", v, rev ? "reversed" : "not reversed");
        return;
    }
    s_rt->set_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", rev ? "1" : "0");
    if (s_log) s_log("ReShade: RESHADE_DEPTH_INPUT_IS_REVERSED set to %d (from the game's camera)", rev);
}

unsigned rsl_presents(void) { return s_presents; }   // ReShade's Present ran (its menu drawn) this many times
int rsl_reversed(void) { return s_reversed; }
const char *rsl_far_plane(void) { return s_far_set; }
void *rsl_hwnd(void) { return s_rt ? s_rt->get_hwnd() : nullptr; }
void rsl_stats(unsigned *effect_frames, unsigned *depth_binds, unsigned *other) { *effect_frames = s_effect_frames; *depth_binds = s_bound; *other = s_other; }

}
