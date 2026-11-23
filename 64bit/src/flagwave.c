// Flag Wave: real cloth banners for Total War: ATTILA, as its own plugin DLL (<game>\plugins\flagwave.dll, loaded by the
// engine fix's plugin loader at the game's start, before any of the game's code runs). Notes: notes/banners.md.
// None of the engine fix's code is needed; the cloth simulation and the shaders are shared with it
// (heapfix/flagsim.c, heapfix/flagvs.h).
//   - empire.retail.dll looks up D3D11CreateDevice and CreateDXGIFactory1 with GetProcAddress: its import of
//     GetProcAddress is hooked (on top of whatever hook is there already) to see the device and the swapchain.
//   - device: CreateVertexShader makes the game's two rigid prop shaders (main view 55ca645c, shadows eb3006be) from our
//     copies (identical for every model the game ships; flag cloth from the weight pack follows the simulation);
//     CreateBuffer finds the flag meshes' marker vertices.
//   - context: VSSetShader (one of ours set: the first time in a frame, the cloth is simulated; our results are lent to
//     VS slots t14 / t15 and the main view's wind block to the shadow shader, the game's own put back before any other
//     shader); VSSetConstantBuffers / VSSetShaderResources (the game setting those slots itself); DrawIndexedInstanced
//     (a flag mesh drawn with our shader: its instance records are gathered, a GPU copy).
//   - swapchain Present: counts frames.
// FPS tests: Ctrl+Shift+F in game switches the flags off / on (off = the game's own shaders, nothing of ours), flagwave_off.txt
// in the game folder starts them off; flagwave_ab.txt: on in even 10 s windows of UTC time, off in odd.
// Log: flagwave.log in the game folder.
#define COBJMACROS
#include <windows.h>
#include <stdint.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include "flagvs.h"
#include "flagsim.h"

static wchar_t g_dir[MAX_PATH], g_logp[MAX_PATH];
static HMODULE g_d3d11;

static void wlog(const char *fmt, ...) {
    char buf[2048];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(buf, sizeof buf, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    int k = vsnprintf(buf + n, sizeof buf - n - 2, fmt, a);
    va_end(a);
    if (k < 0) k = 0;
    n += k < (int)sizeof buf - n - 2 ? k : (int)sizeof buf - n - 3;
    buf[n++] = '\n';
    HANDLE f = CreateFileW(g_logp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, buf, (DWORD)n, &w, NULL);
    CloseHandle(f);
}

static void patch_slot(void **slot, void *to, void **old) {
    DWORD prot;
    if (*slot == to || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &prot)) return;
    *old = *slot;
    *slot = to;
    VirtualProtect(slot, sizeof *slot, prot, &prot);
}
static HMODULE owner_of(const void *p) {
    HMODULE m = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)p, &m);
    return m;
}
static unsigned long long fnv64(const void *p, size_t n) {
    const unsigned char *b = p;
    unsigned long long h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// ---- state
static ID3DBlob *g_blob[2];                       // our shaders: main view, shadow
#define FW_NVS 64   // copies of each shader remembered (2026-11-12: 4 lost most of 1212's main-view draws: no cloth, flicker)
static ID3D11VertexShader *g_vs[2][FW_NVS], *g_own[2][FW_NVS];
static volatile LONG g_made[2];
static int g_ab, g_cur = -1, g_wind_lent, g_fs_lent, g_fs_ok;
static volatile LONG g_off;   // dev switch: the flags off (Ctrl+Shift+F in game, flagwave_off.txt at start)
static ID3D11Buffer *g_wind, *g_wind_saved, *g_cam;
static ID3D11ShaderResourceView *g_srv_saved[2];
static volatile LONG g_presents;
static LONG g_last_gather = -1000;   // the frame (Present count) a flag was last gathered in
static LARGE_INTEGER g_qpc0, g_qpc_last, g_qf;
static unsigned long long g_ticks, g_draws, g_flagdraws, g_simmed, g_runs, g_rehooks;
static volatile LONG g_found;
// per frame (Present): our shaders set, our draw hook passed by a draw with them, flag draws gathered; and per 1800 frames
// how the frames went (2026-11-12: a recording gathered flags in 1 frame of 4)
static LONG g_f_set, g_f_dii, g_f_flag;
static unsigned long long g_fr_flag, g_fr_setnodraw, g_fr_drawnoflag, g_fr_none, g_fr_hooklost;

static HRESULT(STDMETHODCALLTYPE *r_cvs)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11VertexShader **);
static HRESULT(STDMETHODCALLTYPE *r_cbuf)(ID3D11Device *, const D3D11_BUFFER_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Buffer **);
static void(STDMETHODCALLTYPE *r_vsss)(ID3D11DeviceContext *, ID3D11VertexShader *, ID3D11ClassInstance *const *, UINT);
static void(STDMETHODCALLTYPE *r_vscb)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
static void(STDMETHODCALLTYPE *r_vssrv)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
static void(STDMETHODCALLTYPE *r_dii)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
static HRESULT(STDMETHODCALLTYPE *r_present)(IDXGISwapChain *, UINT, UINT);
static ID3D11DeviceContext *g_ctx;

