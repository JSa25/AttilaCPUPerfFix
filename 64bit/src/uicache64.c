// uicache64: battle UI panel cache, ported from heapfix/uicache.c (read that file's header for the method: the unit
// cards are drawn into two textures, one cleared to black and one to white, and shown from them with one draw in the
// frames they are not redrawn).
// OFF unless heapfix.ini has [fixes] battle_ui_cache=1: this port has not run in the game yet, and it sits between the
// game and Sol's DirectX 11 back end, which the x86 method was never tried against.
// Engine (names from the game file's symbols, offsets from its debug info):
//   TWUI::Component::Draw(DrawDetails &, unsigned, bool, bool)  0x2918f90   SysV: rdi this, rsi details; returns bool
//     its first 17 bytes (6 pushes ; sub rsp, 0x148) are replaced by jmp [rip+0] to the entry stub
//   Component: m_id (CA::String: characters at +8) at +0xf8, m_parent +0x108, m_absolute_position +0x128,
//     m_Visible +0x199; Width() 0x2916bd0, Height() 0x2916bf0; the root component: TWUI::Component::m_Root 0x51632d8
//   DrawDetails: 72 bytes, its first field the sprite batch (WARSCAPE::WS_SPRITE &, the WS_SPRITE_IMP itself)
//   WARSCAPE::WS_SPRITE_IMP::pr_draw()  0x30964d0   draws the quads gathered since the last state change
// Different from x86:
//   - a redraw is two runs of the game's own Draw of the panel, the first into the black texture, the second into the
//     white one (2026-11-22). The first version caught each draw in the context's function table and drew it a
//     second time into the white texture; d3d11 puts its own functions back into that table between calls (Flag Wave
//     counts it: about 1.4 times a frame), and every draw after such a swap reached the black texture only: the panel
//     came out washed for the 66 ms until the next redraw - the flicker of the first game runs (the probe's frame
//     recorder, 2026-11-22 03:49: every jump a redraw, none in the game's own frames). The draws are not caught at all
//     now. d3d11's functions are not patched either;
//   - four wrappers (OMSetRenderTargets, RSSetViewports, RSSetScissorRects, OMSetBlendState) sit in the context's
//     function table only while a panel is drawn into its textures, taking whatever each slot holds then as the
//     original, and come out when it ends, so nothing of another mod's is ever displaced; a call that passes them by
//     (the runtime swapped a slot) is harmless: the game sets none of them inside the panel's Draw (probe, 147 draws);
//   - the batch to flush is taken from the DrawDetails the game hands to Draw, so the game's flush is not hooked;
//   - the device and context come from d3d64.c;
//   - the scissor rectangle is moved with the viewport (2026-11-22): Sol's back end clips the interface with
//     RSSetScissorRects in the target's pixels (OpenGL's glScissor; the x86 game's renderer did not), so in a panel's
//     textures, whose corner sits at the panel's, an unmoved rectangle cut the cards wrongly - the flicker in the
//     first game run. Its clears (ClearView inside the scissor rectangle) land in the texture moved the same way.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "perffix64.h"

#define SV __attribute__((sysv_abi))
extern volatile LONG g_frame_no;            // d3d64.c
extern ID3D11Device *g_d3d_dev;
extern ID3D11DeviceContext *g_d3d_ctx;
extern volatile LONG g_d3d_device_no;
extern DWORD g_d3d_thread;

#define UC_DRAW 0x2918f90u
#define UC_WIDTH 0x2916bd0u
#define UC_HEIGHT 0x2916bf0u
#define UC_PRDRAW 0x30964d0u
#define UC_ROOT 0x51632d8u
static const uint8_t UC_DRAW_B[17] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x48, 0x01, 0x00, 0x00};
static const uint8_t UC_DIM_B[8] = {0x48, 0x8b, 0x87, 0x40, 0x01, 0x00, 0x00, 0x48};
static const uint8_t UC_PRDRAW_B[12] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83};
#define C_NAME 0x100             // char *: the component's name (the CA::String at +0xf8, its characters at +8)
#define C_PARENT 0x108
#define C_POS 0x128              // float x, y in the UI's units
#define C_VISIBLE 0x199
#define DETAILS 72               // bytes of the DrawDetails compared
// shortcut: MARGIN is all the room children get outside the panel's own rectangle (x86 run 3: 7 units at most), a
// child drawn further out is cut off - walk the children's rectangles if that ever shows
#ifndef UC_INTERVAL
#define UC_INTERVAL 66   // ms between redraws; the offline test sets a longer one so a busy PC cannot fail it
#endif
enum { NTARGET = 4, NBLEND = 32, INTERVAL = UC_INTERVAL, MARGIN = 48 };

typedef SV float (*DimFn)(void *self);
typedef SV void (*FlushFn)(void *batch);

