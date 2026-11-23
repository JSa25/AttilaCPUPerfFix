// Part of bridgeprobe.c (included there in this order, not compiled on its own): the game's swapchain: made on a hidden window while the bridge runs (w_create_sc), fullscreen and
// window answers for it, the device / factory hooks behind the game's GetProcAddress, the ReShade link.
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
        if (g_live_state != 1) {   // bridge off: no per-stage draw hooks either, and its textures go (round 2 R2)
            g_motion = 0; g_start_failed = 1;
            if (fgm_release) fgm_release();
            if (fgb_release_gpu) fgb_release_gpu();
        }
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
            if (g_sharp_auto && g_sharp == 2 && fgb_sr_backend() == 2) {
                g_sharp = 3;
                g_log("DLSS add-on: sharpness high, the default with AMD FSR upscaling");
                settings_save();
            }
            g_sharp_auto = 0;
            fgb_set_sharpness(SHARP_CON[g_sharp]);
            fgb_set_vsync(g_vsync);
            if (!g_vsync && !fgb_helper_has_vsync()) g_log("DLSS add-on: V-Sync off needs add-on version 7 - its helper keeps V-Sync on");
            if (fgb_driver_vsync() >= 2)   // the driver's choice wins over the panel's (AGENT_LOG 157)
                g_log("DLSS add-on: V-Sync is %s in NVIDIA's settings (Control Panel / app, global profile) - the panel's V-Sync choice has no effect",
                      fgb_driver_vsync() == 2 ? "forced on" : fgb_driver_vsync() == 3 ? "forced off" : "Fast Sync");
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
            if (!panel_setup(g_dev, g_ctx, g_live_w, g_live_h, d->BufferDesc.Format, g_motion ? g_motion_on : no_fg ? -2 : -1))
                g_log("frame bridge: settings panel not available (back buffer format %u)", d->BufferDesc.Format);
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
