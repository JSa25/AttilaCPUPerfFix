// Dev-only measurement (passtime_on.txt containing "gpu"): GPU time per frame of each render stage, from D3D11
// timestamp queries issued where passtime.c's hooks enter and leave the stage driver FUN_1176a450 (one call per stage,
// the stage object's vtable names it: AGENT_LOG 50/61) and the render level FUN_11778e90. Measuring only: the queries
// change nothing that is drawn. The device and immediate context come from the game's own D3D11CreateDevice, which it
// looks up with GetProcAddress (+0x179B8D4, import read at the call): that import hands out a wrapper for this name.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <string.h>
#include <stddef.h>

#define GT_SLOTS 6    // frames in flight before a frame's results are read
#define GT_Q 1024     // timestamps per frame (~55 stages x 2, scene calls, render level, effects-update calls)
#define GT_DEPTH 64
#define GT_KEYS 256

extern unsigned g_pt_view_id, g_pt_scene_id, g_pt_fx_id, g_pt_base;  // passtime.c
extern DWORD g_pt_main;
const char *pt_name(unsigned id);                          // passtime.c
extern void (*g_gt_enter)(unsigned id, const void *obj);  // passtime.c calls these from its stubs
extern void (*g_gt_exit)(unsigned id);
extern void (*g_gt_frame)(void);
extern void (*g_gt_report)(void (*log)(const char *fmt, ...));

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11Query *g_disj[GT_SLOTS], *g_ts[GT_SLOTS][GT_Q];
static struct { unsigned key, b, e; } g_rec[GT_SLOTS][GT_Q / 2];
static unsigned g_nq[GT_SLOTS], g_nrec[GT_SLOTS], g_slot;
static int g_live[GT_SLOTS], g_open;  // live: the slot holds a finished frame whose results are not read yet
static struct { unsigned key, b; } g_stk[GT_DEPTH];
static unsigned g_sp;
static unsigned g_key[GT_KEYS], g_n[GT_KEYS], g_nkeys;
static double g_ms[GT_KEYS];
static unsigned g_frames, g_late, g_disjoint, g_full, g_devices;
static DWORD g_create_tid;
// The scene renderer FUN_117fa310 is keyed by its call number in the frame (calls 0-3 are the shadow cascades,
// AGENT_LOG 61); every 64th frame each call's depth target, first colour target and viewport are read at its exit.
#define GT_SCENE_KEY 0x8000u
#define GT_SCENES 32
static unsigned g_scene_n, g_frame_no;
static struct { UINT dw, dh, dfmt, darr, cw, ch, cfmt, seen; float vx, vy, vw, vh; } g_tgt[GT_SCENES];

static void read_targets(unsigned n) {
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &rtv, &dsv);
    if (dsv) {
        ID3D11Resource *r;
        ID3D11DepthStencilView_GetResource(dsv, &r);
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11Resource_GetType(r, &dim);
        D3D11_TEXTURE2D_DESC d;
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
            g_tgt[n].dw = d.Width; g_tgt[n].dh = d.Height; g_tgt[n].dfmt = d.Format; g_tgt[n].darr = d.ArraySize;
        }
        ID3D11Resource_Release(r);
        ID3D11DepthStencilView_Release(dsv);
    }
    if (rtv) {
        ID3D11Resource *r;
        ID3D11RenderTargetView_GetResource(rtv, &r);
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11Resource_GetType(r, &dim);
        D3D11_TEXTURE2D_DESC d;
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
            g_tgt[n].cw = d.Width; g_tgt[n].ch = d.Height; g_tgt[n].cfmt = d.Format;
        }
        ID3D11Resource_Release(r);
        ID3D11RenderTargetView_Release(rtv);
    }
    UINT nv = 1;
    D3D11_VIEWPORT vp = {0};
    ID3D11DeviceContext_RSGetViewports(g_ctx, &nv, &vp);
    g_tgt[n].vx = vp.TopLeftX; g_tgt[n].vy = vp.TopLeftY; g_tgt[n].vw = vp.Width; g_tgt[n].vh = vp.Height;
    g_tgt[n].seen++;
}

static void fx_patch(int on);