typedef struct {
    char name[48];
    const char *under;             // an ancestor it must have (NULL: any)
    void *comp;                    // the component last seen under this name
    ID3D11Texture2D *tex[2];       // drawn over black, drawn over white
    ID3D11RenderTargetView *rtv[2];
    ID3D11ShaderResourceView *srv[2];
    unsigned tw, th;               // the textures' size
    int x, y;                      // where their corner is on the target
    float rect[4];                 // the component's x, y, width, height when it was last drawn
    int valid, off;                // off: something in it cannot be cached, it is drawn the game's way from then on
    DWORD drawn;                   // GetTickCount of the last redraw
    LONG seen;                     // frame it was last drawn in
    unsigned char details[DETAILS];
    unsigned why[6], reuse;        // redraws by reason (1 first, 2 moved / details, 3 a key or button, 4 interval, 5 the cursor over it), frames shown from the textures
} Target;
static Target g_t[NTARGET];
static int g_nt;

static void (*g_log)(const char *fmt, ...);
DimFn uc_width = (DimFn)(uintptr_t)UC_WIDTH, uc_height = (DimFn)(uintptr_t)UC_HEIGHT;   // the test points these at its own
FlushFn uc_flush = (FlushFn)(uintptr_t)UC_PRDRAW;
void **uc_root = (void **)(uintptr_t)UC_ROOT;
int uc_in_game = 1;                // 0 in the test: no cursor, its own keys
static float g_root_wh[2];         // the root's width and height (the UI's own units), read once a frame
static int g_on = 1;
void *g_uic_ret, *g_uic_draw_body; // where a redrawn panel's Draw was to return to; Draw behind its first 17 bytes
// The game does not draw a UI element when its component is drawn: it gathers quads in a batch and sends them to D3D
// when the next element changes state. So the panel's last quads would be drawn after its Draw has returned, and the
// quads of the element before it during the redraw. The batch is flushed at both edges, with the game's own function.
static void *g_batch;              // the batch of the Draw being looked at
static void flush_pending(void) { if (g_batch && uc_flush) uc_flush(g_batch); }

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11BlendState *g_blend;
static ID3D11DepthStencilState *g_dss;
static ID3D11RasterizerState *g_rs;
static struct { ID3D11BlendState *bs; int ok; } g_seen[NBLEND];
static int g_nseen, g_tried;

// while the game draws a panel into its textures
static Target *g_red;
static ID3D11RenderTargetView *g_bb, *g_game_rt;   // the target it was drawing to (held); the one it last asked for
static ID3D11DepthStencilView *g_dsv;
static D3D11_VIEWPORT g_game_vp;                   // the viewport it last asked for
static D3D11_RECT g_game_sc;                       // the scissor rectangle it last asked for
static int g_game_nsc;                             // 0: none set
static unsigned g_sc_moved;                        // rectangles moved into a panel's textures (for the log)
static int g_red_bad, g_own;                       // g_own: the D3D calls are ours

#define REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)
#define VT_PROBE ID3D11DeviceContext
#include "uicache64_probe.inc"
static unsigned g_pr_draws;

static const char *hlsl =
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint i : SV_VertexID) { V o; o.uv = float2((i << 1) & 2, i & 2); o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "Texture2D black : register(t0); Texture2D white : register(t1);\n"
    "struct O { float4 add : SV_Target0; float4 keep : SV_Target1; };\n"
    "O ps(V i) { float w, h; black.GetDimensions(w, h); int3 at = int3(i.uv * float2(w, h), 0);\n"
    "  float3 b = black.Load(at).rgb, wh = white.Load(at).rgb; O o; o.add = float4(b, 1); o.keep = float4(max(wh - b, 0), 1); return o; }\n";

// Everything made on the device is let go (the game made a new one, or there is none yet).
static void drop_device(void) {
    for (int i = 0; i < g_nt; i++) {
        for (int k = 0; k < 2; k++) { REL(g_t[i].srv[k]); REL(g_t[i].rtv[k]); REL(g_t[i].tex[k]); }
        g_t[i].valid = 0; g_t[i].tw = g_t[i].th = 0;
    }
    REL(g_vs); REL(g_ps); REL(g_blend); REL(g_dss); REL(g_rs);
    g_ctx = NULL; g_dev = NULL;
    g_nseen = g_tried = 0;
}

// The game's context and our own objects on its device. NULL: nothing can be cached now.
static ID3D11DeviceContext *device(void) {
    static LONG device_no;   // a new device can sit at the old one's address
    if (g_d3d_dev != g_dev || g_d3d_ctx != g_ctx || device_no != g_d3d_device_no) { drop_device(); g_dev = g_d3d_dev; g_ctx = g_d3d_ctx; device_no = g_d3d_device_no; }
    if (!g_ctx || !g_dev) return NULL;
    if (g_tried) return g_vs && g_ps && g_blend && g_dss && g_rs ? g_ctx : NULL;
    g_tried = 1;   // once per device
    pD3DCompile comp = (pD3DCompile)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    ID3DBlob *vb = NULL, *pb = NULL;
    if (comp && SUCCEEDED(comp(hlsl, strlen(hlsl), "uicache", NULL, NULL, "vs", "vs_4_0", 0, 0, &vb, NULL)) &&
        SUCCEEDED(comp(hlsl, strlen(hlsl), "uicache", NULL, NULL, "ps", "ps_4_0", 0, 0, &pb, NULL))) {
        ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &g_vs);
        ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &g_ps);
    }
    REL(vb); REL(pb);
    D3D11_BLEND_DESC bd = {0};   // scene x keep + add; the target's alpha is left alone
    bd.RenderTarget[0] = (D3D11_RENDER_TARGET_BLEND_DESC){TRUE, D3D11_BLEND_ONE, D3D11_BLEND_SRC1_COLOR, D3D11_BLEND_OP_ADD, D3D11_BLEND_ZERO,
                                                          D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE};
    ID3D11Device_CreateBlendState(g_dev, &bd, &g_blend);
    D3D11_DEPTH_STENCIL_DESC dd = {0};
    ID3D11Device_CreateDepthStencilState(g_dev, &dd, &g_dss);
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs);
    int ok = g_vs && g_ps && g_blend && g_dss && g_rs;
    if (g_log) g_log("ui cache: its shaders and states %s", ok ? "made" : "NOT made (no d3dcompiler_47?) - nothing is cached");
    return ok ? g_ctx : NULL;
}

