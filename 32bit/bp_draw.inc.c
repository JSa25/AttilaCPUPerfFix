// Part of bridgeprobe.c (included there in this order, not compiled on its own): the draw hooks (flag gather, motion pass, the bloom source pass), Map, and the dev tools that
// log draws (vertex dump, the capture's per-draw lines); mp_hook places them.
// ---- vertex dump (dev, motion probe on: motion_on.txt; AGENT_LOG 181): verts_go.txt in the game folder -> the next 4
// frames' draws of the shaders whose vertices the CPU writes every frame (37ca95e9: arrows / trails, 416460f3: distant
// soldiers' billboards) go to verts_dump.bin: per draw the frame, shader, stage, index count / start, base vertex,
// vertex stride, index size, then the indices and a 64 KB vertex window from the base vertex. Whether the game keeps
// their order from frame to frame decides how their motion can be made. Read back at once (a stall: 4 frames, dev only).
static void vdump_draw(ID3D11DeviceContext *c, UINT count, UINT start_idx, INT base_v) {
    if (!g_vdump_left || !g_vdump || c != g_ctx || GetCurrentThreadId() != g_pt_main) return;
    ID3D11VertexShader *vs = NULL;
    ID3D11DeviceContext_VSGetShader(c, &vs, NULL, NULL);
    unsigned long long h = vs ? mp_vs_hash(vs) : 0;
    if (vs) ID3D11VertexShader_Release(vs);
    int want = h == 0x37ca95e9b122bd8dull || h == 0x416460f36ab45618ull;
    for (unsigned k = 0; k < g_vdump_ncounts && !want; k++) want = g_vdump_counts[k] == count;
    if (!want) return;
    ID3D11Buffer *vb = NULL, *ib = NULL;
    UINT st = 0, of = 0, ibo = 0;
    DXGI_FORMAT ibf = DXGI_FORMAT_UNKNOWN;
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 1, &vb, &st, &of);
    ID3D11DeviceContext_IAGetIndexBuffer(c, &ib, &ibf, &ibo);
    if (vb && ib && st) {
        UINT isz = ibf == DXGI_FORMAT_R32_UINT ? 4 : 2;
        D3D11_BUFFER_DESC vd, id;
        ID3D11Buffer_GetDesc(vb, &vd);
        ID3D11Buffer_GetDesc(ib, &id);
        UINT v0 = of + (UINT)base_v * st, i0 = ibo + start_idx * isz, ib_bytes = count * isz;
        UINT vb_bytes = v0 < vd.ByteWidth ? (vd.ByteWidth - v0 < 65536 ? vd.ByteWidth - v0 : 65536) : 0;
        if (i0 + ib_bytes <= id.ByteWidth && vb_bytes) {
            D3D11_BUFFER_DESC sd = {ib_bytes + vb_bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            ID3D11Buffer *stg = NULL;
            if (SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &sd, NULL, &stg))) {
                D3D11_BOX bi = {i0, 0, 0, i0 + ib_bytes, 1, 1}, bv = {v0, 0, 0, v0 + vb_bytes, 1, 1};
                ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)stg, 0, 0, 0, 0, (ID3D11Resource *)ib, 0, &bi);
                ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)stg, 0, ib_bytes, 0, 0, (ID3D11Resource *)vb, 0, &bv);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
                    unsigned hdr[10] = {0x56445550u, g_vdump_frame, (unsigned)h, (unsigned)(h >> 32), mp_stage_vt(), count, start_idx, (unsigned)base_v, st, isz};
                    fwrite(hdr, 4, 10, g_vdump);
                    fwrite(&vb_bytes, 4, 1, g_vdump);
                    fwrite(m.pData, 1, ib_bytes + vb_bytes, g_vdump);
                    ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)stg, 0);
                }
                ID3D11Buffer_Release(stg);
            }
        }
    }
    if (vb) ID3D11Buffer_Release(vb);
    if (ib) ID3D11Buffer_Release(ib);
}

