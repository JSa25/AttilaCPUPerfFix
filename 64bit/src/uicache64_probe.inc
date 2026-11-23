// Part of uicache64.c (included at its end of the declarations): a probe of the battle UI cache in the game, for finding
// what goes wrong above Sol's back end (2026-11-22: the unit cards flicker). On only with a file uicache_probe.txt in the
// attila64 folder; it writes into attila64\uicache_probe\ and the log, and changes nothing the player sees except that
// for three single frames the cards are drawn the game's way (the comparison pictures).
//   - pictures (.bmp, 8 bits a colour): the game's own cards on the target (gameway_N), the cache's two textures right
//     after a redraw (black_N, white_N), the target after the cache's picture was put over it, in a redraw frame
//     (redrawn_N) and in a frame shown from the textures (reused_N); all of the panel's rectangle;
//   - the first three redraws, call by call: target changes, viewports, scissor rectangles, and for each draw its
//     blend, depth-stencil and rasterizer states and pixel shader;
//   - every 5 s: redraws by reason, frames shown from the textures, and how many times a frame the panel's Draw ran.
static int g_pr_on;
static wchar_t g_pr_dir[MAX_PATH];
static int g_pr_trace;                   // calls of the redraw being traced logged (count)
static unsigned g_pr_traced;             // redraws traced so far
static int g_pr_game_way;                // the Draw running now is drawn the game's way for a picture
static Target *g_pr_t;                   // the panel of that Draw
static int g_pr_stage, g_pr_n;           // picture schedule: 0 wait, 1 game's way, 2 redraw, 3 reuse; round n of 3
static DWORD g_pr_next;                  // GetTickCount when the next round starts
static LONG g_pr_frame = -1;
static unsigned g_pr_in_frame, g_pr_hist[4], g_pr_reuse0, g_pr_why0[6];
static DWORD g_pr_stat_at;

void uicache64_probe(const wchar_t *home) {
#ifndef UC_PROBE   // only in a probe build (-DUC_PROBE): the released plugin never switches it on
    (void)home;
    return;
#endif
    wchar_t f[MAX_PATH];
    _snwprintf(f, MAX_PATH, L"%ls\\uicache_probe.txt", home);
    if (GetFileAttributesW(f) == INVALID_FILE_ATTRIBUTES) return;
    _snwprintf(g_pr_dir, MAX_PATH, L"%ls\\uicache_probe", home);
    CreateDirectoryW(g_pr_dir, NULL);
    g_pr_on = 1;
    if (g_log) g_log("ui cache probe on (uicache_probe.txt): pictures into %ls, call log of the first redraws", g_pr_dir);
}

static float pr_half(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? (float)m / 1024.0f / 16384.0f : e == 31 ? 65504.0f : (1.0f + (float)m / 1024.0f) * (float)(1u << e) / 32768.0f;
    return s ? -v : v;
}
static uint8_t pr_8(float v) { v = v < 0 ? 0 : v > 1 ? 1 : v; return (uint8_t)(v * 255.0f + 0.5f); }