static int kind_of(ID3D11VertexShader *vs) {
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < FW_NVS; i++)
            if (vs && g_vs[k][i] == vs) return k;
    return -1;
}
static int ab_on(void) {
    if (!g_ab) return 1;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int on = !((((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 100000000ULL) & 1);
    static int last = -1;
    if (on != last) wlog("A/B: flags %s", on ? "ON" : "off");
    last = on;
    return on;
}

// ---- device hooks
static void hook_context(ID3D11DeviceContext *c);
static HRESULT STDMETHODCALLTYPE w_cvs(ID3D11Device *d, const void *bc, SIZE_T len, ID3D11ClassLinkage *cl, ID3D11VertexShader **out) {
    unsigned long long h = fnv64(bc, len);
    int k = h == 0x55ca645c65c0fbe6ull ? 0 : h == 0xeb3006be08a96700ull ? 1 : -1;
    if (k < 0 || !g_blob[k]) return r_cvs(d, bc, len, cl, out);
    HRESULT hr = r_cvs(d, ID3D10Blob_GetBufferPointer(g_blob[k]), ID3D10Blob_GetBufferSize(g_blob[k]), cl, out);
    if (FAILED(hr)) { wlog("our %s shader refused (0x%08lx): the game's own", k ? "shadow" : "main view", (unsigned long)hr); return r_cvs(d, bc, len, cl, out); }
    LONG nth = InterlockedIncrement(&g_made[k]);
    if (nth == 1) wlog("%s shader made from ours", k ? "shadow" : "main view");
    if (out && *out) {
        g_vs[k][(nth - 1) % FW_NVS] = *out;
        if (nth == 5 || nth == FW_NVS + 1) wlog("%s shader made from ours %ld times%s", k ? "shadow" : "main view", nth, nth > FW_NVS ? " - more than remembered" : "");
        ID3D11VertexShader *own = NULL;   // the game's own too: drawn while the flags are off
        if (SUCCEEDED(r_cvs(d, bc, len, cl, &own))) g_own[k][(nth - 1) % FW_NVS] = own;
    }
    if (!g_ctx) {
        ID3D11Device_GetImmediateContext(d, &g_ctx);
        if (g_ctx) hook_context(g_ctx);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE w_cbuf(ID3D11Device *d, const D3D11_BUFFER_DESC *bd, const D3D11_SUBRESOURCE_DATA *sd, ID3D11Buffer **out) {
    HRESULT hr = r_cbuf(d, bd, sd, out);
    if (SUCCEEDED(hr) && out && *out && bd && sd && sd->pSysMem && (bd->BindFlags & D3D11_BIND_VERTEX_BUFFER)) {
        int f = fs_scan(*out, sd->pSysMem, bd->ByteWidth);
        if (f && InterlockedExchangeAdd(&g_found, f) == 0) wlog("first flag meshes found in a new vertex buffer (%d)", f);
    }
    return hr;
}
static void hook_device(ID3D11Device *d) {
    void **vt = *(void ***)d;
    patch_slot(&vt[offsetof(ID3D11DeviceVtbl, CreateVertexShader) / sizeof(void *)], (void *)w_cvs, (void **)&r_cvs);
    patch_slot(&vt[offsetof(ID3D11DeviceVtbl, CreateBuffer) / sizeof(void *)], (void *)w_cbuf, (void **)&r_cbuf);
}

// ---- context hooks
static void fs_unlend(void) {
    for (int i = 0; i < 2; i++) if (g_srv_saved[i]) ID3D11ShaderResourceView_Release(g_srv_saved[i]), g_srv_saved[i] = NULL;
    g_fs_lent = 0;
}
static void wind_unlend(void) {
    if (g_wind_saved) ID3D11Buffer_Release(g_wind_saved);
    g_wind_saved = NULL;
    g_wind_lent = 0;
}
static void sim_frame(ID3D11DeviceContext *c) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_fs_ok) {
        ID3D11Device *d = NULL;
        ID3D11DeviceContext_GetDevice(c, &d);
        char err[600] = "";
        g_fs_ok = d && fs_init(d, err, sizeof err) ? 1 : -1;
        if (d) ID3D11Device_Release(d);
        wlog("cloth simulation %s%s", g_fs_ok > 0 ? "set up" : "NOT set up: ", g_fs_ok > 0 ? "" : err);
        g_qpc0 = g_qpc_last = now;
    }
    if (g_fs_ok < 0) return;
    float dt = (float)(now.QuadPart - g_qpc_last.QuadPart) / (float)g_qf.QuadPart, t = (float)(now.QuadPart - g_qpc0.QuadPart) / (float)g_qf.QuadPart;
    g_qpc_last = now;
    g_simmed += fs_run(c, g_wind, g_cam, dt, t);
    static unsigned st[2], st_logged[2];   // GPU running totals: new cloths started, flags left out of the shader's lookup
    fs_stats(c, st);
    if (++g_runs % 1800 == 0) {
        wlog("%d flag meshes known; per frame %.0f prop draws checked, %.1f flag draws, %.1f flag instances simulated, draw hook put back %.1f times; CPU %.3f ms per frame; "
             "%.2f new cloths and %.2f flags left out of the lookup per frame; our shaders made %ld (main view) / %ld (shadow) times; "
             "frames: %llu with flags gathered, %llu with our shaders drawing but no flag, %llu with our shaders set but no draw seen, %llu without our shaders; draw hook not in place at %llu Presents",
             fs_models(), (double)g_draws / 1800, (double)g_flagdraws / 1800, (double)g_simmed / 1800, (double)g_rehooks / 1800, (double)g_ticks / (double)g_qf.QuadPart * 1000.0 / 1800,
             (double)(st[0] - st_logged[0]) / 1800, (double)(st[1] - st_logged[1]) / 1800, g_made[0], g_made[1], g_fr_flag, g_fr_drawnoflag, g_fr_setnodraw, g_fr_none, g_fr_hooklost);
        g_fr_flag = g_fr_drawnoflag = g_fr_setnodraw = g_fr_none = g_fr_hooklost = 0;
        st_logged[0] = st[0]; st_logged[1] = st[1];
        g_draws = g_flagdraws = g_simmed = g_ticks = g_rehooks = 0;
    }
}
static void STDMETHODCALLTYPE w_dii(ID3D11DeviceContext *c, UINT ic, UINT inst, UINT si, INT bv, UINT sinst);
static int hot_patch(void *fn);
static int hot_try(void **vt);
static int g_hot;
static void keep_draw_hook(ID3D11DeviceContext *c) {
    if (g_hot) { hot_patch((*(void ***)c)[offsetof(ID3D11DeviceContextVtbl, DrawIndexedInstanced) / sizeof(void *)]); return; }
    { static int tries; if (tries < 600 && (tries++ % 60) == 0 && hot_try(*(void ***)c)) { wlog("hot patch applied late"); return; } }
    // d3d11 swaps its own functions into some context slots at times: back on top of the new one (only d3d11's own; a
    // hook another mod put there calls ours as its original)
    void **slot = &(*(void ***)c)[offsetof(ID3D11DeviceContextVtbl, DrawIndexedInstanced) / sizeof(void *)];
    if (*slot == (void *)w_dii) return;
    HMODULE o = owner_of(*slot);
    static int told;
    if (!told++) {
        wchar_t name[MAX_PATH] = L"?";
        if (o) GetModuleFileNameW(o, name, MAX_PATH);
        wlog("draw slot replaced by %ls%s", name, o == g_d3d11 ? " (d3d11's own: hooked again)" : "");
    }
    if (o == g_d3d11) { patch_slot(slot, (void *)w_dii, (void **)&r_dii); g_rehooks++; }
}
static void STDMETHODCALLTYPE w_vsss(ID3D11DeviceContext *c, ID3D11VertexShader *vs, ID3D11ClassInstance *const *ci, UINT n) {
    int k = kind_of(vs);
    if (k >= 0 && (g_off || (g_ab && !ab_on()))) {   // flags off (dev switch, A/B off window): the game's own shader, nothing of ours
        for (int i = 0; i < FW_NVS; i++) if (g_vs[k][i] == vs && g_own[k][i]) { vs = g_own[k][i]; break; }
        k = -1;
    }
    if (g_cur == 0 && k != 0) {   // leaving the main view's prop shader: its wind block (cb2) is the one to keep
        ID3D11Buffer *b = NULL;
        ID3D11DeviceContext_VSGetConstantBuffers(c, 2, 1, &b);
        if (b && b != g_wind) { if (g_wind) ID3D11Buffer_Release(g_wind); g_wind = b; }
        else if (b) ID3D11Buffer_Release(b);
        b = NULL;   // and its camera block (cb0): flags far from the camera simulate less often
        ID3D11DeviceContext_VSGetConstantBuffers(c, 0, 1, &b);
        if (b && b != g_cam) { if (g_cam) ID3D11Buffer_Release(g_cam); g_cam = b; }
        else if (b) ID3D11Buffer_Release(b);
    }
    if (g_wind_lent && k != 1) { r_vscb(c, 2, 1, &g_wind_saved); wind_unlend(); }
    if (g_fs_lent && k < 0) { r_vssrv(c, 14, 2, g_srv_saved); fs_unlend(); }
    r_vsss(c, vs, ci, n);
    if (k >= 0) g_f_set++;
    if (k >= 0) keep_draw_hook(c);   // after the simulation: its compute work is one of the things that make d3d11 swap
    if (k == 1 && g_wind && !g_wind_lent) {   // the shadow pass binds no wind block: the main view's
        ID3D11DeviceContext_VSGetConstantBuffers(c, 2, 1, &g_wind_saved);
        r_vscb(c, 2, 1, &g_wind);
        g_wind_lent = 1;
    }
    if (k >= 0 && g_fs_ok > 0 && !g_fs_lent) {
        ID3D11ShaderResourceView *ours[2];
        fs_vs_views(ours);
        ID3D11DeviceContext_VSGetShaderResources(c, 14, 2, g_srv_saved);
        r_vssrv(c, 14, 2, ours);
        g_fs_lent = 1;
    }
    g_cur = k;
}
static void STDMETHODCALLTYPE w_vscb(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b) {
    if (g_wind_lent && start <= 2 && start + n > 2) wind_unlend();   // the game sets slot 2 itself: nothing to give back
    r_vscb(c, start, n, b);
    if (g_cur >= 0) keep_draw_hook(c);
}
static void STDMETHODCALLTYPE w_vssrv(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11ShaderResourceView *const *v) {
    if (g_fs_lent && start <= 15 && start + n > 14) fs_unlend();
    r_vssrv(c, start, n, v);
    if (g_cur >= 0) keep_draw_hook(c);
}
// a draw with one of our shaders set: a flag mesh's instances are gathered
static void fw_draw(ID3D11DeviceContext *c, UINT inst, INT bv, UINT sinst) {
    if (g_cur < 0 || g_fs_ok <= 0 || c != g_ctx) return;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    ID3D11Buffer *vb[2] = {0};
    UINT st[2] = {0}, of[2] = {0};
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 2, vb, st, of);
    const float *prm = vb[0] ? fs_is_flag(vb[0], of[0] + (UINT)bv * st[0]) : NULL;
    if (prm && vb[1] && st[1] == 112) { fs_pass = g_cur == 1; fs_gather(c, vb[1], of[1] + sinst * 112, inst, prm); g_flagdraws++; g_f_flag++; g_last_gather = g_presents; }
    g_draws++; g_f_dii++;
    if (vb[0]) ID3D11Buffer_Release(vb[0]);
    if (vb[1]) ID3D11Buffer_Release(vb[1]);
    QueryPerformanceCounter(&b);
    g_ticks += (unsigned long long)(b.QuadPart - a.QuadPart);
}
static void STDMETHODCALLTYPE w_dii(ID3D11DeviceContext *c, UINT ic, UINT inst, UINT si, INT bv, UINT sinst) {
    if (!g_hot) fw_draw(c, inst, bv, sinst);   // hot-patched later: the detour sees this draw, not twice
    r_dii(c, ic, inst, si, bv, sinst);
}
// d3d11's own DrawIndexedInstanced, hot-patched (2026-11-13): it starts "mov edi, edi" with 5 bytes of padding before it,
// Windows' hot-patch point. Every call into it comes here whoever holds the context's vtable slot - the engine fix
// re-hooks that slot at every stage with d3d11's function as its next step, which cut the vtable hook out in 5 frames of
// 6. Up to two d3d11 functions (d3d11 swaps its own in), each with its own detour; anything else: the vtable hook.
static void(STDMETHODCALLTYPE *r_hot[2])(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
static void *g_hot_fn[2];
static int hot_patch(void *fn);
// d3d11's DrawIndexedInstanced: from the slot when d3d11 holds it; when another mod's hook does (the engine fix hooks it
// before our plugin loads: 2026-11-13 the hot patch never applied in game), from that mod's fgb_d3d11_original export
static int hot_try(void **vt) {
    unsigned idx = offsetof(ID3D11DeviceContextVtbl, DrawIndexedInstanced) / sizeof(void *);
    void *fn = vt[idx];
    if (owner_of(fn) == g_d3d11) return hot_patch(fn);
    static int told;
    HMODULE o = owner_of(fn);
    void *(__cdecl *orig)(unsigned) = o ? (void *(__cdecl *)(unsigned))(void *)GetProcAddress(o, "fgb_d3d11_original") : NULL;
    void *d = orig ? orig(idx) : NULL;
    if (!told++) {
        wchar_t name[MAX_PATH] = L"?";
        if (o) GetModuleFileNameW(o, name, MAX_PATH);
        wlog("draw slot held by %ls at start; its fgb_d3d11_original: %s", name, d ? (owner_of(d) == g_d3d11 ? "d3d11's function" : "not d3d11's") : "none");
    }
    return d && hot_patch(d);
}
static void STDMETHODCALLTYPE w_hot0(ID3D11DeviceContext *c, UINT ic, UINT inst, UINT si, INT bv, UINT sinst) { fw_draw(c, inst, bv, sinst); r_hot[0](c, ic, inst, si, bv, sinst); }
static void STDMETHODCALLTYPE w_hot1(ID3D11DeviceContext *c, UINT ic, UINT inst, UINT si, INT bv, UINT sinst) { fw_draw(c, inst, bv, sinst); r_hot[1](c, ic, inst, si, bv, sinst); }
static int hot_patch(void *fn) {
    if (g_hot >= 2 || !fn || owner_of(fn) != g_d3d11) return 0;
    for (int i = 0; i < g_hot; i++) if (g_hot_fn[i] == fn) return 1;
    unsigned char *f = fn;
    for (int i = -5; i < 0; i++) if (f[i] != 0xCC && f[i] != 0x90) return 0;
    if (f[0] != 0x8B || f[1] != 0xFF) return 0;
    DWORD prot;
    if (!VirtualProtect(f - 5, 7, PAGE_EXECUTE_READWRITE, &prot)) return 0;
    void *det = g_hot ? (void *)w_hot1 : (void *)w_hot0;
    r_hot[g_hot] = (void *)(f + 2);   // past "mov edi, edi": the function as it was
    f[-5] = 0xE9;
    *(int *)(f - 4) = (int)((unsigned char *)det - f);
    *(volatile unsigned short *)f = 0xF9EB;   // jmp short -7 (one aligned 2-byte write)
    VirtualProtect(f - 5, 7, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), f - 5, 7);
    g_hot_fn[g_hot++] = fn;
    wlog("d3d11's DrawIndexedInstanced at d3d11+%#lx hot-patched", (unsigned long)((unsigned char *)fn - (unsigned char *)g_d3d11));
    return 1;
}
// the game sets the vertex buffers right before each prop draw: with one of our shaders set, the draw slot is checked
// here too (in game d3d11 put its own function back between our checks so often that 9 in 10 flag draws went unseen,
// 2026-11-12 Abbasid test: "the flags stutter")
static void(STDMETHODCALLTYPE *r_iavb)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *, const UINT *, const UINT *);
static void STDMETHODCALLTYPE w_iavb(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b, const UINT *st, const UINT *of) {
    r_iavb(c, start, n, b, st, of);
    if (g_cur >= 0) keep_draw_hook(c);
}

