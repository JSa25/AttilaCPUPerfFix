// Part of bridgeprobe.c (included there in this order, not compiled on its own): the add-on's settings: settings.txt, what a pick in the panel does, the frame cap, a new helper
// (switch_now), the dev sweep, the panel's set-up.
// the F11 panel's switches (take effect from the next frame)
// render size per SR mode: off and DLAA native, then NVIDIA's preset scales (Quality 2/3, Balanced 0.58, Performance 1/2,
// Ultra Performance 1/3), rounded down to even
// the game's depth buffer cannot be copied (its Anti-Aliasing on MSAA): nothing upscales the render, so rendering below the
// screen size would leave the scene shrunk in a corner for the whole session (ultrareview 2026-11-02); full size from then on
static void sr_set(int mode) {
    if (g_depth_bad) mode = 0;
    static const float scale[6] = {1, 1, 2.0f / 3, 0.58f, 0.5f, 1.0f / 3};
    float k = scale[mode >= 0 && mode < 6 ? mode : 0];
    g_sr = mode;
    g_rw = mode > 1 ? (UINT)(g_live_w * k) & ~1u : g_live_w;
    g_rh = mode > 1 ? (UINT)(g_live_h * k) & ~1u : g_live_h;
    fgb_set_sr(mode != 0, g_rw, g_rh);
}
// the add-on's settings.txt: one key=value per line, written back whenever the F11 panel changes something
static const char *const SR_KEYS[6] = {"off", "dlaa", "quality", "balanced", "performance", "ultra_performance"};
static const char *const FG_KEYS[4] = {"off", "2x", "3x", "4x"};
// the frame cap: off, auto (default), or a fixed rendered-frame cap
static const char *const CAP_KEYS[5] = {"off", "auto", "60", "90", "120"};
static const unsigned CAP_FPS[5] = {0, 0, 60, 90, 120};
static int g_fcap = 1;
// the DLSS preset (NVIDIA's model, Rome 2 DLSS_LOG 38): default (NVIDIA's pick per mode: K for DLAA / Quality / Balanced,
// M Performance, L Ultra Performance) or one for every mode; handed to the helper as NVIDIA's preset number
static const char *const PRESET_KEYS[5] = {"default", "j", "k", "l", "m"};
static const unsigned PRESET_NGX[5] = {0, 10, 11, 12, 13};
static int g_preset;
// sharpening of the DLSS output (fgbridge.c: AMD's RCAS): off, or RCAS's con at 1.25 / 0.6 / 0.2 stops (AMD's default
// 0.2). Attila in game (AGENT_LOG 141, 4K battle, detail at half size against native): sharpness off / low / medium / high
// 84 / 90 / 96 / 106 % at DLAA, low / medium 90 / 97 % at Quality and Balanced: medium is the default (Rome 2: low)
static const char *const SHARP_KEYS[4] = {"off", "low", "medium", "high"};
static const float SHARP_CON[4] = {0, 0.42f, 0.66f, 0.87f};
static int g_sharp = 2;
static int g_sharp_auto;   // sharpness still the default (or Medium from before version=5): High once AMD FSR is known to upscale
static void shot_log(int n) {
    unsigned fgb_sr_backend(void);
    const char *tech = !g_sr ? "-" : fgb_sr_backend() == 2 ? "AMD FSR" : "NVIDIA DLSS";
    g_log("screenshot %d: shot_%d.raw - super resolution %s (%s), render %ux%u, preset %s, sharpness %s, frame generation %s", n, n,
          SR_KEYS[g_sr >= 0 && g_sr < 6 ? g_sr : 0], tech, g_rw, g_rh, PRESET_KEYS[g_preset >= 0 && g_preset < 5 ? g_preset : 0],
          SHARP_KEYS[g_sharp >= 0 && g_sharp < 4 ? g_sharp : 0], g_fg ? "on" : "off");
}
// V-Sync of the add-on's frames (6.9.2, players with fps held at their screen's refresh rate): on (default, no tearing) or off
static const char *const VSYNC_KEYS[2] = {"off", "on"};
static int g_vsync = 1;
// the frame-rate counter (F11 panel): rendered / shown frames per second in a corner; off by default
static const char *const FPSC_KEYS[5] = {"off", "top_left", "top_right", "bottom_left", "bottom_right"};
static int g_fpsc;
// the key that opens the panel (default F8 since 2026-11-02: Mat's 4K UI Scale 2.1.1+ holds F11 for its compare picture)
static const char *const KEY_KEYS[18] = {"f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
                                         "insert", "delete", "home", "end", "pause", "page_up"};
static const int KEY_VK[18] = {VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
                               VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PAUSE, VK_PRIOR};