static void release_all(void) {
    for (int s = 0; s < GT_SLOTS; s++) {
        if (g_disj[s]) ID3D11Query_Release(g_disj[s]);
        g_disj[s] = NULL;
        for (int q = 0; q < GT_Q; q++) {
            if (g_ts[s][q]) ID3D11Query_Release(g_ts[s][q]);
            g_ts[s][q] = NULL;
        }
        g_live[s] = 0;
        g_nq[s] = g_nrec[s] = 0;
    }
    fx_patch(0);
    if (g_ctx) ID3D11DeviceContext_Release(g_ctx);
    if (g_dev) ID3D11Device_Release(g_dev);
    g_ctx = NULL;
    g_dev = NULL;
    g_open = 0;
    g_sp = 0;
}

// A new device (first creation, or the game recreating it): measure on that one.
static void gt_attach(ID3D11Device *dev, ID3D11DeviceContext *ctx) {
    release_all();
    D3D11_QUERY_DESC dj = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, ts = {D3D11_QUERY_TIMESTAMP, 0};
    for (int s = 0; s < GT_SLOTS; s++) {
        if (FAILED(ID3D11Device_CreateQuery(dev, &dj, &g_disj[s]))) goto fail;
        for (int q = 0; q < GT_Q; q++)
            if (FAILED(ID3D11Device_CreateQuery(dev, &ts, &g_ts[s][q]))) goto fail;
    }
    ID3D11Device_AddRef(dev);  // kept alive while we hold its queries
    ID3D11DeviceContext_AddRef(ctx);
    g_dev = dev;
    g_ctx = ctx;
    g_devices++;
    g_create_tid = GetCurrentThreadId();
    return;
fail:
    for (int s = 0; s < GT_SLOTS; s++) {  // release_all would also release dev/ctx, which we did not AddRef yet
        if (g_disj[s]) ID3D11Query_Release(g_disj[s]);
        g_disj[s] = NULL;
        for (int q = 0; q < GT_Q; q++) {
            if (g_ts[s][q]) ID3D11Query_Release(g_ts[s][q]);
            g_ts[s][q] = NULL;
        }
    }
}

static void add(unsigned key, double ms) {
    unsigned i = 0;
    while (i < g_nkeys && g_key[i] != key) i++;
    if (i == g_nkeys) {
        if (g_nkeys == GT_KEYS) return;
        g_key[i] = key;
        g_ms[i] = 0;
        g_n[i] = 0;
        g_nkeys++;
    }
    g_ms[i] += ms;
    g_n[i]++;
}

// Reads a finished frame's timestamps (never waits: a frame whose results are not there yet is dropped).
static void collect(unsigned s) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g_disj[s], &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        g_late++;
        return;
    }
    if (dj.Disjoint || !dj.Frequency) {
        g_disjoint++;
        return;
    }
    for (unsigned r = 0; r < g_nrec[s]; r++) {
        UINT64 b, e;
        if (ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g_ts[s][g_rec[s][r].b], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g_ts[s][g_rec[s][r].e], &e, sizeof e, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            g_late++;
            return;
        }
        add(g_rec[s][r].key, e >= b ? (double)(e - b) * 1000.0 / (double)dj.Frequency : 0);
    }
    g_frames++;
}

// Effects update (FUN_11893cc0, AGENT_LOG 66: ~1-3 ms of GPU work for ~0.05-0.4 ms of CPU): while it runs on the main
// thread, the context methods that can cause GPU work are wrapped in timestamps and grouped by method and resource.
// The wrappers sit in the context's vtable for the whole session and pass straight through outside that window.
#define FX_KEY 0x9000u
#define FX_N 256
enum { M_DRAWIDX, M_DRAW, M_MAP, M_UNMAP, M_DRAWIDXINST, M_DRAWINST, M_DRAWAUTO, M_DRAWIDXINDIR, M_DRAWINSTINDIR,
       M_DISPATCH, M_DISPATCHINDIR, M_COPYREGION, M_COPY, M_UPDATE, M_CLEARRTV, M_CLEARDSV, M_GENMIPS, M_COUNT };