static void keep_draw_hook(ID3D11DeviceContext *c);
// d3d11 puts its own DrawIndexedInstanced back in the context's slot after many state changes (lazy validation; the engine
// fix re-hooks at every stage for the same reason). 2026-11-13 counters: our shaders set in 2,156 frames, our draw hook
// passed in 364. So after every state call while one of our shaders is set, the draw slot is checked and taken again
// (a pointer compare). Generic wrappers: every argument is 4 bytes on x86.
#define FW_WRAP1(m) static void(STDMETHODCALLTYPE *r_##m)(ID3D11DeviceContext *, uintptr_t); \
    static void STDMETHODCALLTYPE w_##m(ID3D11DeviceContext *c, uintptr_t a) { r_##m(c, a); if (g_cur >= 0) keep_draw_hook(c); }
#define FW_WRAP2(m) static void(STDMETHODCALLTYPE *r_##m)(ID3D11DeviceContext *, uintptr_t, uintptr_t); \
    static void STDMETHODCALLTYPE w_##m(ID3D11DeviceContext *c, uintptr_t a, uintptr_t b) { r_##m(c, a, b); if (g_cur >= 0) keep_draw_hook(c); }
#define FW_WRAP3(m) static void(STDMETHODCALLTYPE *r_##m)(ID3D11DeviceContext *, uintptr_t, uintptr_t, uintptr_t); \
    static void STDMETHODCALLTYPE w_##m(ID3D11DeviceContext *c, uintptr_t a, uintptr_t b, uintptr_t d) { r_##m(c, a, b, d); if (g_cur >= 0) keep_draw_hook(c); }