static void mp_draw(ID3D11DeviceContext *c, const char *kind, UINT count, UINT inst, UINT start_inst) {
    if (g_jit_cur && !g_jit_drawn && c == g_ctx) { g_jit_drawn = 1; ps_cb_apply(c); }   // the pixel shaders see the jitter from now on
    if (!g_capturing || !g_cap || c != g_ctx || GetCurrentThreadId() != g_pt_main) return;
    g_mp_draws++;
    ID3D11VertexShader *vs = NULL;
    ID3D11InputLayout *il = NULL;
    ID3D11Buffer *vb[4] = {0}, *cb[4] = {0};
    UINT st[4] = {0}, of[4] = {0};
    ID3D11ShaderResourceView *srv[8] = {0}, *psrv[16] = {0};
    ID3D11SamplerState *psmp[16] = {0};
    ID3D11PixelShader *ps = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 16, psrv);
    ID3D11DeviceContext_PSGetSamplers(c, 0, 16, psmp);
    ID3D11DeviceContext_VSGetShader(c, &vs, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(c, &il);
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 4, vb, st, of);
    ID3D11DeviceContext_VSGetShaderResources(c, 0, 8, srv);
    ID3D11DeviceContext_VSGetConstantBuffers(c, 0, 4, cb);
    ID3D11DeviceContext_OMGetRenderTargets(c, 1, &rtv, &dsv);
    unsigned long long h = vs ? mp_vs_hash(vs) : 0;
    if (vs && !h) g_mp_vs_unknown++;
    int li = il ? mp_layout(il) : -1;
    if (li >= 0) g_mp_il[li].used = 1;
    char line[2048];
    int n = snprintf(line, sizeof line, "  draw %u stage +0x%X %s count %u inst %u start %u vs %016llx il %d", g_mp_draws, mp_stage_vt(), kind,
                     count, inst, start_inst, h, li);
