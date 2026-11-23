
// ---- part of the engine fix on 64-bit Attila: perffix64.c calls these (d3d64.c finds the device and the frame)
// The pack switch, as in the 32-bit engine fix since 2026-11-21 (heapfix/fwpack.c, included unchanged): Flag Wave goes
// in only when a loaded pack carries flagwave\flag_wave.txt and it says 1. The loaded packs are the list the loader
// hands the game: <game>\used_mods_64.txt, the loader's own packs folder first, then the player's mod_list.txt
// (loader.c writes it on every start, before the game thread and the plugins). A copy older than this process is from
// an earlier start without a list, and is not read. Returns 1 on, 0 a pack says 0, -1 no loaded pack has the file;
// who gets the deciding pack's name.
#include "fwpack.c"
int flagwave64_pack_switch(const wchar_t *home, char *who, int wn) {
    wchar_t game[MAX_PATH], p[MAX_PATH];
    wcsncpy(game, home, MAX_PATH - 1);
    game[MAX_PATH - 1] = 0;
    wchar_t *s = wcsrchr(game, L'\\');
    if (s) *s = 0;   // the game folder: the attila64 folder's parent, the loader's working folder
    _snwprintf(p, MAX_PATH, L"%ls\\used_mods_64.txt", game);
    static char text[65536];
    size_t n = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    FILETIME made, x1, x2, x3;
    if (GetFileAttributesExW(p, GetFileExInfoStandard, &fa) && GetProcessTimes(GetCurrentProcess(), &made, &x1, &x2, &x3) &&
        CompareFileTime(&fa.ftLastWriteTime, &made) >= 0) {
        FILE *f = _wfopen(p, L"rb");
        if (f) { n = fread(text, 1, sizeof text - 1, f); fclose(f); }
    }
    text[n] = 0;
    return fwp_from_list(text, game, who, wn);
}
static HRESULT STDMETHODCALLTYPE fw64_presented(IDXGISwapChain *sc, UINT sync, UINT flags) { (void)sc; (void)sync; (void)flags; return S_OK; }
// home: the attila64 folder (flagwave.log, flagwave_off.txt, flagwave_ab.txt there). 1 = shaders compiled, waiting for the device
int flagwave64_setup(const wchar_t *home) {
    wcsncpy(g_dir, home, MAX_PATH - 1);
    _snwprintf(g_logp, MAX_PATH, L"%ls\\flagwave.log", g_dir);
    DeleteFileW(g_logp);
    wlog("Flag Wave (cloth banners), part of the 64-bit engine fix, %ls", g_dir);
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\flagwave_off.txt", g_dir);
    g_off = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    wlog("flags %s at start; Ctrl+Shift+F in game switches them off / on (flagwave_off.txt: start off)", g_off ? "OFF" : "on");
    _snwprintf(p, MAX_PATH, L"%ls\\flagwave_ab.txt", g_dir);
    g_ab = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
    if (g_ab) wlog("A/B: flags on in even 10 s windows of UTC time, off in odd (flagwave_ab.txt)");
    QueryPerformanceFrequency(&g_qf);
    r_present = fw64_presented;   // the frame work below runs inside perffix64's own Present hook
    if (!setup()) return 0;
    wlog("on: waiting for the game's device");
    return 1;
}
// The game's device and the context the game was given, right after they are made (before any shader): with the
// upscaling add-on in, that context is ctxwrap64's wrapper, and its table - ours, which d3d11 cannot reset - is where
// the hooks go (change 10). The loader can close its device and make another
// (dx_destroy, then create_device): everything that belonged to the old one is let go here - the shaders, buffers and
// views remembered or lent, the context, and the flag meshes found in the old device's vertex buffers (a new buffer
// can sit at an old one's address). The cloth simulation's own objects move in fs_init, at the next frame with flags.
void fs64_forget_models(void);   // flagsim64.c
static int g_devices;
void flagwave64_device(void *d, void *ctx) {
    if (!g_d3d11) g_d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (g_devices++) {
        fs_unlend(); wind_unlend();
        if (g_wind) ID3D11Buffer_Release(g_wind);
        if (g_cam) ID3D11Buffer_Release(g_cam);
        g_wind = g_cam = NULL;
        AcquireSRWLockExclusive(&g_fwh_lock);
        for (int h = 0; h < FW_NH; h++) {   // ours are held (since 2026-11-22), and the game's own copies are ours
            if (g_fwh[h].vs) ID3D11VertexShader_Release(g_fwh[h].vs);
            if (g_fwh[h].own) ID3D11VertexShader_Release(g_fwh[h].own);
        }
        memset(g_fwh, 0, sizeof g_fwh);
        g_fwh_n = 0;
        ReleaseSRWLockExclusive(&g_fwh_lock);
        g_made[0] = g_made[1] = g_made[2] = 0;
        if (g_ctx) ID3D11DeviceContext_Release(g_ctx);
        g_ctx = NULL;
        g_cur = -1; g_fs_ok = 0; g_last_gather = -1000; g_f_set = g_f_dii = g_f_flag = 0;
        InterlockedExchange(&g_found, 0);
        fs64_forget_models();
        wlog("the game made a new device: starting over on it");
    }
    g_ctx_given = (ID3D11DeviceContext *)ctx;
    hook_device((ID3D11Device *)d);
    wlog("the game's device found");
}
// before each Present, on the drawing thread
void flagwave64_frame(void *swapchain) { w_present((IDXGISwapChain *)swapchain, 0, 0); }