static int pr_format_ok(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_R10G10B10A2_UNORM;
}
// An 8-bit picture (rows of (w * 3 + 3) & ~3 bytes, blue green red) as a .bmp, top row first (a negative height).
static void pr_bmp(const char *name, const uint8_t *out, int w, int h) {
    int row = (w * 3 + 3) & ~3;
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\%hs.bmp", g_pr_dir, name);
    HANDLE fh = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (fh == INVALID_HANDLE_VALUE) return;
    uint8_t hd[54] = {'B', 'M'};
    uint32_t size = 54 + (uint32_t)row * (uint32_t)h, off = 54, info = 40, planes_bits = 1 | (24 << 16), img = (uint32_t)row * (uint32_t)h;
    int32_t ww = w, hh = -h;
    memcpy(hd + 2, &size, 4); memcpy(hd + 10, &off, 4); memcpy(hd + 14, &info, 4); memcpy(hd + 18, &ww, 4); memcpy(hd + 22, &hh, 4);
    memcpy(hd + 26, &planes_bits, 4); memcpy(hd + 34, &img, 4);
    DWORD wr;
    WriteFile(fh, hd, 54, &wr, NULL);
    WriteFile(fh, out, (DWORD)row * (DWORD)h, &wr, NULL);
    CloseHandle(fh);
}
// A staging texture (w x h, format f, already copied into) as an 8-bit picture, written as name.bmp unless name is
// NULL. Returns the picture (HeapFree it) when keep is set, else NULL.
static uint8_t *pr_write(const char *name, ID3D11Texture2D *st, int w, int h, DXGI_FORMAT f, int keep) {
    int half = f == DXGI_FORMAT_R16G16B16A16_FLOAT, rgba = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        ten = f == DXGI_FORMAT_R10G10B10A2_UNORM;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) return NULL;
    int row = (w * 3 + 3) & ~3;
    uint8_t *out = HeapAlloc(GetProcessHeap(), 0, (size_t)row * h);
    if (out) {
        for (int j = 0; j < h; j++) {
            const uint8_t *p = (const uint8_t *)m.pData + (size_t)j * m.RowPitch;
            uint8_t *o = out + (size_t)j * row;
            for (int i = 0; i < w; i++, o += 3) {
                float r, g, b;
                if (half) { const uint16_t *q = (const uint16_t *)p + i * 4; r = pr_half(q[0]); g = pr_half(q[1]); b = pr_half(q[2]); }
                else if (ten) { uint32_t v = ((const uint32_t *)p)[i]; r = (float)(v & 1023) / 1023; g = (float)((v >> 10) & 1023) / 1023; b = (float)((v >> 20) & 1023) / 1023; }
                else { const uint8_t *q = p + i * 4; r = q[rgba ? 0 : 2] / 255.0f; g = q[1] / 255.0f; b = q[rgba ? 2 : 0] / 255.0f; }
                o[0] = pr_8(b); o[1] = pr_8(g); o[2] = pr_8(r);
            }
        }
        if (name) pr_bmp(name, out, w, h);
        if (!keep) { HeapFree(GetProcessHeap(), 0, out); out = NULL; }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
    return out;
}
// A w x h rectangle of src at x, y as a .bmp.
static void pr_save(const char *name, ID3D11Texture2D *src, int x, int y, int w, int h) {
    if (!src || !g_ctx || !g_dev || w <= 0 || h <= 0) return;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(src, &d);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((UINT)(x + w) > d.Width) w = (int)d.Width - x;
    if ((UINT)(y + h) > d.Height) h = (int)d.Height - y;
    if (w <= 0 || h <= 0 || d.SampleDesc.Count != 1) return;
    if (!pr_format_ok(d.Format)) { if (g_log) g_log("ui cache probe: %s not saved, format %u", name, (unsigned)d.Format); return; }
    D3D11_TEXTURE2D_DESC sd = {(UINT)w, (UINT)h, 1, 1, d.Format, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    ID3D11Texture2D *st = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &sd, NULL, &st))) return;
    D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1};
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)src, 0, &box);
    pr_write(name, st, w, h, d.Format, 0);
    REL(st);
    if (g_log) g_log("ui cache probe: %s saved (%dx%d at %d,%d of a %ux%u target, format %u)", name, w, h, x, y, d.Width, d.Height, (unsigned)d.Format);
}

// ---- the frame recorder: the panel's rectangle of every finished frame (at Present) in a ring of the last PR_BEFORE
// frames, with what the cache did in each. Ctrl+Shift+U (the player sees the flicker): those frames are kept, the next
// PR_AFTER are recorded with the cache off (the game's way, the same moment for comparison), and all of it is written:
// each frame against the one before it in the log, a picture of every frame that jumps and of the one before it.
// Three times at most.
enum { PR_BEFORE = 90, PR_AFTER = 90, PR_ALL = PR_BEFORE + PR_AFTER, PR_EVENTS = 3 };
static ID3D11Texture2D *g_pr_ring[PR_ALL];
static int g_pr_ring_path[PR_ALL];   // 0 shown from the textures, 1-5 redrawn (first, moved, input, interval, cursor), 8 the game's way, 9 not drawn
static int g_pr_path = 9;            // what the cache did with the panel in the frame being drawn
static int g_pr_ri, g_pr_rn;         // next ring slot, slots filled
static int g_pr_phase;               // 0 off, 1 recording the ring, 2 recording after the key, 3..5 waiting for the copies
static int g_pr_after, g_pr_event;
static int g_pr_sx, g_pr_sy, g_pr_sw, g_pr_sh;
static DXGI_FORMAT g_pr_sf;
SHORT (WINAPI *pr_key)(int) = GetAsyncKeyState;   // the test has keys of its own
static const char *const g_pr_path_name[10] = {"shown from the textures", "redrawn: first", "redrawn: moved or details", "redrawn: input",
                                                "redrawn: interval", "redrawn: cursor over it", "?", "?", "the game's way", "panel not drawn"};