#define FW_WRAP6(m) static void(STDMETHODCALLTYPE *r_##m)(ID3D11DeviceContext *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t); \
    static void STDMETHODCALLTYPE w_##m(ID3D11DeviceContext *c, uintptr_t a, uintptr_t b, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g) { r_##m(c, a, b, d, e, f, g); if (g_cur >= 0) keep_draw_hook(c); }
FW_WRAP3(IASetIndexBuffer) FW_WRAP1(IASetInputLayout) FW_WRAP1(IASetPrimitiveTopology)
FW_WRAP3(PSSetShader) FW_WRAP3(PSSetShaderResources) FW_WRAP3(PSSetConstantBuffers) FW_WRAP3(PSSetSamplers)
FW_WRAP3(VSSetSamplers) FW_WRAP3(GSSetShader) FW_WRAP3(OMSetBlendState) FW_WRAP2(OMSetDepthStencilState)
FW_WRAP1(RSSetState) FW_WRAP3(OMSetRenderTargets) FW_WRAP2(RSSetViewports) FW_WRAP6(UpdateSubresource) FW_WRAP2(Unmap)
static void hook_state_calls(void **vt) {
#define FW_HOOK(m) patch_slot(&vt[offsetof(ID3D11DeviceContextVtbl, m) / sizeof(void *)], (void *)w_##m, (void **)&r_##m)
    FW_HOOK(IASetIndexBuffer); FW_HOOK(IASetInputLayout); FW_HOOK(IASetPrimitiveTopology); FW_HOOK(PSSetShader);
    FW_HOOK(PSSetShaderResources); FW_HOOK(PSSetConstantBuffers); FW_HOOK(PSSetSamplers); FW_HOOK(VSSetSamplers);
    FW_HOOK(GSSetShader); FW_HOOK(OMSetBlendState); FW_HOOK(OMSetDepthStencilState); FW_HOOK(RSSetState);
    FW_HOOK(OMSetRenderTargets); FW_HOOK(RSSetViewports); FW_HOOK(UpdateSubresource); FW_HOOK(Unmap);
#undef FW_HOOK
}
static void hook_context(ID3D11DeviceContext *c) {
    void **vt = *(void ***)c;
    patch_slot(&vt[offsetof(ID3D11DeviceContextVtbl, IASetVertexBuffers) / sizeof(void *)], (void *)w_iavb, (void **)&r_iavb);
    patch_slot(&vt[offsetof(ID3D11DeviceContextVtbl, VSSetConstantBuffers) / sizeof(void *)], (void *)w_vscb, (void **)&r_vscb);
    patch_slot(&vt[offsetof(ID3D11DeviceContextVtbl, VSSetShaderResources) / sizeof(void *)], (void *)w_vssrv, (void **)&r_vssrv);
    if (!hot_try(vt))
        patch_slot(&vt[offsetof(ID3D11DeviceContextVtbl, DrawIndexedInstanced) / sizeof(void *)], (void *)w_dii, (void **)&r_dii);
    else r_dii = r_hot[0];
    if (!g_hot) hook_state_calls(vt);   // only for the vtable hook (the hot patch needs no help)
    if (r_vscb && r_vssrv && r_dii && r_iavb) patch_slot(&vt[offsetof(ID3D11DeviceContextVtbl, VSSetShader) / sizeof(void *)], (void *)w_vsss, (void **)&r_vsss);
    wlog("context hooked%s", r_vsss ? "" : " - NOT all slots");
}