// 1 if what a draw with this blend state leaves is "what was there x something + something" in colour.
static int blend_ok(ID3D11BlendState *bs) {
    if (!bs) return 1;   // no blending: the draw's own colour
    for (int i = 0; i < g_nseen; i++) if (g_seen[i].bs == bs) return g_seen[i].ok;
    D3D11_BLEND_DESC d;
    ID3D11BlendState_GetDesc(bs, &d);
    const D3D11_RENDER_TARGET_BLEND_DESC *r = &d.RenderTarget[0];
#define READS_ALPHA(f) ((f) == D3D11_BLEND_DEST_ALPHA || (f) == D3D11_BLEND_INV_DEST_ALPHA || (f) == D3D11_BLEND_SRC_ALPHA_SAT)
    int ok = !r->BlendEnable || (!READS_ALPHA(r->SrcBlend) && !READS_ALPHA(r->DestBlend) && r->BlendOp != D3D11_BLEND_OP_MIN && r->BlendOp != D3D11_BLEND_OP_MAX);
#undef READS_ALPHA
    if (g_nseen < NBLEND) { g_seen[g_nseen].bs = bs; g_seen[g_nseen].ok = ok; g_nseen++; }
    if (g_log) g_log("ui cache: blend state %p (on %d, colour %d/%d op %d, alpha %d/%d op %d, mask %x)%s", (void *)bs, r->BlendEnable, r->SrcBlend, r->DestBlend,
                     r->BlendOp, r->SrcBlendAlpha, r->DestBlendAlpha, r->BlendOpAlpha, r->RenderTargetWriteMask,
                     ok ? "" : " CANNOT be cached: a panel that uses it is drawn the game's way");
    return ok;
}

// ---- the four wrappers: in the context's function table only while a panel is drawn into its textures ----
#define VT ID3D11DeviceContext
static void (STDMETHODCALLTYPE *r_OMSetRenderTargets)(VT *, UINT, ID3D11RenderTargetView *const *, ID3D11DepthStencilView *);
static void (STDMETHODCALLTYPE *r_RSSetViewports)(VT *, UINT, const D3D11_VIEWPORT *);
static void (STDMETHODCALLTYPE *r_RSSetScissorRects)(VT *, UINT, const D3D11_RECT *);
static void (STDMETHODCALLTYPE *r_OMSetBlendState)(VT *, ID3D11BlendState *, const FLOAT *, UINT);
static int g_pass;   // the run of the panel's Draw: 0 into the black texture, 1 into the white one
static void *g_arg_self, *g_arg_details;   // its arguments (SysV: rdi, rsi, edx, cl, r8b), for the second run
static unsigned g_arg_a;
static char g_arg_b, g_arg_c;
static D3D11_VIEWPORT g_begin_vp;   // the viewport and scissor rectangle the first run started with
static D3D11_RECT g_begin_sc;
static int g_begin_nsc;
typedef SV char (*DrawFn)(void *self, void *details, unsigned a, char b, char c);
static int live(VT *c) { return g_red && !g_own && c == g_ctx; }