static const char *const FX_NAME[M_COUNT] = {"DrawIndexed", "Draw", "Map", "Unmap", "DrawIndexedInstanced", "DrawInstanced",
    "DrawAuto", "DrawIndexedInstancedIndirect", "DrawInstancedIndirect", "Dispatch", "DispatchIndirect",
    "CopySubresourceRegion", "CopyResource", "UpdateSubresource", "ClearRenderTargetView", "ClearDepthStencilView", "GenerateMips"};
typedef struct { UINT method, dim, w, h, d, fmt, usage, bind, extra; } fx_sig;  // extra: map type
static struct { fx_sig s; unsigned long long calls, amount; } g_fx[FX_N];
static unsigned g_nfx, g_fx_depth, g_fx_frames, g_in_wrap;

static void res_sig(ID3D11Resource *r, fx_sig *s) {
    if (!r) return;
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    s->dim = dim;
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d;
        ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &d);
        s->w = d.ByteWidth; s->usage = d.Usage; s->bind = d.BindFlags;
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
        s->w = d.Width; s->h = d.Height; s->d = d.ArraySize; s->fmt = d.Format; s->usage = d.Usage; s->bind = d.BindFlags;
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        D3D11_TEXTURE3D_DESC d;
        ID3D11Texture3D_GetDesc((ID3D11Texture3D *)r, &d);
        s->w = d.Width; s->h = d.Height; s->d = d.Depth; s->fmt = d.Format; s->usage = d.Usage; s->bind = d.BindFlags;
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
        D3D11_TEXTURE1D_DESC d;
        ID3D11Texture1D_GetDesc((ID3D11Texture1D *)r, &d);
        s->w = d.Width; s->d = d.ArraySize; s->fmt = d.Format; s->usage = d.Usage; s->bind = d.BindFlags;
    }
}

// Before a wrapped call: ~0 when not measuring, else (entry << 16 | begin timestamp), or (entry << 16 | 0xFFFF).
static unsigned fx_pre(ID3D11DeviceContext *c, unsigned method, ID3D11Resource *res, ID3D11View *view, UINT extra,
                       unsigned long long amount) {
    if (!g_fx_depth || g_in_wrap || c != g_ctx || !g_open || GetCurrentThreadId() != g_pt_main) return ~0u;
    fx_sig s = {method, 0, 0, 0, 0, 0, 0, 0, extra};
    if (view) {
        ID3D11Resource *vr = NULL;
        ID3D11View_GetResource(view, &vr);
        res_sig(vr, &s);
        if (vr) ID3D11Resource_Release(vr);
    } else
        res_sig(res, &s);
    unsigned e = 0;
    while (e < g_nfx && memcmp(&g_fx[e].s, &s, sizeof s)) e++;
    if (e == g_nfx) {
        if (g_nfx == FX_N) return ~0u;
        g_fx[e].s = s;
        g_fx[e].calls = g_fx[e].amount = 0;
        g_nfx++;
    }
    g_fx[e].calls++;
    g_fx[e].amount += amount;
    g_in_wrap = 1;
    if (g_nq[g_slot] + 2 > GT_Q || g_nrec[g_slot] >= GT_Q / 2) { g_full++; return e << 16 | 0xFFFF; }
    unsigned b = g_nq[g_slot]++;
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g_ts[g_slot][b]);
    return e << 16 | b;
}

static void fx_post(unsigned t) {
    if (t == ~0u) return;
    g_in_wrap = 0;
    fx_patch(2);  // the call may have made the runtime rewrite entries (seen with UpdateSubresource in gputime_test)
    if ((t & 0xFFFF) == 0xFFFF) return;
    unsigned s = g_slot, e = g_nq[s]++;
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g_ts[s][e]);
    g_rec[s][g_nrec[s]].key = FX_KEY + (t >> 16);
    g_rec[s][g_nrec[s]].b = t & 0xFFFF;
    g_rec[s][g_nrec[s]].e = e;
    g_nrec[s]++;
}

#define VT ID3D11DeviceContext
static void(STDMETHODCALLTYPE *r_DrawIndexed)(VT *, UINT, UINT, INT);
static void STDMETHODCALLTYPE w_DrawIndexed(VT *c, UINT n, UINT s, INT b) {
    unsigned t = fx_pre(c, M_DRAWIDX, NULL, NULL, 0, n); r_DrawIndexed(c, n, s, b); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_Draw)(VT *, UINT, UINT);