static void pr_copy(ID3D11Texture2D *bb, int slot) {
    D3D11_TEXTURE2D_DESC sd = {(UINT)g_pr_sw, (UINT)g_pr_sh, 1, 1, g_pr_sf, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    if (!g_pr_ring[slot] && FAILED(ID3D11Device_CreateTexture2D(g_dev, &sd, NULL, &g_pr_ring[slot]))) g_pr_ring[slot] = NULL;
    if (!g_pr_ring[slot]) return;
    D3D11_BOX box = {(UINT)g_pr_sx, (UINT)g_pr_sy, 0, (UINT)(g_pr_sx + g_pr_sw), (UINT)(g_pr_sy + g_pr_sh), 1};
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_pr_ring[slot], 0, 0, 0, 0, (ID3D11Resource *)bb, 0, &box);
    g_pr_ring_path[slot] = g_pr_path;
}
static void pr_write_event(void) {
    int row = (g_pr_sw * 3 + 3) & ~3, n = g_pr_rn < PR_BEFORE ? g_pr_rn : PR_BEFORE;
    uint8_t *prev = NULL;
    int prev_saved = 0;
    char prev_name[40] = "";
    unsigned jumps[2] = {0, 0};
    for (int k = 0; k < n + PR_AFTER; k++) {
        int slot = k < n ? (g_pr_ri - n + k + PR_BEFORE) % PR_BEFORE : PR_BEFORE + (k - n);
        if (!g_pr_ring[slot]) continue;
        char nm[40];
        if (k < n) _snprintf(nm, sizeof nm, "ev%d_cache_%03d", g_pr_event, k - n + 1);
        else _snprintf(nm, sizeof nm, "ev%d_game_%03d", g_pr_event, k - n);
        uint8_t *cur = pr_write(NULL, g_pr_ring[slot], g_pr_sw, g_pr_sh, g_pr_sf, 1);
        if (!cur) continue;
        unsigned big = 0, worst = 0;
        if (prev && k != n)
            for (int y = 0; y < g_pr_sh; y++)
                for (int x = 0; x < g_pr_sw * 3; x += 3) {
                    const uint8_t *a = cur + (size_t)y * row + x, *b = prev + (size_t)y * row + x;
                    unsigned dd = 0;
                    for (int c = 0; c < 3; c++) { unsigned e = a[c] > b[c] ? a[c] - b[c] : b[c] - a[c]; if (e > dd) dd = e; }
                    if (dd > 24) big++;
                    if (dd > worst) worst = dd;
                }
        int jump = big > 500;
        if (jump) {
            jumps[k >= n]++;
            if (!prev_saved && prev) pr_bmp(prev_name, prev, g_pr_sw, g_pr_sh);
            pr_bmp(nm, cur, g_pr_sw, g_pr_sh);
        }
        if (g_log) g_log("ui cache probe: %s (%s): %u pixels changed by more than 24 of 255 from the frame before, worst %u%s", nm,
                         g_pr_path_name[g_pr_ring_path[slot] < 10 ? g_pr_ring_path[slot] : 6], big, worst, jump ? " - JUMP, pictures saved" : "");
        if (prev) HeapFree(GetProcessHeap(), 0, prev);
        prev = cur; prev_saved = jump;
        memcpy(prev_name, nm, sizeof nm);
    }
    if (prev) HeapFree(GetProcessHeap(), 0, prev);
    if (g_log) g_log("ui cache probe: event %d written: %u jumps in the %d frames with the cache, %u in the %d the game's way", g_pr_event, jumps[0], n, jumps[1], PR_AFTER);
}
void uicache64_frame(void *swapchain) {
    if (!g_pr_on || !g_ctx || !g_dev || !swapchain || g_pr_event > PR_EVENTS) return;
    Target *t = &g_t[0];
    if (!g_pr_phase && (!t->valid || !t->tw)) { g_pr_path = 9; return; }   // not before the panel has been cached once
    ID3D11Texture2D *bb = NULL;
    static const GUID iid_tex2d = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
    if (FAILED(IDXGISwapChain_GetBuffer((IDXGISwapChain *)swapchain, 0, &iid_tex2d, (void **)&bb)) || !bb) return;
    if (!g_pr_phase) {   // the panel itself, from its rectangle in the interface's units
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc(bb, &d);
        float sx = g_root_wh[0] > 0 ? (float)d.Width / g_root_wh[0] : 1, sy = g_root_wh[1] > 0 ? (float)d.Height / g_root_wh[1] : 1;
        g_pr_sx = (int)(t->rect[0] * sx); g_pr_sy = (int)(t->rect[1] * sy);
        g_pr_sw = (int)(t->rect[2] * sx); g_pr_sh = (int)(t->rect[3] * sy);
        if (g_pr_sx < 0) g_pr_sx = 0;
        if (g_pr_sy < 0) g_pr_sy = 0;
        if (g_pr_sx + g_pr_sw > (int)d.Width) g_pr_sw = (int)d.Width - g_pr_sx;
        if (g_pr_sy + g_pr_sh > (int)d.Height) g_pr_sh = (int)d.Height - g_pr_sy;
        g_pr_sf = d.Format;
        if (g_pr_sw <= 0 || g_pr_sh <= 0 || !pr_format_ok(d.Format) || d.SampleDesc.Count != 1) { g_pr_event = PR_EVENTS + 1; REL(bb); return; }
        if (g_log) g_log("ui cache probe: frame recorder on: the last %d frames of the panel (%dx%d at %d,%d of the %ux%u back buffer); "
                         "Ctrl+Shift+U when the cards flicker keeps them and records %d more the game's way (%d times at most)",
                         PR_BEFORE, g_pr_sw, g_pr_sh, g_pr_sx, g_pr_sy, d.Width, d.Height, PR_AFTER, PR_EVENTS);
        g_pr_phase = 1; g_pr_event = 1;
    }
    static int was;
    int down = (pr_key(VK_CONTROL) & 0x8000) && (pr_key(VK_SHIFT) & 0x8000) && (pr_key('U') & 0x8000);
    DWORD pid = 0;
    if (down && !was && pr_key == GetAsyncKeyState) GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    int pressed = down && !was && (pr_key != GetAsyncKeyState || pid == GetCurrentProcessId());   // the test presses its own
    was = down;
    if (g_pr_phase == 1) {
        pr_copy(bb, g_pr_ri);
        g_pr_ri = (g_pr_ri + 1) % PR_BEFORE;
        if (g_pr_rn < PR_BEFORE) g_pr_rn++;
        if (pressed) {
            if (g_log) g_log("ui cache probe: Ctrl+Shift+U (event %d): the last %d frames kept, the next %d the game's way", g_pr_event, g_pr_rn, PR_AFTER);
            g_pr_phase = 2; g_pr_after = 0; g_on = 0;
        }
    } else if (g_pr_phase == 2) {
        pr_copy(bb, PR_BEFORE + g_pr_after);
        if (++g_pr_after == PR_AFTER) { g_on = 1; g_pr_phase = 3; }
    } else if (++g_pr_phase == 6) {   // three frames later the copies are done
        pr_write_event();
        g_pr_event++; g_pr_rn = 0; g_pr_ri = 0;
        g_pr_phase = g_pr_event > PR_EVENTS ? 0 : 1;
        if (g_pr_event > PR_EVENTS) for (int i = 0; i < PR_ALL; i++) REL(g_pr_ring[i]);
    }
    g_pr_path = g_on ? 9 : 8;   // the next frame's, until uic64_pre says otherwise
    REL(bb);
}
// The target bound now, as a texture (held: REL it), and its panel rectangle shot.
static void pr_save_target(const char *name, Target *t) {
    ID3D11RenderTargetView *rt = NULL;
    ID3D11Resource *res = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &rt, NULL);
    if (rt) ID3D11RenderTargetView_GetResource(rt, &res);
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (res) ID3D11Resource_GetType(res, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) pr_save(name, (ID3D11Texture2D *)res, t->x, t->y, (int)t->tw, (int)t->th);
    REL(res); REL(rt);
}

