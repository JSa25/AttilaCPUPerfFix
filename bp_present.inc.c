// Part of bridgeprobe.c (included there in this order, not compiled on its own): Present: the bridge's frame end, the fallback after the bridge, the nested-Present guard, and the
// vtable slot helpers (patch_slot, patch_once, dxgi_own).
// ---- full-size screenshots for image comparisons (AGENT_LOG 182): shot_go.txt in the game folder -> the game's back buffer
// at this Present (after DLSS / FSR, before the F11 panel and frame generation) to shot_<n>.raw (u32 width, height, DXGI
// format, then the rows), and a log line with the settings it was taken with (shot_log, below the setting names)
static void shot_log(int n);
static void shot_take(ID3D11Resource *bb) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)bb, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
    ID3D11Texture2D *stg = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &stg))) return;
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)stg, bb);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
        wchar_t p[MAX_PATH];
        _snwprintf(p, MAX_PATH, L"%ls\\shot_%d.raw", g_dir, ++g_shots);
        FILE *f = _wfopen(p, L"wb");
        if (f) {
            unsigned hdr[3] = {d.Width, d.Height, d.Format};
            fwrite(hdr, 4, 3, f);
            for (UINT y = 0; y < d.Height; y++) fwrite((const unsigned char *)m.pData + (size_t)y * m.RowPitch, 4, d.Width, f);
            fclose(f);
            shot_log(g_shots);
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stg, 0);
    }
    ID3D11Texture2D_Release(stg);
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
        if (fgm_release) fgm_release();   // its view of the bridge's motion-vector texture and its own copies (round 2 R1)
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
        g_log("settings panel: %s over the frames the mod shows (DLSS off until the game restarts)", ok ? "available" : "NOT available");
    } else
        ID3D11Device_Release(d);
    if (ok) fgp_frame(bb, g_out_hwnd);
}

static void start_failed_panel(IDXGISwapChain *sc);
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
                if (g_shot_armed) { g_shot_armed = 0; shot_take(bb); }
                fgp_frame(bb, g_out_hwnd);
                fgb_present(bb);
                {   // the helper's fences stuck at all ones (its GPU device was lost): a new helper, 3 times a session at most
                    static int lost;
                    if (!g_switch && lost < 3 && fgb_fences_lost()) {
                        lost++;
                        g_log("frame bridge: the helper's GPU fences are stuck (its device was lost) - starting a new helper (%d of 3)", lost);
                        g_switch = 1;
                    } else if (!g_switch && lost >= 3 && fgb_fences_lost()) {
                        g_off_note = L"DLSS stopped: the GPU reset the add-on's helper several times. Restart the game.";
                        bridge_off();
                    }
                }
                if (g_switch) switch_now();
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
    if (g_start_failed && game_sc && !g_hid_sc) start_failed_panel(sc);
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