#define snprintf(b, l, ...) (n < (int)sizeof line ? snprintf(b, l, __VA_ARGS__) : 0)   // full: the rest is dropped
    for (int i = 0; i < 4; i++)
        if (vb[i]) {
            D3D11_BUFFER_DESC bd;
            ID3D11Buffer_GetDesc(vb[i], &bd);
            n += snprintf(line + n, sizeof line - n, " vb%d[stride %u off %u size %u u%u]", i, st[i], of[i], bd.ByteWidth, bd.Usage);
        }
    int anim = -1;
    for (int i = 0; i < 8; i++)
        if (srv[i]) {
            ID3D11Resource *r;
            D3D11_RESOURCE_DIMENSION dim;
            ID3D11ShaderResourceView_GetResource(srv[i], &r);
            ID3D11Resource_GetType(r, &dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC td;
                ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
                n += snprintf(line + n, sizeof line - n, " t%d[%ux%u f%u]", i, td.Width, td.Height, td.Format);
                if (td.Width == 768 && td.Height == 512 && td.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) anim = i;
            } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
                D3D11_BUFFER_DESC bd;
                ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &bd);
                n += snprintf(line + n, sizeof line - n, " t%d[buf %u]", i, bd.ByteWidth);
            } else n += snprintf(line + n, sizeof line - n, " t%d[dim %d]", i, dim);
            ID3D11Resource_Release(r);
        }
    for (int i = 0; i < 4; i++)
        if (cb[i]) {
            D3D11_BUFFER_DESC bd;
            ID3D11Buffer_GetDesc(cb[i], &bd);
            n += snprintf(line + n, sizeof line - n, " cb%d[%u]", i, bd.ByteWidth);
        }
    if (rtv || dsv) {
        char t[200];
        ID3D11Resource *bbr = backbuffer();
        desc_of((ID3D11View *)rtv, t, sizeof t, bbr);
        n += snprintf(line + n, sizeof line - n, " rt0[%s]", t);
        desc_of((ID3D11View *)dsv, t, sizeof t, bbr);
        n += snprintf(line + n, sizeof line - n, " ds[%s]", t);
        if (bbr) ID3D11Resource_Release(bbr);
    }
    if (r_rsgetvp) {   // the viewport the GPU uses (shrunk / jittered by the render scale) and the one the game set
        D3D11_VIEWPORT v;
        UINT nv = 1;
        r_rsgetvp(c, &nv, &v);
        if (nv) n += snprintf(line + n, sizeof line - n, " vp[%.2f,%.2f %.0fx%.0f game %.0fx%.0f%s]", v.TopLeftX, v.TopLeftY, v.Width,
                              v.Height, g_ngvp ? g_gvp[0].Width : 0.f, g_ngvp ? g_gvp[0].Height : 0.f, g_jit_on ? " scaled" : "");
    }
    // the pixel shader, the textures it reads and its samplers (AGENT_LOG 139: the audit of every pass below native size)
    if (ps) {
        char sum[17];
        ps_sum(ps, sum);
        n += snprintf(line + n, sizeof line - n, " ps %s reads:", sum);
        for (int i = 0; i < 16; i++)
            if (psrv[i]) {
                ID3D11Resource *r;
                D3D11_RESOURCE_DIMENSION dim;
                ID3D11ShaderResourceView_GetResource(psrv[i], &r);
                ID3D11Resource_GetType(r, &dim);
                if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                    D3D11_TEXTURE2D_DESC td;
                    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
                    n += snprintf(line + n, sizeof line - n, " t%d [%ux%u f%u%s]", i, td.Width, td.Height, td.Format,
                                  td.MipLevels > 1 ? " mips" : "");
                } else n += snprintf(line + n, sizeof line - n, " t%d [dim %d]", i, dim);
                ID3D11Resource_Release(r);
            }
        n += snprintf(line + n, sizeof line - n, " samp:");
        for (int i = 0; i < 16; i++)
            if (psmp[i]) {
                D3D11_SAMPLER_DESC sd;
                ID3D11SamplerState_GetDesc(psmp[i], &sd);
                n += snprintf(line + n, sizeof line - n, " s%d [f0x%X a%u/%u b%.2f]", i, sd.Filter, sd.AddressU, sd.AddressV, sd.MipLODBias);
            }
    }
#undef snprintf
    fprintf(g_cap, "%s%s\n", line, anim >= 0 ? " ANIM" : "");
    // the first draws that read the animation texture: their instance records and VS constant buffers
    if (anim >= 0) {
        g_mp_anim_draws++;
        if (g_mp_anim_draws <= 6 && li >= 0) {
            for (unsigned j = 0; j < g_mp_il[li].n; j++) {
                const D3D11_INPUT_ELEMENT_DESC *e = &g_mp_il[li].e[j];
                if (e->InputSlotClass != D3D11_INPUT_PER_INSTANCE_DATA || e->InputSlot >= 4 || !vb[e->InputSlot]) continue;
                unsigned s = e->InputSlot, rec = st[s] ? st[s] : 16, k = inst < 4 ? inst : 4;
                char tag[96];
                snprintf(tag, sizeof tag, "draw %u vb%u instance records %u..%u", g_mp_draws, s, start_inst, start_inst + k - 1);
                mp_copy_buffer(vb[s], of[s] + start_inst * rec, k * rec, rec, 1, tag);
                break;
            }
            for (int i = 0; i < 4 && g_mp_anim_draws <= 2; i++)
                if (cb[i]) {
                    D3D11_BUFFER_DESC bd;
                    ID3D11Buffer_GetDesc(cb[i], &bd);
                    char tag[96];
                    snprintf(tag, sizeof tag, "draw %u VS cb%d", g_mp_draws, i);
                    mp_copy_buffer(cb[i], 0, bd.ByteWidth < 4096 ? bd.ByteWidth : 4096, 16, 2, tag);
                }
        }
    }
    if (vs) ID3D11VertexShader_Release(vs);
    if (il) ID3D11InputLayout_Release(il);
    for (int i = 0; i < 4; i++) { if (vb[i]) ID3D11Buffer_Release(vb[i]); if (cb[i]) ID3D11Buffer_Release(cb[i]); }
    for (int i = 0; i < 8; i++) if (srv[i]) ID3D11ShaderResourceView_Release(srv[i]);
    for (int i = 0; i < 16; i++) { if (psrv[i]) ID3D11ShaderResourceView_Release(psrv[i]); if (psmp[i]) ID3D11SamplerState_Release(psmp[i]); }
    if (ps) ID3D11PixelShader_Release(ps);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (dsv) ID3D11DepthStencilView_Release(dsv);
}