static void STDMETHODCALLTYPE w_Draw(VT *c, UINT n, UINT s) {
    unsigned t = fx_pre(c, M_DRAW, NULL, NULL, 0, n); r_Draw(c, n, s); fx_post(t);
}
static HRESULT(STDMETHODCALLTYPE *r_Map)(VT *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
static HRESULT STDMETHODCALLTYPE w_Map(VT *c, ID3D11Resource *r, UINT sub, D3D11_MAP type, UINT fl, D3D11_MAPPED_SUBRESOURCE *m) {
    unsigned t = fx_pre(c, M_MAP, r, NULL, type, 0); HRESULT hr = r_Map(c, r, sub, type, fl, m); fx_post(t); return hr;
}
static void(STDMETHODCALLTYPE *r_Unmap)(VT *, ID3D11Resource *, UINT);
static void STDMETHODCALLTYPE w_Unmap(VT *c, ID3D11Resource *r, UINT sub) {
    unsigned t = fx_pre(c, M_UNMAP, r, NULL, 0, 0); r_Unmap(c, r, sub); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_DrawIndexedInstanced)(VT *, UINT, UINT, UINT, INT, UINT);
static void STDMETHODCALLTYPE w_DrawIndexedInstanced(VT *c, UINT n, UINT inst, UINT s, INT b, UINT si) {
    unsigned t = fx_pre(c, M_DRAWIDXINST, NULL, NULL, 0, (unsigned long long)n * inst); r_DrawIndexedInstanced(c, n, inst, s, b, si); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_DrawInstanced)(VT *, UINT, UINT, UINT, UINT);
static void STDMETHODCALLTYPE w_DrawInstanced(VT *c, UINT n, UINT inst, UINT s, UINT si) {
    unsigned t = fx_pre(c, M_DRAWINST, NULL, NULL, 0, (unsigned long long)n * inst); r_DrawInstanced(c, n, inst, s, si); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_DrawAuto)(VT *);
static void STDMETHODCALLTYPE w_DrawAuto(VT *c) {
    unsigned t = fx_pre(c, M_DRAWAUTO, NULL, NULL, 0, 0); r_DrawAuto(c); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_DrawIndexedInstancedIndirect)(VT *, ID3D11Buffer *, UINT);
static void STDMETHODCALLTYPE w_DrawIndexedInstancedIndirect(VT *c, ID3D11Buffer *a, UINT o) {
    unsigned t = fx_pre(c, M_DRAWIDXINDIR, (ID3D11Resource *)a, NULL, 0, 0); r_DrawIndexedInstancedIndirect(c, a, o); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_DrawInstancedIndirect)(VT *, ID3D11Buffer *, UINT);
static void STDMETHODCALLTYPE w_DrawInstancedIndirect(VT *c, ID3D11Buffer *a, UINT o) {
    unsigned t = fx_pre(c, M_DRAWINSTINDIR, (ID3D11Resource *)a, NULL, 0, 0); r_DrawInstancedIndirect(c, a, o); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_Dispatch)(VT *, UINT, UINT, UINT);
static void STDMETHODCALLTYPE w_Dispatch(VT *c, UINT x, UINT y, UINT z) {
    unsigned t = fx_pre(c, M_DISPATCH, NULL, NULL, 0, (unsigned long long)x * y * z); r_Dispatch(c, x, y, z); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_DispatchIndirect)(VT *, ID3D11Buffer *, UINT);
static void STDMETHODCALLTYPE w_DispatchIndirect(VT *c, ID3D11Buffer *a, UINT o) {
    unsigned t = fx_pre(c, M_DISPATCHINDIR, (ID3D11Resource *)a, NULL, 0, 0); r_DispatchIndirect(c, a, o); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_CopySubresourceRegion)(VT *, ID3D11Resource *, UINT, UINT, UINT, UINT, ID3D11Resource *, UINT, const D3D11_BOX *);
static void STDMETHODCALLTYPE w_CopySubresourceRegion(VT *c, ID3D11Resource *d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource *s, UINT ss, const D3D11_BOX *b) {
    unsigned t = fx_pre(c, M_COPYREGION, d, NULL, 0, 0); r_CopySubresourceRegion(c, d, ds, x, y, z, s, ss, b); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_CopyResource)(VT *, ID3D11Resource *, ID3D11Resource *);
static void STDMETHODCALLTYPE w_CopyResource(VT *c, ID3D11Resource *d, ID3D11Resource *s) {
    unsigned t = fx_pre(c, M_COPY, d, NULL, 0, 0); r_CopyResource(c, d, s); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_UpdateSubresource)(VT *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT);
static void STDMETHODCALLTYPE w_UpdateSubresource(VT *c, ID3D11Resource *d, UINT ds, const D3D11_BOX *b, const void *p, UINT rp, UINT dp) {
    unsigned t = fx_pre(c, M_UPDATE, d, NULL, 0, 0); r_UpdateSubresource(c, d, ds, b, p, rp, dp); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_ClearRenderTargetView)(VT *, ID3D11RenderTargetView *, const FLOAT[4]);
static void STDMETHODCALLTYPE w_ClearRenderTargetView(VT *c, ID3D11RenderTargetView *v, const FLOAT col[4]) {
    unsigned t = fx_pre(c, M_CLEARRTV, NULL, (ID3D11View *)v, 0, 0); r_ClearRenderTargetView(c, v, col); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_ClearDepthStencilView)(VT *, ID3D11DepthStencilView *, UINT, FLOAT, UINT8);
static void STDMETHODCALLTYPE w_ClearDepthStencilView(VT *c, ID3D11DepthStencilView *v, UINT f, FLOAT dep, UINT8 st) {
    unsigned t = fx_pre(c, M_CLEARDSV, NULL, (ID3D11View *)v, 0, 0); r_ClearDepthStencilView(c, v, f, dep, st); fx_post(t);
}
static void(STDMETHODCALLTYPE *r_GenerateMips)(VT *, ID3D11ShaderResourceView *);
static void STDMETHODCALLTYPE w_GenerateMips(VT *c, ID3D11ShaderResourceView *v) {
    unsigned t = fx_pre(c, M_GENMIPS, NULL, (ID3D11View *)v, 0, 0); r_GenerateMips(c, v); fx_post(t);
}
#undef VT

// The runtime rewrites entries of the context's vtable as its state changes (seen in gputime_test: a slot patched at
// device creation held a different function a few hundred calls later), so the wrappers go in at the start of the
// effects update, taking whatever each slot holds then as the original, and come out at its end. An entry the runtime
// rewrites inside the window just stops being counted; nothing ever calls a stale original.
static int g_fx_patched;
static void fx_patch(int on) {
    if (!g_ctx || (on == g_fx_patched && on != 2)) return;  // 2: put back any wrapper the runtime replaced meanwhile
    if (on == 2) on = 1;
    void **vt = *(void ***)g_ctx;
    static const struct { size_t off; void **orig; void *wrap; } H[] = {
#define FXH(m) {offsetof(ID3D11DeviceContextVtbl, m), (void **)&r_##m, (void *)w_##m}
        FXH(DrawIndexed), FXH(Draw), FXH(Map), FXH(Unmap), FXH(DrawIndexedInstanced), FXH(DrawInstanced), FXH(DrawAuto),
        FXH(DrawIndexedInstancedIndirect), FXH(DrawInstancedIndirect), FXH(Dispatch), FXH(DispatchIndirect),
        FXH(CopySubresourceRegion), FXH(CopyResource), FXH(UpdateSubresource), FXH(ClearRenderTargetView),
        FXH(ClearDepthStencilView), FXH(GenerateMips),
#undef FXH
    };
    DWORD old;
    if (!VirtualProtect(vt, sizeof(ID3D11DeviceContextVtbl), PAGE_READWRITE, &old)) return;
    for (size_t i = 0; i < sizeof H / sizeof H[0]; i++) {
        void **slot = &vt[H[i].off / sizeof(void *)];
        if (on) {
            if (*slot != H[i].wrap) { *H[i].orig = *slot; *slot = H[i].wrap; }
        } else if (*slot == H[i].wrap)
            *slot = *H[i].orig;
    }
    VirtualProtect(vt, sizeof(ID3D11DeviceContextVtbl), old, &old);
    g_fx_patched = on;
}

// Start of each frame on the main thread (heapfix.c frame_checks): close the frame, read an old one, open the next.
__attribute__((force_align_arg_pointer)) static void gt_frame(void) {
    if (!g_ctx) return;
    unsigned s = g_slot;
    if (g_open) {
        ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g_disj[s]);
        g_live[s] = 1;
    }
    s = g_slot = (s + 1) % GT_SLOTS;
    if (g_live[s]) collect(s);
    g_live[s] = 0;
    g_nq[s] = g_nrec[s] = 0;
    g_sp = 0;  // a frame left open by an exception unwinding past a hook
    g_scene_n = 0;
    g_frame_no++;
    g_fx_depth = 0;
    fx_patch(0);  // a window left open by an exception
    g_fx_frames++;
    ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)g_disj[s]);
    g_open = 1;
}

