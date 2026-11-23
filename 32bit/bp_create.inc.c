// Part of bridgeprobe.c (included there in this order, not compiled on its own): what the game creates, seen on the device: vertex and pixel shaders, input layouts (and, in
// bp_flagwave.inc.c, the flag wave: its shaders, buffers and the VSSetShader hook).
// ---- Motion probe (dev, AGENT_LOG 97): what a per-object motion pass needs to know. With motion_on.txt in the game
// folder at start, every vertex shader the game creates is appended once (by hash) to motion_vs.bin and every input
// layout's elements are kept. In a bridge capture (bridge_go.txt) every draw is logged with its stage, VS hash, input
// layout, vertex buffers, VS textures / constant buffers and targets; for the first draws that read the animation
// texture (768x512 R32G32B32A32_FLOAT) the instance records and VS constant buffers are copied out at draw time.
static int g_mp_on;
static CRITICAL_SECTION g_mp_lock;
static FILE *g_mp_vsf;
static struct { const void *vs; unsigned long long h; } g_mp_vs[32768];          // VS object -> bytecode hash
static unsigned long long g_mp_written[16384];                                     // hashes already in motion_vs.bin
static unsigned g_mp_nwritten, g_mp_vs_unknown;
#define MP_MAX_LAYOUTS 4096
static struct { const void *il; unsigned n; unsigned long long sig; D3D11_INPUT_ELEMENT_DESC e[16]; char names[16][24]; int used; } *g_mp_il;
static unsigned g_mp_nil;
static HRESULT(STDMETHODCALLTYPE *r_cvs)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11VertexShader **);
static HRESULT(STDMETHODCALLTYPE *r_cil)(ID3D11Device *, const D3D11_INPUT_ELEMENT_DESC *, UINT, const void *, SIZE_T, ID3D11InputLayout **);

