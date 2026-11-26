// The F8 panel and the pop-ups without the upscaling add-on (with it, the add-on's bridge draws them: bridgeprobe.c).
// The game's swapchain Present is wrapped, found the way the bridge finds it (the game's GetProcAddress for
// CreateDXGIFactory1, then the factory's CreateSwapChain), and the panel (fgpanel.c) is blended over the game's frame
// before it goes to the screen. F8 opens Culling directly here: there is no upscaling page.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stddef.h>
#include "gamerva.h"

#include "fgpanel.h"
int fgp_init(ID3D11Device *, ID3D11DeviceContext *, UINT bw, UINT bh, DXGI_FORMAT fmt, int sr, int fg, int motion, FgpApply apply);
int fgp_move_device(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT bw, UINT bh);
void fgp_frame(ID3D11Resource *backbuffer, HWND hwnd);
void fgp_set_cull_only(void), fgp_di8_created(void *di);   // fgpanel.c
void culling_pick(int c);                                  // heapfix.c
extern void (*g_dev_di8_created)(void *di);                // heapfix.c: the panel hides the game's mouse over it

static void (*o_log)(const char *fmt, ...);
static HRESULT(STDMETHODCALLTYPE *r_present)(IDXGISwapChain *, UINT, UINT);
static HRESULT(STDMETHODCALLTYPE *r_create_sc)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
static HRESULT(WINAPI *r_factory)(REFIID, void **);
static FARPROC(WINAPI *r_gpa)(HMODULE, LPCSTR);
static const GUID iid_tex2d = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};   // ID3D11Texture2D

// once per slot (the swapchains share their table): another mod hooking it later calls ours as its original, and ours
// hooking again on top of it would make the two call each other forever (bridgeprobe.c patch_once)
static void patch_once(void **slot, void *to, void **old) {
    DWORD prot;
    if (*old || *slot == to || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &prot)) return;
    *old = *slot;
    *slot = to;
    VirtualProtect(slot, sizeof *slot, prot, &prot);
}

static void draw(IDXGISwapChain *sc) {
    static ID3D11Device *dev;   // the panel's device (it keeps a reference, as the bridge's panel does)
    static int failed;
    ID3D11Texture2D *bb = NULL;
    DXGI_SWAP_CHAIN_DESC sd;
    if (failed || FAILED(IDXGISwapChain_GetBuffer(sc, 0, &iid_tex2d, (void **)&bb))) return;
    ID3D11Device *d = NULL;
    ID3D11Texture2D_GetDevice(bb, &d);
    if (d && d != dev && SUCCEEDED(IDXGISwapChain_GetDesc(sc, &sd))) {   // the first frame, or a new device
        ID3D11DeviceContext *c = NULL;
        D3D11_TEXTURE2D_DESC td;
        ID3D11Device_GetImmediateContext(d, &c);
        ID3D11Texture2D_GetDesc(bb, &td);
        FgpApply ap = {0};
        ap.cull = culling_pick;
        int ok = dev ? fgp_move_device(d, c, td.Width, td.Height) : fgp_init(d, c, td.Width, td.Height, td.Format, 0, 0, -1, ap);
        if (!dev) {
            if (ok) fgp_set_cull_only();
            o_log("F8 panel (Culling, What's new) %s over the game's frames", ok ? "ready" : "NOT available (back buffer format or GPU objects)");
        }
        if (ok) { dev = d; d = NULL; }   // kept with its context
        else { if (c) ID3D11DeviceContext_Release(c); failed = !dev; }
    }
    if (d) ID3D11Device_Release(d);
    if (dev && SUCCEEDED(IDXGISwapChain_GetDesc(sc, &sd))) fgp_frame((ID3D11Resource *)bb, sd.OutputWindow);
    ID3D11Texture2D_Release(bb);
}

// A Present of the swapchain this thread is already presenting is skipped. A player's crash dumps (7.5, 2026-11-26):
// ours -> the Steam overlay -> DXGI's Present -> back into ours for the same swapchain, thousands deep until the stack
// overflowed. Passing it on only goes round again; returning ends the loop wherever it comes from. Another swapchain's
// Present inside ours still goes through (not drawn on).
static HRESULT STDMETHODCALLTYPE w_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    static __thread IDXGISwapChain *presenting;
    static LONG skipped;
    if (sc == presenting) {
        if (InterlockedIncrement(&skipped) == 1)
            o_log("F8 panel: the game's Present was called again inside itself (another program's hook) - skipped (only logged once)");
        return S_OK;
    }
    IDXGISwapChain *outer = presenting;
    presenting = sc;
    if (!outer && !(flags & DXGI_PRESENT_TEST)) draw(sc);
    HRESULT hr = r_present(sc, sync, flags);
    presenting = outer;
    return hr;
}
static HRESULT STDMETHODCALLTYPE w_create_sc(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out) {
    HRESULT hr = r_create_sc(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out)
        patch_once(&(*(void ***)*out)[offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *)], (void *)w_present, (void **)&r_present);
    return hr;
}
static HRESULT WINAPI w_factory(REFIID r, void **out) {
    HRESULT hr = r_factory(r, out);
    if (SUCCEEDED(hr) && out && *out)
        patch_once(&(*(void ***)*out)[offsetof(IDXGIFactoryVtbl, CreateSwapChain) / sizeof(void *)], (void *)w_create_sc, (void **)&r_create_sc);
    return hr;
}
static FARPROC WINAPI w_gpa(HMODULE m, LPCSTR name) {
    FARPROC f = r_gpa(m, name);
    if (f && (UINT_PTR)name > 0xFFFF && !strcmp(name, "CreateDXGIFactory1") && !r_factory) {
        r_factory = (HRESULT(WINAPI *)(REFIID, void **))(void *)f;
        return (FARPROC)(void *)w_factory;
    }
    return f;
}

int install_overlay(HMODULE m, void (*log)(const char *fmt, ...)) {
    void *old = iat_hook(m, "GetProcAddress", (void *)w_gpa);
    if (!old) { log("F8 panel: the game's GetProcAddress import not found - no panel without the upscaling add-on"); return 0; }
    r_gpa = (FARPROC(WINAPI *)(HMODULE, LPCSTR))old;
    o_log = log;
    g_dev_di8_created = fgp_di8_created;
    return 1;
}