// ---- swapchain: frames
// Ctrl+Shift+F while the game's window is in front: the flags off / on (dev switch for FPS tests), once per press
static void hotkey(void) {
    static int was;
    int down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('F') & 0x8000);
    if (down && !was) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId()) wlog("flags %s (Ctrl+Shift+F)", InterlockedXor(&g_off, 1) ? "ON" : "OFF");
    }
    was = down;
    // Ctrl+Shift+R: record the next 1200 simulated frames (~20 s) of what the game hands the cloth, for an offline replay
    static int wasr;
    int downr = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('R') & 0x8000);
    if (downr && !wasr && g_fs_ok > 0 && !fs_recording()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId()) {
            wchar_t p[MAX_PATH];
            _snwprintf(p, MAX_PATH, L"%ls\\flagwave_rec.bin", g_dir);
            wlog(fs_record_start(p, 1200) ? "recording 1200 frames to %ls (Ctrl+Shift+R)" : "could not start a recording to %ls", p);
        }
    }
    static int was_rec;
    if (was_rec && !fs_recording()) wlog("recording finished");
    was_rec = fs_recording();
}
// the cloth is simulated once a frame here, at the frame's end (all its flag draws gathered), right before Present:
// not in the middle of the game's drawing (it ran inside the first of our shaders being set until 2026-11-12: against
// plain 1212 the average fps was the same but the 1 % lows dropped 60 -> 53). Skipped with the flags off and when no flag
// was drawn for 12 frames (the campaign map, menus); the first time, it only sets the simulation up.
static void present_work(void) {
    if (!g_ctx || !(g_made[0] | g_made[1]) || g_off || (g_ab && !ab_on())) return;
    if (g_fs_ok != 0 && g_presents - g_last_gather > 12) return;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    if (g_fs_lent) { r_vssrv(g_ctx, 14, 2, g_srv_saved); fs_unlend(); }   // our views off the VS before they become UAVs
    sim_frame(g_ctx);
    QueryPerformanceCounter(&b);
    g_ticks += (unsigned long long)(b.QuadPart - a.QuadPart);
}
static HRESULT STDMETHODCALLTYPE w_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    hotkey();
    if (g_ctx) {
        void **slot = &(*(void ***)g_ctx)[offsetof(ID3D11DeviceContextVtbl, DrawIndexedInstanced) / sizeof(void *)];
        g_fr_hooklost += g_hot ? (*slot != g_hot_fn[0] && (g_hot < 2 || *slot != g_hot_fn[1]) && owner_of(*slot) == g_d3d11) : *slot != (void *)w_dii;
    }
    if (g_f_flag) g_fr_flag++; else if (g_f_dii) g_fr_drawnoflag++; else if (g_f_set) g_fr_setnodraw++; else g_fr_none++;
    g_f_set = g_f_dii = g_f_flag = 0;
    present_work();
    InterlockedIncrement(&g_presents);
    return r_present(sc, sync, flags);
}
static HRESULT(STDMETHODCALLTYPE *r_csc)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
static HRESULT STDMETHODCALLTYPE w_csc(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out) {
    HRESULT hr = r_csc(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out && !r_present) {   // all swapchains share the vtable: hooked once
        void **vt = *(void ***)*out;
        patch_slot(&vt[offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *)], (void *)w_present, (void **)&r_present);
        wlog("swapchain Present hooked");
    }
    return hr;
}

