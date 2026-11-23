// Part of bridgeprobe.c (included there in this order, not compiled on its own): the frame capture's helpers (dev: targets, back buffer dumps), and between them the stage vtables
// and the switches every later part reads.
// capture
static int g_armed, g_capturing, g_captures, g_camera_left, g_dev_from_sc, g_creates;
static unsigned g_frame, g_scene_no, g_dumps;
static FILE *g_cap, *g_cam;
static ID3D11Texture2D *g_staging;
static const void *g_view_obj[16];
static unsigned g_view_sp;
static struct { unsigned id; const void *obj; } g_stk[64];
static unsigned g_sp;

static const GUID iid_resource = {0xdc8e63f3, 0xd12b, 0x4952, {0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d}};   // ID3D11Resource

static int in_game(unsigned a) { return a >= g_pt_base + 0x1000 && a < g_pt_base + g_mod_size; }

// the first return address inside empire.retail.dll above this frame (the UI mod may sit between the game and us)
static unsigned game_caller(const unsigned *sp) {
    for (int i = 0; i < 256; i++) {
        MEMORY_BASIC_INFORMATION m;
        if (i % 64 == 0 && (!VirtualQuery(sp + i, &m, sizeof m) || m.State != MEM_COMMIT)) return 0;
        if (in_game(sp[i])) return sp[i] - g_pt_base;
    }
    return 0;
}

static ID3D11Resource *backbuffer(void) {
    ID3D11Resource *bb = NULL;
    if (g_sc) IDXGISwapChain_GetBuffer(g_sc, 0, &iid_resource, (void **)&bb);
    return bb;
}

static void desc_of(ID3D11View *v, char *out, size_t len, ID3D11Resource *bb) {
    if (!v) { snprintf(out, len, "-"); return; }
    ID3D11Resource *r;
    ID3D11View_GetResource(v, &r);
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
        snprintf(out, len, "%s%ux%u f%u b%x a%u m%u @%p", r == bb ? "BACKBUFFER " : "", d.Width, d.Height, d.Format, d.BindFlags,
                 d.ArraySize, d.MipLevels, (void *)r);
    } else snprintf(out, len, "dim%d @%p", dim, (void *)r);
    ID3D11Resource_Release(r);
}

// targets bound right now: returns 1 if the back buffer is render target 0
static int targets(char *out, size_t len) {
    ID3D11RenderTargetView *rtv[2] = {0};
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 2, rtv, &dsv);
    ID3D11Resource *bb = backbuffer(), *r0 = NULL;
    char a[160], b[160], c[160];
    desc_of((ID3D11View *)rtv[0], a, sizeof a, bb);
    desc_of((ID3D11View *)rtv[1], b, sizeof b, bb);
    desc_of((ID3D11View *)dsv, c, sizeof c, bb);
    snprintf(out, len, "rt0 [%s] rt1 [%s] depth [%s]", a, b, c);
    if (rtv[0]) ID3D11RenderTargetView_GetResource(rtv[0], &r0);
    int is_bb = r0 && r0 == bb;
    if (r0) ID3D11Resource_Release(r0);
    for (int i = 0; i < 2; i++) if (rtv[i]) ID3D11RenderTargetView_Release(rtv[i]);
    if (dsv) ID3D11DepthStencilView_Release(dsv);
    if (bb) ID3D11Resource_Release(bb);
    return is_bb;
}

// the stage vtables the bridge tells apart (main 3D view, HUD, overlay), this game build's rvas (set at install)
static unsigned g_vt_main = 0x1CCE4E4, g_vt_hud = 0x1C9A900, g_vt_overlay = 0x1CA0858;
// the main view's depth prepasses (AGENT_LOG 178): grass (+0x1CCE3E0, +0x1CCE2E4) and trees (+0x1CA0404) draw only there or
// there first, into the main depth; the motion pass takes their swaying draws from them too (0: not this game build)
static unsigned g_vt_pre[3] = {0x1CCE3E0, 0x1CCE2E4, 0x1CA0404};
// the vertex dump (vdump_draw, AGENT_LOG 181): frames left, frame number, file; g_mp_on: the motion probe (defined below)
static int g_vdump_left, g_mp_on;
static int g_shot_armed, g_shots;   // shot_take (AGENT_LOG 182)
static float g_lod_extra;   // fsrtest.txt "bias=": added to the texture mip bias (AGENT_LOG 183)
static unsigned g_vdump_frame;
static unsigned g_vdump_counts[512], g_vdump_ncounts;   // extra index counts to dump (verts_counts.txt)
static FILE *g_vdump;
static int g_switch;   // a new helper after this frame's Present: an F11 pick of upscaler / frame generation, or recovery
static void switch_now(void);
static unsigned g_vt_fx = 0x1C99FB8, g_vt_fx_end = 0x1C9D064;   // transparent effects into the HDR scene; the next stage
void fgb_react_begin(ID3D11Resource *hdr);
void fgb_react_end(void);
// half-size BGRA/RGBA dump of the back buffer: bridge_bb_<capture>_<k>.raw (header: w, h, format)
static void dump_bb(const char *where) {
    if (g_dumps >= 24) return;
    ID3D11Resource *bb = backbuffer();
    if (!bb) return;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)bb, &d);
    if (!g_staging) {
        D3D11_TEXTURE2D_DESC s = d;
        s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
        s.SampleDesc.Count = 1; s.SampleDesc.Quality = 0; s.MipLevels = 1; s.ArraySize = 1;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &s, NULL, &g_staging))) { ID3D11Resource_Release(bb); return; }
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_staging, bb);
    ID3D11Resource_Release(bb);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_staging, 0, D3D11_MAP_READ, 0, &m))) return;
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\bridge_bb_%d_%02u.raw", g_dir, g_captures, g_dumps);
    FILE *f = _wfopen(p, L"wb");
    if (f) {
        unsigned hw = d.Width / 2, hh = d.Height / 2, hdr[3] = {hw, hh, d.Format};
        fwrite(hdr, 4, 3, f);
        static unsigned row[4096];
        for (unsigned y = 0; y < hh && hw <= 4096; y++) {
            const unsigned *src = (const unsigned *)((const unsigned char *)m.pData + (size_t)y * 2 * m.RowPitch);
            for (unsigned x = 0; x < hw; x++) row[x] = src[x * 2];
            fwrite(row, 4, hw, f);
        }
        fclose(f);
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_staging, 0);
    fprintf(g_cap, "    -> back buffer dump %02u (%s)\n", g_dumps, where);
    g_dumps++;
}