__attribute__((force_align_arg_pointer)) static void gt_enter(unsigned id, const void *obj) {
    if (id == g_pt_fx_id && !g_fx_depth++ && GetCurrentThreadId() == g_pt_main) fx_patch(1);
    if (g_sp >= GT_DEPTH) { g_sp++; return; }
    unsigned b = ~0u;
    if (g_open && g_nq[g_slot] < GT_Q) {
        b = g_nq[g_slot]++;
        ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g_ts[g_slot][b]);
    } else if (g_open)
        g_full++;
    if (id == g_pt_scene_id) g_stk[g_sp].key = GT_SCENE_KEY + (g_scene_n < GT_SCENES ? g_scene_n++ : GT_SCENES - 1);
    else g_stk[g_sp].key = id == g_pt_view_id && obj ? *(const unsigned *)obj : id;  // stages by vtable
    g_stk[g_sp].b = b;
    g_sp++;
}

__attribute__((force_align_arg_pointer)) static void gt_exit(unsigned id) {
    if (id == g_pt_fx_id && g_fx_depth && !--g_fx_depth) fx_patch(0);
    if (!g_sp) return;
    if (--g_sp >= GT_DEPTH) return;
    unsigned key = g_stk[g_sp].key;
    if (g_ctx && key - GT_SCENE_KEY < GT_SCENES && !(g_frame_no & 63)) read_targets(key - GT_SCENE_KEY);
    unsigned s = g_slot, b = g_stk[g_sp].b;
    if (b == ~0u || !g_open) return;
    if (g_nq[s] >= GT_Q || g_nrec[s] >= GT_Q / 2) { g_full++; return; }
    unsigned e = g_nq[s]++;
    ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g_ts[s][e]);
    g_rec[s][g_nrec[s]].key = g_stk[g_sp].key;
    g_rec[s][g_nrec[s]].b = b;
    g_rec[s][g_nrec[s]].e = e;
    g_nrec[s]++;
}