// ---- the game's lookups
typedef HRESULT(WINAPI *create_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
                                   D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *factory_fn)(REFIID, void **);
static create_fn r_create;
static factory_fn r_factory;
static HRESULT WINAPI w_create(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE sw, UINT fl, const D3D_FEATURE_LEVEL *lv, UINT nlv, UINT sdk,
                               ID3D11Device **dev, D3D_FEATURE_LEVEL *got, ID3D11DeviceContext **ctx) {
    HRESULT hr = r_create(a, t, sw, fl, lv, nlv, sdk, dev, got, ctx);
    if (SUCCEEDED(hr) && dev && *dev) {
        if (!g_d3d11) g_d3d11 = GetModuleHandleW(L"d3d11.dll");
        hook_device(*dev);
    }
    return hr;
}
static HRESULT WINAPI w_factory(REFIID iid, void **out) {
    HRESULT hr = r_factory(iid, out);
    if (SUCCEEDED(hr) && out && *out && !r_csc) {
        void **vt = *(void ***)*out;
        patch_slot(&vt[offsetof(IDXGIFactoryVtbl, CreateSwapChain) / sizeof(void *)], (void *)w_csc, (void **)&r_csc);
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
// the import slot of kernel32's GetProcAddress in a module (by name, as any hook already in it keeps the slot)
static void **iat_slot(HMODULE m, const char *fn) {
    unsigned char *b = (unsigned char *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(b + d->Name), "kernel32.dll")) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (!strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(b + names->u1.AddressOfData))->Name, fn)) return (void **)&slots->u1.Function;
        }
    }
    return NULL;
}