// The viewport the game asked for, as it has to be while our textures stand for its target (moved by their corner).
static void set_viewport(VT *c, int ours) {
    D3D11_VIEWPORT v = g_game_vp;
    if (ours) { v.TopLeftX -= (FLOAT)g_red->x; v.TopLeftY -= (FLOAT)g_red->y; }
    r_RSSetViewports(c, 1, &v);
}
// The same for the scissor rectangle.
static void set_scissor(VT *c, int ours) {
    if (!g_game_nsc) return;
    D3D11_RECT r = g_game_sc;
    if (ours) { r.left -= g_red->x; r.right -= g_red->x; r.top -= g_red->y; r.bottom -= g_red->y; g_sc_moved++; }
    r_RSSetScissorRects(c, 1, &r);
}
// OMSetRenderTargets: the old target stands for the black texture
static void STDMETHODCALLTYPE w_OMSetRenderTargets(VT *c, UINT n, ID3D11RenderTargetView *const *v, ID3D11DepthStencilView *dsv) {
    if (!live(c)) { r_OMSetRenderTargets(c, n, v, dsv); return; }
    ID3D11RenderTargetView *mine[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    int was = g_game_rt == g_bb;
    g_game_rt = n && v ? v[0] : NULL;
    int is = g_game_rt == g_bb && n <= D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
    if (is) {
        memcpy(mine, v, n * sizeof *v);
        mine[0] = g_red->rtv[g_pass];
        r_OMSetRenderTargets(c, n, mine, NULL);   // no depth buffer: ours are another size (the UI does not test depth)
    } else {
        if (g_game_rt == g_bb) g_red_bad = 1;
        r_OMSetRenderTargets(c, n, v, dsv);
    }
    if (is != was) { set_viewport(c, is); set_scissor(c, is); }   // its own render to texture and back
    PRT("ui cache probe:   target set: %u views, %s, depth view %p", n, is ? "the game's back buffer (the black texture stands in)" : "ANOTHER target", (void *)dsv);
}
static void STDMETHODCALLTYPE w_RSSetViewports(VT *c, UINT n, const D3D11_VIEWPORT *v) {
    if (!live(c) || !n || !v) { r_RSSetViewports(c, n, v); return; }
    D3D11_VIEWPORT mine[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    g_game_vp = v[0];
    PRT("ui cache probe:   viewport %.0f,%.0f %.0fx%.0f (%u)", v[0].TopLeftX, v[0].TopLeftY, v[0].Width, v[0].Height, n);
    if (g_game_rt != g_bb || n > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) { r_RSSetViewports(c, n, v); return; }
    memcpy(mine, v, n * sizeof *v);
    for (UINT i = 0; i < n; i++) { mine[i].TopLeftX -= (FLOAT)g_red->x; mine[i].TopLeftY -= (FLOAT)g_red->y; }
    r_RSSetViewports(c, n, mine);
}
static void STDMETHODCALLTYPE w_RSSetScissorRects(VT *c, UINT n, const D3D11_RECT *r) {
    if (!live(c) || !n || !r) { r_RSSetScissorRects(c, n, r); return; }
    g_game_sc = r[0]; g_game_nsc = 1;
    PRT("ui cache probe:   scissor rectangle %ld,%ld-%ld,%ld (%u)", r[0].left, r[0].top, r[0].right, r[0].bottom, n);
    if (g_game_rt != g_bb || n > 1) { r_RSSetScissorRects(c, n, r); return; }
    set_scissor(c, 1);
}
// a blend state set inside the panel: one the two-texture picture cannot stand for makes the redraw unusable
static void STDMETHODCALLTYPE w_OMSetBlendState(VT *c, ID3D11BlendState *bs, const FLOAT *f, UINT mask) {
    if (live(c) && !blend_ok(bs)) g_red_bad = 1;
    r_OMSetBlendState(c, bs, f, mask);
}
// on: the wrappers go in, taking what each slot holds now as the original. off: each wrapper still in its slot comes
// out (one the runtime replaced meanwhile is left as it is). 0: the table could not be written.
static int wrappers(VT *c, int on) {
    static const struct { size_t off; void **orig; void *wrap; } H[] = {
#define UH(m) {offsetof(ID3D11DeviceContextVtbl, m), (void **)&r_##m, (void *)w_##m}
        UH(OMSetRenderTargets), UH(RSSetViewports), UH(RSSetScissorRects), UH(OMSetBlendState),
#undef UH
    };
    void **vt = *(void ***)c;
    DWORD old;
    if (!VirtualProtect(vt, sizeof(ID3D11DeviceContextVtbl), PAGE_READWRITE, &old)) return 0;
    for (size_t i = 0; i < sizeof H / sizeof H[0]; i++) {
        void **slot = &vt[H[i].off / sizeof(void *)];
        if (on) { if (*slot != H[i].wrap) { *H[i].orig = *slot; *slot = H[i].wrap; } }
        else if (*slot == H[i].wrap) *slot = *H[i].orig;
    }
    VirtualProtect(vt, sizeof(ID3D11DeviceContextVtbl), old, &old);
    return 1;
}
#undef VT

// The panel's rectangle on the target, MARGIN around it, cut to the target.
static void panel_rect(const float rect[4], unsigned tw, unsigned th, int out[4]) {
    float sx = g_root_wh[0] > 0 ? (float)tw / g_root_wh[0] : 1, sy = g_root_wh[1] > 0 ? (float)th / g_root_wh[1] : 1;
    float x0 = (rect[0] - MARGIN) * sx, y0 = (rect[1] - MARGIN) * sy, x1 = (rect[0] + rect[2] + MARGIN) * sx, y1 = (rect[1] + rect[3] + MARGIN) * sy;
    out[0] = x0 > 0 ? (int)x0 : 0; out[1] = y0 > 0 ? (int)y0 : 0;
    out[2] = x1 < (float)tw ? (int)x1 + 1 : (int)tw; out[3] = y1 < (float)th ? (int)y1 + 1 : (int)th;
}

// The panel's two textures: at least w x h, grown in steps of 64 and never shrunk. Half floats whatever the target
// is: an additive draw must not clip in the white one, and the numbers the game's shaders write go in and come out
// unchanged.
static int make_textures(Target *t, unsigned w, unsigned h) {
    if (t->tex[0] && t->tw >= w && t->th >= h) return 1;
    w = (w + 63) & ~63u; h = (h + 63) & ~63u;
    if (w < t->tw) w = t->tw;
    if (h < t->th) h = t->th;
    t->valid = 0;
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        REL(t->srv[i]); REL(t->rtv[i]); REL(t->tex[i]);
        ok = ok && SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t->tex[i])) &&
             SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)t->tex[i], NULL, &t->rtv[i])) &&
             SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t->tex[i], NULL, &t->srv[i]));
    }
    if (!ok) {
        for (int i = 0; i < 2; i++) { REL(t->srv[i]); REL(t->rtv[i]); REL(t->tex[i]); }
        t->tw = t->th = 0;
        return 0;
    }
    t->tw = w; t->th = h;
    if (g_log) g_log("ui cache: %s gets two %ux%u half-float textures (%u MB)", t->name, w, h, (w * h + 32768) / 65536);
    return 1;
}