// the call log of a traced redraw
#define PRT(...) do { if (g_pr_trace && g_pr_trace < 200 && g_log) { g_pr_trace++; g_log(__VA_ARGS__); } } while (0)
static __attribute__((unused)) void pr_draw_state(VT_PROBE *c, const char *kind, UINT n) {   // unused since the two runs
    if (!g_pr_trace || g_pr_trace >= 200) return;
    ID3D11BlendState *bs = NULL; ID3D11DepthStencilState *ds = NULL; ID3D11RasterizerState *rs = NULL; ID3D11PixelShader *ps = NULL;
    ID3D11DepthStencilView *dsv = NULL; ID3D11RenderTargetView *rt = NULL;
    FLOAT bf[4]; UINT mask, sref;
    ID3D11DeviceContext_OMGetBlendState(c, &bs, bf, &mask);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &ds, &sref);
    ID3D11DeviceContext_RSGetState(c, &rs);
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    ID3D11DeviceContext_OMGetRenderTargets(c, 1, &rt, &dsv);
    D3D11_DEPTH_STENCIL_DESC dd = {0}; D3D11_RASTERIZER_DESC rd = {0}; D3D11_BLEND_DESC bd = {0};
    if (ds) ID3D11DepthStencilState_GetDesc(ds, &dd);
    if (rs) ID3D11RasterizerState_GetDesc(rs, &rd);
    if (bs) ID3D11BlendState_GetDesc(bs, &bd);
    D3D11_VIEWPORT vp = {0}; UINT nvp = 1; D3D11_RECT sc = {0}; UINT nsc = 1;
    ID3D11DeviceContext_RSGetViewports(c, &nvp, &vp);
    ID3D11DeviceContext_RSGetScissorRects(c, &nsc, &sc);
    PRT("ui cache probe:   %s %u: target %s, depth view %p; blend %s %d/%d op %d mask %x; depth %d stencil %d (ref %u); scissor test %d "
        "rect %ld,%ld-%ld,%ld; viewport %.0f,%.0f %.0fx%.0f; pixel shader %p",
        kind, n, rt == g_red->rtv[0] ? "the black texture" : rt == g_red->rtv[1] ? "the white texture" : "ANOTHER", (void *)dsv,
        bd.RenderTarget[0].BlendEnable ? "on" : "off", bd.RenderTarget[0].SrcBlend, bd.RenderTarget[0].DestBlend, bd.RenderTarget[0].BlendOp,
        bd.RenderTarget[0].RenderTargetWriteMask, dd.DepthEnable, dd.StencilEnable, sref, rd.ScissorEnable, sc.left, sc.top, sc.right, sc.bottom,
        vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, (void *)ps);
    REL(bs); REL(ds); REL(rs); REL(ps); REL(dsv); REL(rt);
}