static unsigned long long fnv64(const void *p, size_t n) {
    const unsigned char *b = p;
    unsigned long long h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

#include "bp_flagwave.inc.c"

static HRESULT STDMETHODCALLTYPE w_cvs(ID3D11Device *d, const void *bc, SIZE_T len, ID3D11ClassLinkage *cl, ID3D11VertexShader **out) {
    unsigned long long h = fnv64(bc, len);
    if (g_fw_on && (h == 0x55ca645c65c0fbe6ull || h == 0xeb3006be08a96700ull || h == 0x0cae34c7869ca505ull)) {
        int k = h == 0x55ca645c65c0fbe6ull ? 0 : h == 0xeb3006be08a96700ull ? 1 : 2;
        static const char *const kn[FW_KINDS] = {"main view", "shadow", "main view twin"};
        HRESULT hr = r_cvs(d, flag_vs_bc[k], flag_vs_bclen[k], cl, out);
        if (SUCCEEDED(hr)) {
            LONG nth = InterlockedIncrement(&g_fw_swapped[k]);
            if (nth == 1 && g_log) g_log("flag wave: %s shader created from ours", kn[k]);
            if (out && *out) {
                ID3D11VertexShader *own = NULL;   // A/B: the game's own shader too, drawn in the off windows
                if (g_fw_ab && FAILED(r_cvs(d, bc, len, cl, &own))) own = NULL;
                fw_store(k, nth, *out, own);
            }
            if (nth == FW_NVS + 1 && g_log) g_log("flag wave: %s shader made from ours %ld times - more than remembered", kn[k], nth);
            // once: shaders are created on loading threads too, and two of them here together could each take the
            // other's hook for d3d11's own function (patch_slot is not atomic) and call it for ever
            static volatile LONG hooking;
            if (!r_vsss && !InterlockedExchange(&hooking, 1)) {
                ID3D11DeviceContext *c = NULL;
                ID3D11Device_GetImmediateContext(d, &c);
                if (c) {
                    void **vt = *(void ***)c;
                    if (!g_ctx) { g_ctx = c; ID3D11DeviceContext_AddRef(c); }   // the draw hooks (mp_hook) need it without the frame bridge
                    patch_slot(&vt[offsetof(ID3D11DeviceContextVtbl, VSSetConstantBuffers) / sizeof(void *)], (void *)w_vscb, (void **)&r_vscb);
                    patch_slot(&vt[offsetof(ID3D11DeviceContextVtbl, VSSetShaderResources) / sizeof(void *)], (void *)w_vssrv, (void **)&r_vssrv);
                    if (r_vscb && r_vssrv) patch_slot(&vt[offsetof(ID3D11DeviceContextVtbl, VSSetShader) / sizeof(void *)], (void *)w_vsss, (void **)&r_vsss);
                    ID3D11DeviceContext_Release(c);
                }
                if (!r_vsss) InterlockedExchange(&hooking, 0);   // not placed (no context yet): the next copy tries again
            }
        }
        else { hr = r_cvs(d, bc, len, cl, out); if (g_log) g_log("flag wave: ours refused (0x%08lx) - the game's own", (unsigned long)hr); }   // the game's own
        if (!g_mp_on) return hr;
        if (FAILED(hr) || !out || !*out) return hr;
    } else
    {
        HRESULT hr = r_cvs(d, bc, len, cl, out);
        if (!g_mp_on || FAILED(hr) || !out || !*out) return hr;
    }
    HRESULT hr = S_OK;
    EnterCriticalSection(&g_mp_lock);
    unsigned i = (unsigned)(((UINT_PTR)*out >> 4) * 2654435761u) & 32767;
    for (unsigned k = 0; k < 32768; k++, i = (i + 1) & 32767)
        if (!g_mp_vs[i].vs || g_mp_vs[i].vs == *out) { g_mp_vs[i].vs = *out; g_mp_vs[i].h = h; break; }
    int seen = 0;
    for (unsigned k = 0; k < g_mp_nwritten && !seen; k++) seen = g_mp_written[k] == h;
    if (!seen && g_mp_vsf && g_mp_nwritten < 16384) {
        unsigned sz = (unsigned)len;
        fwrite(&h, 8, 1, g_mp_vsf);
        fwrite(&sz, 4, 1, g_mp_vsf);
        fwrite(bc, 1, len, g_mp_vsf);
        fflush(g_mp_vsf);
        g_mp_written[g_mp_nwritten++] = h;
    }
    LeaveCriticalSection(&g_mp_lock);
    return hr;
}

// Pixel shaders the game creates (AGENT_LOG 139): their DXBC checksum (blob bytes 4..11, as Rome 2's audit scripts in
// tools/shader_audit/rome2 key them; shader_index.py maps it to the fxc\ name), so a capture names every draw's pixel shader
static struct { const void *ps; unsigned char sum[8]; } g_ps[16384];
static SRWLOCK g_ps_lock = SRWLOCK_INIT;   // created on loading threads, looked up on the main thread
static const unsigned char BLOOM_DS_SUM[8] = {0x64, 0x23, 0x2a, 0x1e, 0xc0, 0x0e, 0x69, 0xc2};   // the bloom source pass (below)
static const void *g_ps_bloom_ds;          // its shader object
static HRESULT(STDMETHODCALLTYPE *r_cps)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11PixelShader **);
static HRESULT STDMETHODCALLTYPE w_cps(ID3D11Device *d, const void *bc, SIZE_T len, ID3D11ClassLinkage *cl, ID3D11PixelShader **out) {
    HRESULT hr = r_cps(d, bc, len, cl, out);
    if (FAILED(hr) || !out || !*out || len < 12) return hr;
    AcquireSRWLockExclusive(&g_ps_lock);
    unsigned i = (unsigned)(((UINT_PTR)*out >> 4) * 2654435761u) & 16383;
    for (unsigned k = 0; k < 16384; k++, i = (i + 1) & 16383)   // shortcut: full after 16384 shaders, the rest log as unknown
        if (!g_ps[i].ps || g_ps[i].ps == *out) { g_ps[i].ps = *out; memcpy(g_ps[i].sum, (const char *)bc + 4, 8); break; }
    ReleaseSRWLockExclusive(&g_ps_lock);
    if (!memcmp((const char *)bc + 4, BLOOM_DS_SUM, 8)) g_ps_bloom_ds = *out;   // the bloom source pass (AGENT_LOG 141)
    return hr;
}
static void ps_sum(const void *ps, char out[17]) {   // "?" for one made before the hook
    strcpy(out, "?");
    AcquireSRWLockShared(&g_ps_lock);
    unsigned i = (unsigned)(((UINT_PTR)ps >> 4) * 2654435761u) & 16383;
    for (unsigned k = 0; k < 16384 && g_ps[i].ps; k++, i = (i + 1) & 16383)
        if (g_ps[i].ps == ps) { for (int j = 0; j < 8; j++) sprintf(out + 2 * j, "%02x", g_ps[i].sum[j]); break; }
    ReleaseSRWLockShared(&g_ps_lock);
}

static unsigned long long mp_vs_hash(const void *vs) {
    unsigned i = (unsigned)(((UINT_PTR)vs >> 4) * 2654435761u) & 32767;
    for (unsigned k = 0; k < 32768 && g_mp_vs[i].vs; k++, i = (i + 1) & 32767)
        if (g_mp_vs[i].vs == vs) return g_mp_vs[i].h;
    return 0;
}