// From here the game's draws go into the panel's textures. 0: not possible now, it is drawn the game's way.
static int redraw_begin(Target *t, ID3D11DeviceContext *c, const float rect[4]) {
    static const FLOAT black[4] = {0, 0, 0, 0}, white[4] = {1, 1, 1, 1};
    ID3D11Resource *res = NULL;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    D3D11_TEXTURE2D_DESC td = {0};
    int r[4];
    UINT nvp = 1;
    g_bb = NULL; g_dsv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(c, 1, &g_bb, &g_dsv);
    if (g_bb) ID3D11RenderTargetView_GetResource(g_bb, &res);
    if (res) ID3D11Resource_GetType(res, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    REL(res);
    panel_rect(rect, td.Width, td.Height, r);
    ID3D11DeviceContext_RSGetViewports(c, &nvp, &g_game_vp);
    UINT nsc = 1;
    g_game_nsc = 0;
    ID3D11DeviceContext_RSGetScissorRects(c, &nsc, &g_game_sc);
    if (nsc) g_game_nsc = 1;
    if (!td.Width || td.SampleDesc.Count != 1 || !nvp || r[2] <= r[0] || r[3] <= r[1] || !make_textures(t, (unsigned)(r[2] - r[0]), (unsigned)(r[3] - r[1])) ||
        !wrappers(c, 1)) {
        REL(g_bb); REL(g_dsv);
        return 0;
    }
    t->x = r[0]; t->y = r[1];
    g_red_bad = 0;
    ID3D11DeviceContext_ClearRenderTargetView(c, t->rtv[0], black);
    ID3D11DeviceContext_ClearRenderTargetView(c, t->rtv[1], white);
    r_OMSetRenderTargets(c, 1, &t->rtv[0], NULL);
    g_game_rt = g_bb;
    g_red = t;
    g_pass = 0;
    set_viewport(c, 1);
    set_scissor(c, 1);
    g_begin_vp = g_game_vp; g_begin_sc = g_game_sc; g_begin_nsc = g_game_nsc;
    {   // the blend state the panel starts with (the game sets none inside it as a rule: the wrapper sees the rest)
        ID3D11BlendState *bs = NULL; FLOAT bf[4]; UINT mask;
        ID3D11DeviceContext_OMGetBlendState(c, &bs, bf, &mask);
        if (!blend_ok(bs)) g_red_bad = 1;
        REL(bs);
    }
    g_pr_draws = 0;
    if (g_pr_on && g_pr_traced < 3 && g_log) {
        g_pr_traced++;
        g_pr_trace = 1;
        g_log("ui cache probe: redraw %u of %s begins: the game's target %ux%u format %u, depth view %p; viewport %.0f,%.0f %.0fx%.0f; "
              "scissor %ld,%ld-%ld,%ld; panel %.1f,%.1f %.1fx%.1f of a %.0fx%.0f interface; textures at %d,%d", g_pr_traced, t->name,
              td.Width, td.Height, (unsigned)td.Format, (void *)g_dsv, g_game_vp.TopLeftX, g_game_vp.TopLeftY, g_game_vp.Width, g_game_vp.Height,
              g_game_sc.left, g_game_sc.top, g_game_sc.right, g_game_sc.bottom, rect[0], rect[1], rect[2], rect[3], g_root_wh[0], g_root_wh[1], t->x, t->y);
    }
    return 1;
}

// The game has drawn the panel: the device gets its target and its viewport again.
static void redraw_end(ID3D11DeviceContext *c) {
    Target *t = g_red;
    g_red = NULL;
    wrappers(c, 0);
    if (g_game_rt == g_bb) {
        ID3D11DeviceContext_OMSetRenderTargets(c, 1, &g_bb, g_dsv);
        ID3D11DeviceContext_RSSetViewports(c, 1, &g_game_vp);
        if (g_game_nsc) ID3D11DeviceContext_RSSetScissorRects(c, 1, &g_game_sc);
    } else g_red_bad = 1;   // it left another target bound: that one stays, and this panel is not cached again
    REL(g_bb); REL(g_dsv);
    t->valid = !g_red_bad;
    if (g_pr_trace && g_log) { g_log("ui cache probe: redraw ends: %u draws, %s", g_pr_draws, g_red_bad ? "NOT cacheable (bad)" : "cached"); g_pr_trace = 0; }
    static int told;
    if (!told && g_game_nsc && g_log) {
        told = 1;
        g_log("ui cache: %s redrawn with the game's scissor rectangle %ld,%ld-%ld,%ld moved to the cache's corner %d,%d (%u moved so far)", t->name,
              g_game_sc.left, g_game_sc.top, g_game_sc.right, g_game_sc.bottom, t->x, t->y, g_sc_moved);
    }
    if (g_red_bad) {
        t->off = 1;   // what is in the textures is shown this once
        if (g_log) g_log("ui cache: %s is drawn the game's way from now on (blending or a render target the cache cannot stand in for)", t->name);
    }
}

// The textures over the scene. Everything set for it is put back: the game keeps its own record of what is bound.
static void composite(Target *t, ID3D11DeviceContext *c) {
    ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL; ID3D11GeometryShader *gs = NULL;
    ID3D11InputLayout *il = NULL; ID3D11ShaderResourceView *srv[2] = {NULL, NULL}; ID3D11BlendState *bs = NULL;
    ID3D11DepthStencilState *dss = NULL; ID3D11RasterizerState *rs = NULL;
    D3D11_PRIMITIVE_TOPOLOGY topo; FLOAT bf[4]; UINT mask, sref, nvp = 1;
    D3D11_VIEWPORT vp = {0}, mine = {(FLOAT)t->x, (FLOAT)t->y, (FLOAT)t->tw, (FLOAT)t->th, 0, 1};
    ID3D11DeviceContext_VSGetShader(c, &vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(c, &gs, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(c, &il);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &topo);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 2, srv);
    ID3D11DeviceContext_OMGetBlendState(c, &bs, bf, &mask);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &dss, &sref);
    ID3D11DeviceContext_RSGetState(c, &rs);
    ID3D11DeviceContext_RSGetViewports(c, &nvp, &vp);

    static const FLOAT one[4] = {1, 1, 1, 1};
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, g_ps, NULL, 0);
    if (gs) ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 2, t->srv);
    ID3D11DeviceContext_OMSetBlendState(c, g_blend, one, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(c, g_dss, 0);
    ID3D11DeviceContext_RSSetState(c, g_rs);
    ID3D11DeviceContext_RSSetViewports(c, 1, &mine);
    ID3D11DeviceContext_Draw(c, 3, 0);

    ID3D11DeviceContext_IASetInputLayout(c, il);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, topo);
    ID3D11DeviceContext_VSSetShader(c, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, ps, NULL, 0);
    if (gs) ID3D11DeviceContext_GSSetShader(c, gs, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 2, srv);
    ID3D11DeviceContext_OMSetBlendState(c, bs, bf, mask);
    ID3D11DeviceContext_OMSetDepthStencilState(c, dss, sref);
    ID3D11DeviceContext_RSSetState(c, rs);
    if (nvp) ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    REL(vs); REL(ps); REL(gs); REL(il); REL(srv[0]); REL(srv[1]); REL(bs); REL(dss); REL(rs);
}