static void STDMETHODCALLTYPE w_dii(ID3D11DeviceContext *c, UINT ic, UINT inst, UINT si, INT bv, UINT sinst) {
    g_hk[HK_DRAW]++;
    mp_draw(c, "DrawIndexedInstanced", ic, inst, sinst);
    vdump_draw(c, ic, si, bv);
    if (g_fw_on && g_fw_cur >= 0 && c == g_ctx) fs_draw(c, inst, bv, sinst);
    if (g_motion && c == g_ctx && GetCurrentThreadId() == g_pt_main) {
        unsigned vt = mp_stage_vt();
        if (vt == g_vt_main || (vt && (vt == g_vt_pre[0] || vt == g_vt_pre[1] || vt == g_vt_pre[2]))) fgm_draw(c, ic, inst, si, bv, sinst);
    }
    r_dii(c, ic, inst, si, bv, sinst);
}
static void STDMETHODCALLTYPE w_di(ID3D11DeviceContext *c, UINT ic, UINT si, INT bv) {
    mp_draw(c, "DrawIndexed", ic, 1, 0);
    vdump_draw(c, ic, si, bv);   // the arrows and billboards draw this way (siege capture, AGENT_LOG 181)
    r_di(c, ic, si, bv);
}
static void STDMETHODCALLTYPE w_dinst(ID3D11DeviceContext *c, UINT vc, UINT inst, UINT sv, UINT sinst) {
    mp_draw(c, "DrawInstanced", vc, inst, sinst);
    r_dinst(c, vc, inst, sv, sinst);
}
// ---- Bloom with DLSS (AGENT_LOG 141): the tone mapper's bloom source (hdr_tone_mapper.hlsl
// ps_downsample_image_store_luma, DXBC checksum 64232a1ec00e69c2) takes 3 x 3 bilinear samples a quarter texel apart per
// 256 x 256 texel from the HDR image, which DLSS jitters every frame: a small bright spot (a standard's gold) falls between
// and onto the samples in turn and its glow pulsed (user: around a Roman banner at Quality). At its draw the same
// shader runs with the samples moved by the frame's jitter, so it reads what the game reads without DLSS. Below native
// size the frame fills only the image's top-left render size: samples past its last pixel take that pixel, as the
// screen's edge does at native size (the empty rest was blurred into the bloom: the bottom-right of the view 2-3 %
// darker at Quality / Balanced / Performance in game, AGENT_LOG 141).
static ID3D11PixelShader *g_bloom_ps;
static ID3D11Buffer *g_bloom_cb;
static int g_bloom_off;              // 1 F11 Debug View (test builds), 2 ours not built
static unsigned g_bloom_draws;
#include "bp_bloomsrc.inc.c"
typedef HRESULT(WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                    ID3DBlob **, ID3DBlob **);
static int bloom_init(void) {
    compile_fn comp = (compile_fn)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    ID3DBlob *b = NULL;
    D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    int ok = comp && SUCCEEDED(comp(bloom_src, strlen(bloom_src), "fgb_bloom", NULL, NULL, "ps", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, NULL)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &g_bloom_ps)) &&
             SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_bloom_cb));
    if (b) ID3D10Blob_Release(b);
    if (!ok) { g_bloom_off = 2; g_log("frame bridge: the bloom pass with the jitter taken out not built - the game's runs as it is"); }
    return ok;
}
static int bloom_draw(ID3D11DeviceContext *c, UINT vc, UINT sv) {   // 1: drawn here
    ID3D11PixelShader *ps = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    if (ps && (const void *)ps == g_ps_bloom_ds) ID3D11DeviceContext_PSGetShaderResources(c, 0, 1, &srv);
    int done = 0;
    if (srv && (g_bloom_ps || bloom_init())) {
        ID3D11Resource *r;
        D3D11_TEXTURE2D_DESC td = {0};
        ID3D11ShaderResourceView_GetResource(srv, &r);
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
        ID3D11Resource_Release(r);
        float k[4] = {g_jx / td.Width, g_jy / td.Height, (g_rw - 0.5f) / td.Width, (g_rh - 0.5f) / td.Height};   // the scene 1:1 (render pixel = texel)
        ID3D11Buffer *old = NULL;
        r_updsub(c, (ID3D11Resource *)g_bloom_cb, 0, NULL, k, 0, 0);
        ID3D11DeviceContext_PSGetConstantBuffers(c, 2, 1, &old);
        ID3D11DeviceContext_PSSetShader(c, g_bloom_ps, NULL, 0);
        r_pscb(c, 2, 1, &g_bloom_cb);
        r_draw(c, vc, sv);
        ID3D11DeviceContext_PSSetShader(c, ps, NULL, 0);
        r_pscb(c, 2, 1, &old);
        if (old) ID3D11Buffer_Release(old);
        mp_hook(NULL);   // d3d11 can put its own Draw back during PSSetShader (ToB)
        g_bloom_draws++;
        done = 1;
    }
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (ps) ID3D11PixelShader_Release(ps);
    return done;
}
static void STDMETHODCALLTYPE w_draw(ID3D11DeviceContext *c, UINT vc, UINT sv) {
    mp_draw(c, "Draw", vc, 1, 0);
    if (g_ps_bloom_ds && !g_bloom_off && c == g_ctx && g_jit_on && (g_jx || g_jy) && bloom_draw(c, vc, sv)) return;
    r_draw(c, vc, sv);
}
static HRESULT(STDMETHODCALLTYPE *r_map)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
static HRESULT STDMETHODCALLTYPE w_map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE *m) {
    unsigned long long t0 = __rdtsc();
    HRESULT hr = r_map(c, r, sub, t, f, m);
    unsigned long long dt = __rdtsc() - t0;
    g_map_ticks += dt;
    if (g_tsc_ms && dt * 5 > g_tsc_ms && c == g_ctx) slow_map(r, t, f, dt);
    g_hk[HK_MAP]++;
    if (SUCCEEDED(hr) && g_motion && c == g_ctx) fgm_mapped(r, t, m);
    if (SUCCEEDED(hr)) cam_mapped(c, r, t, m);
    return hr;
}