// once a call of uic64_pre for a target: counts, and every 5 s a log line
static void pr_count(Target *t) {
    if (!g_pr_on) return;
    if (g_pr_frame != g_frame_no) {
        if (g_pr_frame >= 0) g_pr_hist[g_pr_in_frame > 3 ? 3 : g_pr_in_frame]++;
        g_pr_frame = g_frame_no; g_pr_in_frame = 0;
    }
    g_pr_in_frame++;
    DWORD now = GetTickCount();
    if (!g_pr_stat_at) g_pr_stat_at = now;
    if (now - g_pr_stat_at >= 5000 && g_log) {
        g_log("ui cache probe: 5 s: redraws first %u, moved/details %u, input %u, interval %u, cursor %u; shown from the textures %u; "
              "frames with the panel's Draw run 1x %u, 2x %u, 3x+ %u",
              t->why[1] - g_pr_why0[1], t->why[2] - g_pr_why0[2], t->why[3] - g_pr_why0[3], t->why[4] - g_pr_why0[4], t->why[5] - g_pr_why0[5],
              t->reuse - g_pr_reuse0, g_pr_hist[1], g_pr_hist[2], g_pr_hist[3]);
        memcpy(g_pr_why0, t->why, sizeof g_pr_why0); g_pr_reuse0 = t->reuse;
        memset(g_pr_hist, 0, sizeof g_pr_hist);
        g_pr_stat_at = now;
    }
}
// 1: this Draw is to be drawn the game's way for a picture (uic64_pre then hands it to the after-stub)
static int pr_want_game_way(void) {
    if (!g_pr_on || g_pr_n >= 3) return 0;
    DWORD now = GetTickCount();
    if (g_pr_stage == 0) { if (!g_pr_next) g_pr_next = now + 5000; if ((LONG)(now - g_pr_next) < 0) return 0; g_pr_stage = 1; }
    return g_pr_stage == 1;
}