// Once a frame: 1 for 250 ms after a mouse button or a key that can change the cards goes down or comes up (held, it
// does not count; the camera's keys and the modifiers alone do not count at all, nor the mouse moving or its wheel).
static POINT g_cursor;
SHORT (WINAPI *uc_key)(int) = GetAsyncKeyState;   // the test has keys of its own
static int input_live(DWORD now) {
    static const unsigned char keys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2, VK_BACK, VK_TAB, VK_RETURN, VK_ESCAPE, VK_SPACE,
                                         VK_PRIOR, VK_NEXT, VK_END, VK_HOME, VK_INSERT, VK_DELETE, '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                                         'B', 'C', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'R', 'T', 'U', 'V', 'Y',
                                         VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12};
    static LONG frame = -1;
    static DWORD last, until;
    static unsigned char was[sizeof keys];
    if (frame != g_frame_no) {
        frame = g_frame_no;
        LASTINPUTINFO li = {sizeof li, 0};
        GetCursorPos(&g_cursor);
        GetLastInputInfo(&li);
        if (li.dwTime != last || !uc_in_game)
            for (unsigned i = 0; i < sizeof keys; i++) {
                unsigned char down = (uc_key(keys[i]) & 0x8000) != 0;
                if (down != was[i]) { was[i] = down; until = now + 250; }
            }
        last = li.dwTime;
    }
    return (LONG)(until - now) > 0;
}