static HRESULT STDMETHODCALLTYPE w_cil(ID3D11Device *d, const D3D11_INPUT_ELEMENT_DESC *e, UINT n, const void *bc, SIZE_T len,
                                       ID3D11InputLayout **out) {
    HRESULT hr = r_cil(d, e, n, bc, len, out);
    if (FAILED(hr) || !out || !*out) return hr;
    if (g_motion) fgm_layout_created(*out, e, n, fnv64(bc, len));
    if (!g_mp_on) return hr;
    EnterCriticalSection(&g_mp_lock);
    if (g_mp_nil < MP_MAX_LAYOUTS) {
        unsigned k = g_mp_nil++;
        g_mp_il[k].il = *out;
        g_mp_il[k].n = n < 16 ? n : 16;
        g_mp_il[k].sig = fnv64(bc, len);
        for (unsigned j = 0; j < g_mp_il[k].n; j++) {
            g_mp_il[k].e[j] = e[j];
            snprintf(g_mp_il[k].names[j], 24, "%s", e[j].SemanticName ? e[j].SemanticName : "?");
        }
    }
    LeaveCriticalSection(&g_mp_lock);
    return hr;
}

static int mp_layout(const void *il) {
    for (unsigned k = g_mp_nil; k-- > 0;)   // the newest object at an address
        if (g_mp_il[k].il == il) return (int)k;
    return -1;
}

static void patch_slot(void **slot, void *to, void **old);
static void mp_device(IUnknown *dev) {
    static int done;
    if (!(g_mp_on || g_motion || g_fw_on) || done || !dev) return;
    static const GUID iid_dev = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};   // ID3D11Device
    ID3D11Device *d = NULL;
    if (FAILED(IUnknown_QueryInterface(dev, &iid_dev, (void **)&d))) return;
    void **vt = *(void ***)d;
    if (g_mp_on || g_fw_on)
        patch_slot(&vt[offsetof(ID3D11DeviceVtbl, CreateVertexShader) / sizeof(void *)], (void *)w_cvs, (void **)&r_cvs);
    patch_slot(&vt[offsetof(ID3D11DeviceVtbl, CreateInputLayout) / sizeof(void *)], (void *)w_cil, (void **)&r_cil);
    patch_slot(&vt[offsetof(ID3D11DeviceVtbl, CreatePixelShader) / sizeof(void *)], (void *)w_cps, (void **)&r_cps);
    if (g_fw_on) patch_slot(&vt[offsetof(ID3D11DeviceVtbl, CreateBuffer) / sizeof(void *)], (void *)w_cbuf, (void **)&r_cbuf);   // flag sim: marker vertices
    ID3D11Device_Release(d);
    done = 1;
}

// per-draw logging in the captured frame
static void(STDMETHODCALLTYPE *r_dii)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
static void(STDMETHODCALLTYPE *r_di)(ID3D11DeviceContext *, UINT, UINT, INT);
static void(STDMETHODCALLTYPE *r_dinst)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
static void(STDMETHODCALLTYPE *r_draw)(ID3D11DeviceContext *, UINT, UINT);
static unsigned g_mp_draws, g_mp_anim_draws;
static struct { ID3D11Buffer *stg; unsigned bytes, stride, what; char tag[96]; } g_mp_copy[24];
static unsigned g_mp_ncopy;

static unsigned mp_stage_vt(void) {
    for (unsigned i = g_sp < 64 ? g_sp : 64; i-- > 0;)
        if (g_stk[i].id == g_id_stage && g_stk[i].obj) return *(const unsigned *)g_stk[i].obj - g_pt_base;
    return 0;
}

static void mp_copy_buffer(ID3D11Buffer *src, unsigned off, unsigned bytes, unsigned stride, unsigned what, const char *tag) {
    if (g_mp_ncopy >= 24 || !src || !bytes) return;
    D3D11_BUFFER_DESC bd = {bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ID3D11Buffer *stg = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &stg))) return;
    D3D11_BOX box = {off, 0, 0, off + bytes, 1, 1};
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)stg, 0, 0, 0, 0, (ID3D11Resource *)src, 0, &box);
    g_mp_copy[g_mp_ncopy].stg = stg; g_mp_copy[g_mp_ncopy].bytes = bytes; g_mp_copy[g_mp_ncopy].stride = stride;
    g_mp_copy[g_mp_ncopy].what = what;
    snprintf(g_mp_copy[g_mp_ncopy].tag, 96, "%s", tag);
    g_mp_ncopy++;
}