// the draw slots of the context's vtable: hooked at capture start and checked at every stage of the captured frame,
// because something replaced them (campaign run 2026-10-28: draws were seen in the shadow stage only, then none). A slot
// found replaced is logged with the module that owns the new pointer and hooked again on top of it.
static void mp_hook(const char *where) {
    if (!(g_mp_on || g_motion || g_fw_on) || !g_ctx) return;
    void **vt = *(void ***)g_ctx;
    static const struct { size_t off; const char *name; } slots[5] = {
        {offsetof(ID3D11DeviceContextVtbl, DrawIndexedInstanced), "DrawIndexedInstanced"},
        {offsetof(ID3D11DeviceContextVtbl, DrawIndexed), "DrawIndexed"},
        {offsetof(ID3D11DeviceContextVtbl, DrawInstanced), "DrawInstanced"},
        {offsetof(ID3D11DeviceContextVtbl, Draw), "Draw"},
        {offsetof(ID3D11DeviceContextVtbl, Map), "Map"}};
    void *const ours[5] = {(void *)w_dii, (void *)w_di, (void *)w_dinst, (void *)w_draw, (void *)w_map};
    void **const orig[5] = {(void **)&r_dii, (void **)&r_di, (void **)&r_dinst, (void **)&r_draw, (void **)&r_map};
    static HMODULE d3d;
    static int foreign[5];
    if (!d3d) d3d = GetModuleHandleW(L"d3d11.dll");
    for (int i = 0; i < 5; i++) {
        void **slot = &vt[slots[i].off / sizeof(void *)];
        if (*slot == ours[i]) continue;
        HMODULE owner = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)*slot, &owner);
        if (*orig[i] && owner != d3d) {
            // Not d3d11 swapping its own functions (it does, see above): another mod hooked the slot after us and calls
            // ours as its original. Hooking again would make ours call it back, forever (stack overflow). Left to it.
            if (!foreign[i]) {
                wchar_t name[MAX_PATH] = L"?";
                if (owner) GetModuleFileNameW(owner, name, MAX_PATH);
                const wchar_t *b = wcsrchr(name, L'\\');
                g_log("motion pass: %s on the game's D3D11 context is now hooked by %ls - left to it", slots[i].name, b ? b + 1 : name);
                foreign[i] = 1;
            }
            continue;
        }
        if (where && g_cap) {
            HMODULE mod = NULL;
            char name[MAX_PATH] = "?";
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)*slot, &mod);
            if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
            const char *b = strrchr(name, '\\');
            fprintf(g_cap, "  !! %s slot replaced (%s): now %s+0x%X, hooked again\n", slots[i].name, where, b ? b + 1 : name,
                    (unsigned)((UINT_PTR)*slot - (UINT_PTR)mod));
        }
        patch_slot(slot, ours[i], orig[i]);
    }
}

