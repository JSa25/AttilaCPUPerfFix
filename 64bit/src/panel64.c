// panel64: the F8 panel (Culling, What's new) over the game's frames, ported from heapfix/overlay.c. The panel itself
// is the x86 mod's own heapfix/fgpanel.c, compiled into this DLL unchanged; this file is what it needs around it.
// Called from d3d64.c's per-frame point (before each Present, on the drawing thread) with the swap chain: the panel is
// blended over the back buffer before it goes to the screen.
// Different from x86: the game reads its mouse through SDL here, not DirectInput, so the panel cannot hide clicks from
// the game while the cursor is over it (fgpanel.c's DirectInput part stays unused).
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "perffix64.h"

typedef struct { void (*sr)(int mode); void (*fg)(int frames); void (*motion)(int on); void (*cap)(int choice); void (*preset)(int choice); void (*sharp)(int choice); void (*view)(int choice); void (*vsync)(int on); void (*fps)(int on); void (*tech)(int sr, int fg); void (*cull)(int choice); } FgpApply;
int fgp_init(ID3D11Device *, ID3D11DeviceContext *, UINT bw, UINT bh, DXGI_FORMAT fmt, int sr, int fg, int motion, FgpApply apply);
int fgp_move_device(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT bw, UINT bh);
void fgp_frame(ID3D11Resource *backbuffer, HWND hwnd);
void fgp_set_cull_only(void);
void culling_pick(int c);   // perffix64.c

// what fgpanel.c asks of the upscaling add-on's bridge, when it is not built in (panel64_test): weak, fgbridge.c's win
#define FGB_WEAK __attribute__((weak))
FGB_WEAK UINT fgb_render_w(void) { return 0; }
FGB_WEAK UINT fgb_render_h(void) { return 0; }
FGB_WEAK unsigned fgb_fg_max(void) { return 0; }
FGB_WEAK void fgb_gpu_caps(int *dlss_sr, int *dlss_fg) { *dlss_sr = *dlss_fg = 0; }
FGB_WEAK const wchar_t *fgb_gpu_name(void) { return L""; }
// and of the cull, under its x86 names
void partcull_counts(unsigned *parts, unsigned *skipped) { unsigned f; partcull64_counts(parts, skipped, &f); }
unsigned partcull_forced(void) { unsigned p, s, f; partcull64_counts(&p, &s, &f); return f; }

static void (*p_log)(const char *fmt, ...);
static const GUID iid_tex2d = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};   // ID3D11Texture2D

void panel64_frame(void *swapchain) {
    IDXGISwapChain *sc = swapchain;
    static ID3D11Device *dev;   // the panel's device and context (it keeps a reference to each)
    static ID3D11DeviceContext *dctx;
    static int failed, depth;   // depth: a Present inside a Present (an overlay's window message) is drawn once
    ID3D11Texture2D *bb = NULL;
    DXGI_SWAP_CHAIN_DESC sd;
    if (failed || depth || FAILED(IDXGISwapChain_GetBuffer(sc, 0, &iid_tex2d, (void **)&bb))) return;
    depth = 1;
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
            if (p_log) p_log("F8 panel (Culling, What's new) %s over the game's frames", ok ? "ready" : "NOT available (back buffer format or GPU objects)");
        }
        if (ok || dev) {   // kept with its context; a device before this one is let go (the game closed it)
            if (dev) { ID3D11Device_Release(dev); if (dctx) ID3D11DeviceContext_Release(dctx); if (p_log) p_log("F8 panel: moved to the game's new device%s", ok ? "" : " - NOT available on it"); }
            dev = d; dctx = c; d = NULL;
        } else { if (c) ID3D11DeviceContext_Release(c); failed = 1; }
    }
    if (d) ID3D11Device_Release(d);
    if (dev && SUCCEEDED(IDXGISwapChain_GetDesc(sc, &sd))) fgp_frame((ID3D11Resource *)bb, sd.OutputWindow);
    ID3D11Texture2D_Release(bb);
    depth = 0;
}

// The main menu label (fgpanel.c: "Engine Fix <version>" / "by JGdaCaesar", top right) is drawn while this says the
// game's front end is up: UIDLL::FRONT_END_UI::s_ui at 0x5167298 in the game file's symbols, the same test Sol's
// loader uses for its own menu button (menu_button.c). On 32-bit menuver.c answers it.
extern int (*g_fgp_menu_label)(void);
static int menus_up64(void) { return *(void *volatile *)(uintptr_t)0x5167298 != NULL; }
void panel64_setup(void (*log)(const char *fmt, ...)) {
    p_log = log;
    MEMORY_BASIC_INFORMATION mi;   // only with the game mapped there (not in a test program)
    if (VirtualQuery((void *)(uintptr_t)0x5167298, &mi, sizeof mi) && mi.State == MEM_COMMIT) g_fgp_menu_label = menus_up64;
}