// compiles our shaders; the hooks go in only if they compile (no change to the game otherwise)
static int setup(void) {
    pD3DCompile comp = (pD3DCompile)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    static const char *const entry[2] = {"main_vs", "shadow_vs"};
    for (int k = 0; k < 2; k++) {
        ID3DBlob *err = NULL;
        if (!comp || FAILED(comp(flag_hlsl, sizeof flag_hlsl - 1, "flagvs", NULL, NULL, entry[k], "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g_blob[k], &err))) {
            wlog("%s NOT compiled: %s - off", entry[k], err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "no d3dcompiler_47");
            g_blob[0] = g_blob[1] = NULL;
            return 0;
        }
    }
    return 1;
}

// for tests (flagwave_test.c): hook a device made outside the game, and count a frame
__declspec(dllexport) int fw_test_hook(ID3D11Device *d) {
    if (!g_blob[0] && !setup()) return 0;
    if (!g_d3d11) g_d3d11 = GetModuleHandleW(L"d3d11.dll");
    hook_device(d);
    return 1;
}
__declspec(dllexport) int fw_test_hot(void) { return g_hot; }
__declspec(dllexport) void fw_test_present(void) { present_work(); InterlockedIncrement(&g_presents); }
__declspec(dllexport) void fw_test_off(int off) { g_off = off; }
__declspec(dllexport) void fw_test_stats(unsigned *made, unsigned *meshes, unsigned *flagdraws, unsigned *simmed) {
    *made = (unsigned)(g_made[0] + g_made[1]); *meshes = (unsigned)fs_models(); *flagdraws = (unsigned)g_flagdraws; *simmed = (unsigned)g_simmed;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls(h);
    QueryPerformanceFrequency(&g_qf);
    // the game folder: the plugin sits in <game>\plugins
    GetModuleFileNameW(h, g_dir, MAX_PATH);
    for (int up = 0; up < 2; up++) { wchar_t *s = wcsrchr(g_dir, L'\\'); if (s) *s = 0; }
    _snwprintf(g_logp, MAX_PATH, L"%ls\\flagwave.log", g_dir);
    HMODULE game = GetModuleHandleW(L"empire.retail.dll");
    if (!game) return TRUE;   // a test process: fw_test_hook does the rest
    DeleteFileW(g_logp);
    wlog("Flag Wave plugin (cloth banners), %ls", g_dir);
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\flagwave_off.txt", g_dir);
    g_off = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    wlog("flags %s at start; Ctrl+Shift+F in game switches them off / on (flagwave_off.txt: start off)", g_off ? "OFF" : "on");
    _snwprintf(p, MAX_PATH, L"%ls\\flagwave_ab.txt", g_dir);
    g_ab = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    if (g_ab) wlog("A/B: flags on in even 10 s windows of UTC time, off in odd (flagwave_ab.txt)");
    if (!setup()) return TRUE;
    void **slot = iat_slot(game, "GetProcAddress");
    if (!slot) { wlog("empire.retail.dll's GetProcAddress import not found - off"); return TRUE; }
    patch_slot(slot, (void *)w_gpa, (void **)&r_gpa);
    wlog("on: waiting for the game's device");
    return TRUE;
}