static void mp_capture_start(void) {
    g_mp_draws = g_mp_anim_draws = g_mp_ncopy = g_mp_vs_unknown = 0;
    for (unsigned k = 0; k < g_mp_nil; k++) g_mp_il[k].used = 0;
    mp_hook(NULL);
}

static void mp_capture_end(FILE *f) {
    if (!g_mp_on || !f) return;
    fprintf(f, "== motion probe: %u draws, %u with the animation texture, %u with a vertex shader created before the hooks\n", g_mp_draws,
            g_mp_anim_draws, g_mp_vs_unknown);
    for (unsigned k = 0; k < g_mp_nil; k++) {
        if (!g_mp_il[k].used) continue;
        fprintf(f, "  layout %u (signature %016llx):", k, g_mp_il[k].sig);
        for (unsigned j = 0; j < g_mp_il[k].n; j++) {
            const D3D11_INPUT_ELEMENT_DESC *e = &g_mp_il[k].e[j];
            fprintf(f, " %s%u f%u slot %u off %u %s%u;", g_mp_il[k].names[j], e->SemanticIndex, e->Format, e->InputSlot, e->AlignedByteOffset,
                    e->InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA ? "inst/" : "vert", e->InputSlotClass ? e->InstanceDataStepRate : 0);
        }
        fprintf(f, "\n");
    }
    for (unsigned i = 0; i < g_mp_ncopy; i++) {
        D3D11_MAPPED_SUBRESOURCE m;
        fprintf(f, "  %s (%u bytes, %u per record):\n", g_mp_copy[i].tag, g_mp_copy[i].bytes, g_mp_copy[i].stride);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_mp_copy[i].stg, 0, D3D11_MAP_READ, 0, &m))) {
            const float *v = m.pData;
            const unsigned *u = m.pData;
            for (unsigned w = 0; w < g_mp_copy[i].bytes / 4; w++) {
                if (w % 4 == 0) fprintf(f, "    +%04X:", w * 4);
                fprintf(f, " %12.5g(%08x)", v[w], u[w]);
                if (w % 4 == 3) fprintf(f, "\n");
            }
            if ((g_mp_copy[i].bytes / 4) % 4) fprintf(f, "\n");
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_mp_copy[i].stg, 0);
        }
        ID3D11Buffer_Release(g_mp_copy[i].stg);
    }
    g_mp_ncopy = 0;
}