// The cursor over the component (its rectangle in the UI's units, the window taken as the whole root), 16 px around.
static int cursor_over(const float rect[4]) {
    if (!uc_in_game || g_root_wh[0] <= 0 || g_root_wh[1] <= 0) return 0;
    RECT r;
    HWND w = GetForegroundWindow();
    POINT p = g_cursor;
    if (!w || !GetClientRect(w, &r) || r.right <= 0 || r.bottom <= 0 || !ScreenToClient(w, &p)) return 0;
    float x = (float)p.x * g_root_wh[0] / (float)r.right, y = (float)p.y * g_root_wh[1] / (float)r.bottom;
    float m = 16 * g_root_wh[0] / (float)r.right;
    return x >= rect[0] - m && y >= rect[1] - m && x <= rect[0] + rect[2] + m && y <= rect[1] + rect[3] + m;
}

// Once a frame: the root's size.
static void frame_tick(void) {
    static LONG frame = -1;
    if (frame == g_frame_no) return;
    frame = g_frame_no;
    void *root = uc_root ? *uc_root : NULL;
    g_root_wh[0] = root ? uc_width(root) : 0;
    g_root_wh[1] = root ? uc_height(root) : 0;
}

// 1 if the component has an ancestor of that name (12 levels up at most).
static int is_under(void *self, const char *name) {
    for (int i = 0; i < 12 && (self = *(void **)((char *)self + C_PARENT)) != NULL; i++) {
        const char *n = *(const char **)((char *)self + C_NAME);
        if (n && !strcmp(n, name)) return 1;
    }
    return 0;
}

// Before the game's Draw of a component (from the entry stub; *ret is its return address).
// 0: the game's Draw runs, untouched - or, for a panel to be redrawn, with its return address swapped for the stub
// that calls uic64_post. 1: the panel was shown from its textures, Draw is not run.
SV int uic64_pre(void *self, void **ret, void *details, const uint64_t *regs) {   // regs: r9, r8, rcx, rdx as the caller passed them
    if (g_red || !g_on || GetCurrentThreadId() != g_d3d_thread) return 0;   // inside a redraw, off, or not the drawing thread
    frame_tick();
    const char *name = *(const char **)((char *)self + C_NAME);
    Target *t = NULL;
    if (name)
        for (int i = 0; i < g_nt && !t; i++) {
            Target *x = &g_t[i];
            // a name is looked for only while its component has not been drawn for a while (it may be gone)
            if (x->comp == self ? !strcmp(name, x->name)
                                : (!x->comp || g_frame_no - x->seen > 30) && !strcmp(name, x->name) && (!x->under || is_under(self, x->under)))
                t = x;
        }
    if (!t) return 0;
    if (t->comp != self && g_log) g_log("ui cache: %s is component %p", t->name, self);
    t->comp = self;
    t->seen = g_frame_no;
    pr_count(t);
    ID3D11DeviceContext *c = !t->off && *((unsigned char *)self + C_VISIBLE) ? device() : NULL;
    if (!c) { t->valid = 0; return 0; }
    const float *pos = (const float *)((char *)self + C_POS);
    float rect[4] = {pos[0], pos[1], uc_width(self), uc_height(self)};
    DWORD now = GetTickCount();
    int in = input_live(now);
    int why = !t->valid ? 1 : memcmp(details, t->details, DETAILS) || memcmp(rect, t->rect, sizeof rect) ? 2 : in ? 3 : cursor_over(rect) ? 5 :
              now - t->drawn >= INTERVAL ? 4 : 0;
    g_batch = *(void **)details;   // DrawDetails::sprites
    flush_pending();               // what was gathered before the panel is drawn now, where it belongs
    if (t->valid && pr_want_game_way()) {   // probe: this frame the game's own cards, for a picture of them
        g_pr_game_way = 1;
        g_pr_t = t;
        g_pr_path = 8;
        g_uic_ret = *ret;
        extern void uic64_stub_after(void);
        *ret = (void *)uic64_stub_after;
        return 0;
    }
    g_pr_path = why;   // probe: what this frame does with the panel (0 = shown from the textures)
    if (!why) {
        t->reuse++;
        composite(t, c);
        if (g_pr_on && g_pr_stage == 3) {
            char nm[32];
            _snprintf(nm, sizeof nm, "reused_%d", g_pr_n + 1);
            pr_save_target(nm, t);
            g_pr_n++; g_pr_stage = 0; g_pr_next = GetTickCount() + 1500;
        }
        return 1;
    }
    if (!redraw_begin(t, c, rect)) { t->valid = 0; return 0; }
    memcpy(t->details, details, DETAILS);
    memcpy(t->rect, rect, sizeof rect);
    t->drawn = now;
    t->why[why]++;
    g_arg_self = self; g_arg_details = details;
    g_arg_a = (unsigned)regs[3]; g_arg_b = (char)regs[2]; g_arg_c = (char)regs[1];
    g_uic_ret = *ret;
    extern void uic64_stub_after(void);
    *ret = (void *)uic64_stub_after;
    return 0;
}