static const wchar_t *const KEY_NAMES[18] = {L"F1", L"F2", L"F3", L"F4", L"F5", L"F6", L"F7", L"F8", L"F9", L"F10", L"F11", L"F12",
                                             L"Insert", L"Delete", L"Home", L"End", L"Pause", L"Page Up"};
static int g_pkey = 7;
// technology (FSR_PLAN F5): auto = NVIDIA DLSS / DLSS-G where the GPU has them, AMD FSR for the rest (GPUs that are not
// NVIDIA, frame generation on RTX 20 / 30); fsr = AMD FSR for both on any GPU (to compare)
// dlss = NVIDIA for both (as auto on an RTX 40 / 50), mixed = DLSS upscaling + AMD FSR frame generation (F11 panel)
static const char *const TECH_KEYS[4] = {"auto", "fsr", "dlss", "mixed"};
static int g_tech;
#define TECH_HELP "# technology: auto, dlss, mixed, fsr (auto: NVIDIA DLSS where your GPU has it, AMD FSR for the rest; mixed: DLSS upscaling + AMD FSR frame generation; fsr: AMD FSR for both)\n"
#define TECH_LINE "technology=%s\n"
#define TECH_ARG , TECH_KEYS[g_tech >= 0 && g_tech < 4 ? g_tech : 0]
static void settings_save(void) {
    if (!g_settings[0]) return;
    FILE *f = _wfopen(g_settings, L"w");
    if (!f) { g_log("DLSS settings: %ls not writable - choices not saved", g_settings); return; }
    fprintf(f, "# DLSS add-on settings. The panel in game (F8, see panel_key) changes and saves them.\n"
               "# super_resolution: off, dlaa, quality, balanced, performance, ultra_performance\n"
               "# frame_generation: off, 2x, 3x, 4x (limited to what your GPU allows)\n"
               "# motion_vectors: per_object (Full: soldiers, horses, trees and grass, the default), camera_only (Fast: for weaker CPUs in very big battles)\n"
               "# frame_cap: off, auto, 60, 90, 120 (rendered frames; auto: the screen's refresh rate / the frames shown per\n"
               "#            rendered frame with frame generation on, e.g. 120 at 2x on a 240 Hz screen)\n"
               "# dlss_preset: default, j, k, l, m (NVIDIA's DLSS model; default: K for DLAA / Quality / Balanced, M for\n"
               "#              Performance, L for Ultra Performance)\n"
               "# sharpness: off, low, medium, high (sharpening of the DLSS output, as games with DLSS built in have)\n"
               "# vsync: on, off (on: frames are shown at the screen's refresh rate, no tearing; off: no cap, but they may tear)\n"
               "# fps_counter: off, top_left, top_right, bottom_left, bottom_right (rendered / shown frames per second)\n"
               "# panel_key: the key that opens this panel in game: f1 ... f12, insert, delete, home, end, pause, page_up (default f8)\n"
               "# addon: on, off (off: the add-on does nothing at all from the next game start; set on again to use it)\n"
               TECH_HELP
               "super_resolution=%s\nframe_generation=%s\nmotion_vectors=%s\nframe_cap=%s\ndlss_preset=%s\nsharpness=%s\nvsync=%s\nfps_counter=%s\npanel_key=%s\n" TECH_LINE "addon=on\nversion=5\n",
            SR_KEYS[g_sr >= 0 && g_sr < 6 ? g_sr : 2], FG_KEYS[g_fg < 4 ? g_fg : 0], g_motion_on ? "per_object" : "camera_only",
            CAP_KEYS[g_fcap >= 0 && g_fcap < 5 ? g_fcap : 1], PRESET_KEYS[g_preset >= 0 && g_preset < 5 ? g_preset : 0],
            SHARP_KEYS[g_sharp >= 0 && g_sharp < 4 ? g_sharp : 2], VSYNC_KEYS[g_vsync != 0], FPSC_KEYS[g_fpsc >= 0 && g_fpsc < 5 ? g_fpsc : 0],
            KEY_KEYS[g_pkey >= 0 && g_pkey < 18 ? g_pkey : 7] TECH_ARG);
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
    g_log("settings panel: super resolution %s, render %ux%u", names[mode >= 0 && mode < 6 ? mode : 0], g_rw, g_rh);
    settings_save();
}
// The frame cap on rendered frames, through NVIDIA Reflex's limiter in the helper (from Rome 2's add-on). Auto (the
// default): with frame generation on, the screen's refresh rate / the frames shown per rendered frame (240 Hz, 2x: 120
// rendered, 240 shown), so every rendered frame gets its generated frames instead of the helper dropping them past the
// screen's rate (Rome 2: 166 rendered fps at 2x showed 1.29 frames per present, capped 121 showed 2.00); no cap without
// frame generation (the helper's V-Sync holds the screen's rate). No G-Sync margin: the helper presents from another
// process than the window's, and NVIDIA's driver gives windowed G-Sync only to the window's own process (AGENT_LOG 140),
// so the screen runs at its fixed rate here. Reflex counts shown frames.
static unsigned g_refresh;   // the game window's screen, Hz (0 unknown: Auto does nothing)
static unsigned g_cap_fps;   // the rendered-frame cap in effect (0 none)
static unsigned refresh_hz(HWND w) {
    MONITORINFOEXW mi;
    DEVMODEW dm;
    memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    HMONITOR m = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    if (!m || !GetMonitorInfoW(m, (MONITORINFO *)&mi) || !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;   // 0 / 1: "the hardware's default"
}
static void cap_apply(void) {
    unsigned per = g_fg + 1;
    g_cap_fps = g_fcap == 1 ? (g_fg && g_refresh ? g_refresh / per : 0) : CAP_FPS[g_fcap];
    // microseconds per shown frame, rounded up: never above the screen's rate (4167 us = 239.98 fps at 240 Hz)
    fgb_set_frame_limit(g_cap_fps ? (1000000 + g_cap_fps * per - 1) / (g_cap_fps * per) : 0);
    fgp_set_cap(g_fcap, g_cap_fps);
    if (g_cap_fps) g_log("frame cap %s: %u rendered fps, %u shown (screen %u Hz, frame generation %ux)", CAP_KEYS[g_fcap], g_cap_fps,
                         g_cap_fps * per, g_refresh, per);
    else g_log("frame cap %s: none%s", CAP_KEYS[g_fcap], g_fcap == 1 ? (g_refresh ? " (frame generation off)" : " (screen rate unknown)") : "");
}
static void panel_cap(int choice) {
    g_fcap = choice >= 0 && choice < 5 ? choice : 1;
    cap_apply();
    settings_save();
}
static void panel_fg(int n) {
    g_fg = (unsigned)n;
    fgb_set_fg((unsigned)n);
    g_log("settings panel: frame generation %s", n ? (n == 1 ? "2x" : n == 2 ? "3x" : "4x") : "off");
    cap_apply();   // Auto and the fixed caps depend on the frames shown per rendered frame
    settings_save();
}
static void panel_motion(int on) {
    g_motion_on = on;
    if (g_motion) fgm_set_enabled(on);
    g_log("settings panel: per-object motion %s", on ? "on" : "off");
    settings_save();
}
#define PANEL_VIEW NULL
static void panel_preset(int choice) {
    g_preset = choice >= 0 && choice < 5 ? choice : 0;
    fgb_set_preset(PRESET_NGX[g_preset]);
    g_log("settings panel: DLSS preset %s", PRESET_KEYS[g_preset]);
    settings_save();
}
static void panel_sharp(int choice) {
    g_sharp = choice >= 0 && choice < 4 ? choice : 2;
    fgb_set_sharpness(SHARP_CON[g_sharp]);
    g_log("settings panel: sharpness %s", SHARP_KEYS[g_sharp]);
    settings_save();
}
// Culling (partcull.c): Off / Light / Normal / Strong, applied and saved in heapfix.ini
void culling_pick(int c);   // heapfix.c
#define panel_cull culling_pick

static void panel_fps(int corner) {
    g_fpsc = corner >= 0 && corner < 5 ? corner : 0;
    g_log("settings panel: frame-rate counter %s", FPSC_KEYS[g_fpsc]);
    settings_save();
}
static void tech_set(int t) {   // 0 auto, 1 fsr, 2 dlss, 3 mixed (the sweep restores auto with it)
    g_tech = t;
    g_switch = fgb_can_restart();   // a helper that can be replaced: now (AGENT_LOG 166); otherwise the pick applies at the next start
    g_log("settings panel: technology %s (%s)", TECH_KEYS[g_tech], g_switch ? "switching now" : "from the next game start");
    settings_save();
}
static void panel_tech(int sr, int fg) { tech_set(sr ? 1 : fg ? 3 : 2); }
#define PANEL_TECH panel_tech
// a new helper (with the picked technology in the FSR build), then every setting again (AGENT_LOG 165, 173)
static void switch_now(void) {
    g_switch = 0;
    unsigned fgb_sr_backend(void), fgb_fg_backend(void);
    int r = fgb_restart(g_tech == 1 ? 2 : g_tech == 3 ? 3 : 1);
    if (r < 0) { g_off_note = L"The add-on could not switch - restart the game. Choices made here are saved."; bridge_off(); return; }
    if (!r) return;
    if (g_fg > fgb_fg_max()) g_fg = fgb_fg_max();
    fgb_set_fg(g_fg);
    sr_set(g_sr);
    fgb_set_preset(PRESET_NGX[g_preset]);
    fgb_set_sharpness(SHARP_CON[g_sharp]);
    fgb_set_vsync(g_vsync);
    cap_apply();
    fgp_set_fg((int)g_fg);
    fgp_set_presets_available(fgb_helper_has_presets());
    fgp_set_vsync_available(fgb_helper_has_vsync());
    fgp_set_vsync_driver(fgb_driver_vsync());
    fgp_set_tech(fgb_sr_backend() == 2, fgb_fg_backend() == 2);
}
static void panel_vsync(int on) {
    g_vsync = on != 0;
    fgb_set_vsync(g_vsync);
    g_log("settings panel: V-Sync %s", VSYNC_KEYS[g_vsync]);
    settings_save();
}

// The F11 panel on the game's device with the current choices (motion: the row's state, -1 / -2 when it is not offered)
static int panel_setup(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT w, UINT h, DXGI_FORMAT fmt, int motion) {
    FgpApply ap = {panel_sr, panel_fg, panel_motion, panel_cap, panel_preset, panel_sharp, PANEL_VIEW, panel_vsync, panel_fps, PANEL_TECH,
                   panel_cull};
    if (!fgp_init(dev, ctx, w, h, fmt, g_sr, (int)g_fg, motion, ap)) return 0;
    fgp_set_preset(g_preset);
    fgp_set_sharp(g_sharp);
    fgp_set_presets_available(fgb_helper_has_presets());
    fgp_set_vsync(g_vsync);
    fgp_set_vsync_available(fgb_helper_has_vsync());
    fgp_set_vsync_driver(fgb_driver_vsync());
    fgp_set_fps_counter(g_fpsc);
    {   // the drop-downs show the pick; auto shows what runs
        unsigned fgb_sr_backend(void), fgb_fg_backend(void);
        int sr = g_tech == 1, fg = g_tech == 1 || g_tech == 3;
        if (g_tech == 0) { sr = fgb_sr_backend() == 2; fg = fgb_fg_backend() == 2; }
        fgp_set_tech(sr, fg);
    }
    return 1;
}

// The add-on is installed but did not start (its helper failed, e.g. under Proton): the game presents as without it, and
// F11 still opens the panel over the game's own frames, with a note why, instead of doing nothing (players took a dead
// F11 for a key conflict, 2026-11-02). Choices are saved for the next start, as in fallback_panel.
static void start_failed_panel(IDXGISwapChain *sc) {
    static int tried, ok;
    static HWND wnd;
    ID3D11Resource *bb = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &iid_resource, (void **)&bb))) return;
    if (!tried) {
        tried = 1;
        DXGI_SWAP_CHAIN_DESC sd;
        D3D11_TEXTURE2D_DESC td;
        ID3D11Device *d = NULL;
        ID3D11DeviceContext *c = NULL;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)bb, &td);
        if (SUCCEEDED(IDXGISwapChain_GetDesc(sc, &sd))) wnd = sd.OutputWindow;
        ID3D11Resource_GetDevice(bb, &d);
        if (d) ID3D11Device_GetImmediateContext(d, &c);
        ok = wnd && c && panel_setup(d, c, td.Width, td.Height, td.Format, -1);   // the panel keeps d and c
        static wchar_t gpu_note[200];
        int d12 = !d || fgb_d3d12_ok(d);
        if (!d12) {   // the card itself: said by name, in the pop-up too
            _snwprintf(gpu_note, 200, L"%ls can't run DLSS or AMD FSR: it lacks DirectX 12 feature level 12_0.",
                       fgb_gpu_name()[0] ? fgb_gpu_name() : L"This graphics card");
            g_log("DLSS add-on: the game's graphics card (%ls) has no Direct3D 12 feature level 12_0 - DLSS and AMD FSR cannot run on it",
                  fgb_gpu_name());
        }
        if (ok) fgp_set_note(d12 ? L"The upscaling add-on did not start - see heapfix.log in the game folder. Choices made here are saved." : gpu_note);
        else { if (c) ID3D11DeviceContext_Release(c); if (d) ID3D11Device_Release(d); }
        g_log("settings panel: %s over the game's frames (the add-on did not start)", ok ? "available" : "NOT available");
    }
    if (ok) {
        static LARGE_INTEGER t0, qf;   // the FPS counter: the game's own frames (all of them are shown)
        static unsigned frames;
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (!qf.QuadPart) { QueryPerformanceFrequency(&qf); t0 = t; }
        frames++;
        double dt = (double)(t.QuadPart - t0.QuadPart) / qf.QuadPart;
        if (dt >= 0.5) { fgp_set_fps(frames / dt, frames / dt); frames = 0; t0 = t; }
        fgp_frame(bb, wnd);
    }
    ID3D11Resource_Release(bb);
}