static void gt_report(void (*log)(const char *fmt, ...)) {
    if (!g_dev) { log("gpu time: no D3D11 device seen yet"); return; }
    log("gpu time (%u frames measured, %u late, %u disjoint, %u over the query limit; device %u, created on thread %lu): "
        "GPU ms per frame between each function's entry and exit, inclusive", g_frames, g_late, g_disjoint, g_full,
        g_devices, g_create_tid);
    for (int k = 0; k < 70 && g_frames; k++) {
        unsigned best = GT_KEYS;
        for (unsigned i = 0; i < g_nkeys; i++)
            if (g_n[i] && (best == GT_KEYS || g_ms[i] > g_ms[best])) best = i;
        if (best == GT_KEYS) break;
        char name[64], tgt[160] = "";
        unsigned k = g_key[best];
        if (k - GT_SCENE_KEY < GT_SCENES) {
            unsigned n = k - GT_SCENE_KEY;
            wsprintfA(name, "    scene renderer call %u", n);
            if (g_tgt[n].seen)
                wsprintfA(tgt, "  depth %ux%u fmt %u x%u, colour %ux%u fmt %u, viewport %d,%d %dx%d", g_tgt[n].dw, g_tgt[n].dh,
                          g_tgt[n].dfmt, g_tgt[n].darr, g_tgt[n].cw, g_tgt[n].ch, g_tgt[n].cfmt, (int)g_tgt[n].vx,
                          (int)g_tgt[n].vy, (int)g_tgt[n].vw, (int)g_tgt[n].vh);
        } else if (k - FX_KEY < FX_N) {
            const fx_sig *f = &g_fx[k - FX_KEY].s;
            unsigned long long frames = g_fx_frames ? g_fx_frames : 1;
            wsprintfA(name, "    fx %s", FX_NAME[f->method]);
            wsprintfA(tgt, "  [%u.%02u calls/frame, amount %u/frame] res dim %u %ux%ux%u fmt %u usage %u bind 0x%X map %u",
                      (unsigned)(g_fx[k - FX_KEY].calls / frames), (unsigned)(g_fx[k - FX_KEY].calls * 100 / frames % 100),
                      (unsigned)(g_fx[k - FX_KEY].amount / frames), f->dim, f->w, f->h, f->d, f->fmt, f->usage, f->bind, f->extra);
        } else if (k < 0x10000) lstrcpynA(name, pt_name(k), sizeof name);
        else wsprintfA(name, "  stage vtable +0x%X", k - g_pt_base);
        log("  %-40s %7.3f ms  %5.1f calls%s", name, g_ms[best] / g_frames, (double)g_n[best] / g_frames, tgt);
        g_n[best] = 0;
    }
    g_nkeys = g_frames = g_late = g_disjoint = g_full = 0;
    memset(g_tgt, 0, sizeof g_tgt);
    for (unsigned e = 0; e < g_nfx; e++) g_fx[e].calls = g_fx[e].amount = 0;  // entries stay: in-flight frames still name them
    g_fx_frames = 0;
}