// The game's Draw of a redrawn panel has returned (from the second stub).
SV void uic64_post(void) {
    if (g_pr_game_way) {   // probe: the game has drawn its own cards
        g_pr_game_way = 0;
        flush_pending();
        char nm[32];
        _snprintf(nm, sizeof nm, "gameway_%d", g_pr_n + 1);
        pr_save_target(nm, g_pr_t);
        g_pr_stage = 2;
        return;
    }
    Target *t = g_red;
    if (!t) return;
    flush_pending();   // the first run's last quads: into the black texture
    // the second run, into the white texture, from the state the first one started with (the game's record of what is
    // bound is what the first run left, which is what is bound: nothing else was changed behind it)
    extern void uic64_stub_entry(void);
    g_pass = 1;
    r_OMSetRenderTargets(g_ctx, 1, &t->rtv[1], NULL);
    g_game_rt = g_bb;
    g_game_vp = g_begin_vp; g_game_sc = g_begin_sc; g_game_nsc = g_begin_nsc;
    set_viewport(g_ctx, 1);
    set_scissor(g_ctx, 1);
    void *volatile entry = (void *)uic64_stub_entry;   // through a data pointer: the stub's C type says (void), it takes Draw's
    ((DrawFn)entry)(g_arg_self, g_arg_details, g_arg_a, g_arg_b, g_arg_c);   // pre sees g_red: the game's way
    flush_pending();
    g_pass = 0;
    redraw_end(g_ctx);
    composite(t, g_ctx);
    if (g_pr_on && g_pr_stage == 2) {
        char nm[32];
        _snprintf(nm, sizeof nm, "black_%d", g_pr_n + 1); pr_save(nm, t->tex[0], 0, 0, (int)t->tw, (int)t->th);
        _snprintf(nm, sizeof nm, "white_%d", g_pr_n + 1); pr_save(nm, t->tex[1], 0, 0, (int)t->tw, (int)t->th);
        _snprintf(nm, sizeof nm, "redrawn_%d", g_pr_n + 1); pr_save_target(nm, t);
        g_pr_stage = 3;
    }
}

// Entry: rsp -> the return address (rsp = 8 mod 16). The argument registers are kept around uic64_pre; then either
// the function's own first 17 bytes run and it goes on behind them, or it returns 1 without running.
__attribute__((naked)) void uic64_stub_entry(void) {
    __asm__ volatile(
        "push %rdi\n push %rsi\n push %rdx\n push %rcx\n push %r8\n push %r9\n sub $8, %rsp\n"
        "mov %rsi, %rdx\n"
        "lea 56(%rsp), %rsi\n"
        "lea 8(%rsp), %rcx\n"
        "call uic64_pre\n"
        "add $8, %rsp\n pop %r9\n pop %r8\n pop %rcx\n pop %rdx\n pop %rsi\n pop %rdi\n"
        "test %eax, %eax\n jnz 1f\n"
        "push %rbp\n push %r15\n push %r14\n push %r13\n push %r12\n push %rbx\n sub $0x148, %rsp\n"
        "jmp *g_uic_draw_body(%rip)\n"
        "1: mov $1, %eax\n ret\n");
}
// A redrawn panel's Draw returns here: rax = its result, rsp = 0 mod 16.
__attribute__((naked)) void uic64_stub_after(void) {
    __asm__ volatile(
        "push %rax\n sub $8, %rsp\n"
        "call uic64_post\n"
        "add $8, %rsp\n pop %rax\n"
        "jmp *g_uic_ret(%rip)\n");
}

static void add_target(const char *name, const char *under) {
    if (g_nt < NTARGET && *name) { strncat(g_t[g_nt].name, name, sizeof g_t[g_nt].name - 1); g_t[g_nt].under = under; g_nt++; }
}
void uicache64_setup(void (*log)(const char *fmt, ...)) {
    g_log = log;
    // The battle's unit cards only: the campaign's are another layout (units_panel), which was never run through
    // the cache.
    if (!g_nt) add_target("cards_panel", "battle_orders");
}
void uicache64_counts(unsigned *reuse, unsigned *redraw) {
    *reuse = g_t[0].reuse; *redraw = g_t[0].why[1] + g_t[0].why[2] + g_t[0].why[3] + g_t[0].why[4] + g_t[0].why[5];
}

#ifndef UICACHE64_TEST
// 1 installed, 0 the engine's code differs, -1 a thread inside the site, -2 VirtualProtect failed.
// Runs while every other thread is paused.
int install_uicache64(void) {
    if (memcmp((void *)(uintptr_t)UC_DRAW, UC_DRAW_B, 17) || memcmp((void *)(uintptr_t)UC_WIDTH, UC_DIM_B, 8) ||
        memcmp((void *)(uintptr_t)UC_HEIGHT, UC_DIM_B, 8) || memcmp((void *)(uintptr_t)UC_PRDRAW, UC_PRDRAW_B, 12)) return 0;
    if (thread_inside(UC_DRAW, 17)) return -1;
    g_uic_draw_body = (void *)(uintptr_t)(UC_DRAW + 17);
    uint8_t j[17] = {0xFF, 0x25, 0, 0, 0, 0};
    uint64_t a = (uint64_t)(uintptr_t)uic64_stub_entry;
    memcpy(j + 6, &a, 8);
    j[14] = j[15] = j[16] = 0x90;
    return patch_code((void *)(uintptr_t)UC_DRAW, j, 17) ? 1 : -2;
}
#endif