typedef HRESULT(WINAPI *create_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                   ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
static create_fn r_create;

static HRESULT WINAPI gt_create(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL *fl,
                                UINT nfl, UINT sdk, ID3D11Device **dev, D3D_FEATURE_LEVEL *got, ID3D11DeviceContext **ctx) {
    HRESULT hr = r_create(a, t, sw, flags, fl, nfl, sdk, dev, got, ctx);
    if (SUCCEEDED(hr) && dev && *dev) {
        ID3D11DeviceContext *c = ctx ? *ctx : NULL;
        if (!c) ID3D11Device_GetImmediateContext(*dev, &c);
        else ID3D11DeviceContext_AddRef(c);
        if (c) {
            gt_attach(*dev, c);
            ID3D11DeviceContext_Release(c);
        }
    }
    return hr;
}

static FARPROC(WINAPI *r_gpa)(HMODULE, LPCSTR);
static FARPROC WINAPI gt_gpa(HMODULE m, LPCSTR name) {
    FARPROC f = r_gpa(m, name);
    if (f && (UINT_PTR)name > 0xFFFF && !strcmp(name, "D3D11CreateDevice")) {
        r_create = (create_fn)(void *)f;
        return (FARPROC)(void *)gt_create;
    }
    return f;
}

static void *gt_iat_hook(HMODULE m, const char *name, void *to) {
    unsigned char *b = (unsigned char *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd->VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(b + d->Name), "KERNEL32.dll") != 0 || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(b + d->OriginalFirstThunk), *iat = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(b + names->u1.AddressOfData))->Name, name) != 0) continue;
            void *old = (void *)iat->u1.Function;
            DWORD prot;
            if (!old || !VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &prot)) return NULL;
            iat->u1.Function = (DWORD_PTR)to;
            VirtualProtect(&iat->u1.Function, sizeof(void *), prot, &prot);
            return old;
        }
    }
    return NULL;
}

void *gt_iat_hook_any(HMODULE m, const char *name, void *to) { return gt_iat_hook(m, name, to); }   // bridgeprobe.c

// Called by install_passtime in "gpu" mode, before the game creates its device.
int install_gputime(HMODULE m, void (*log)(const char *fmt, ...)) {
    void *old = gt_iat_hook(m, "GetProcAddress", (void *)gt_gpa);
    if (!old) { log("gpu time: GetProcAddress import not found - not installed"); return 0; }
    r_gpa = (FARPROC(WINAPI *)(HMODULE, LPCSTR))old;
    g_gt_enter = gt_enter;
    g_gt_exit = gt_exit;
    g_gt_frame = gt_frame;
    g_gt_report = gt_report;
    log("gpu time installed: D3D11 timestamps around the render stages, reported with the frame budget");
    return 1;
}
