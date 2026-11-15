// Flag cloth simulation, the D3D11 side (dev, notes/banners.md "a real cloth simulation"); used by bridgeprobe.c's flag
// wave and by dlss/proof/flagtest.c. The weight pack puts one marker vertex (never drawn: no index points at it) at the
// start of every flag mesh: "FwSimV01" then six floats (the cloth's x range along the pole, its size, side, the pole's
// y / z in the model). fs_scan finds them in the vertex data the game creates its buffers with; fs_is_flag tells a draw
// of a flag mesh by its vertex buffer and start; fs_gather copies that draw's instance records (the world matrix rows, a
// GPU copy) and notes the model; fs_run runs the simulation once a frame on what the previous frame gathered.
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flagvs.h"
#include "flagsim.h"

#define FS_MODELS 8192
static struct { const void *buf; UINT off; float prm[6]; } fsg_models[FS_MODELS];   // open hash on (buf, off)
static LONG fsg_nmodels;
static SRWLOCK fsg_lock = SRWLOCK_INIT;   // buffers are created on loading threads

static unsigned fs_hash(const void *buf, UINT off) { return (unsigned)(((UINT_PTR)buf >> 4) * 2654435761u ^ off * 40503u) & (FS_MODELS - 1); }

int fs_scan(const void *buf, const void *data, UINT size) {
    const unsigned char *d = data;
    int found = 0;
    if (!d || size < 32) return 0;
    for (UINT o = 0; o + 32 <= size; o++) {
        const unsigned char *p = memchr(d + o, 'F', size - 32 - o + 1);
        if (!p) break;
        o = (UINT)(p - d);
        if (memcmp(p, "FwSimV01", 8)) continue;
        AcquireSRWLockExclusive(&fsg_lock);
        unsigned h = fs_hash(buf, o);
        for (unsigned k = 0; k < FS_MODELS; k++, h = (h + 1) & (FS_MODELS - 1))
            if (!fsg_models[h].buf || (fsg_models[h].buf == buf && fsg_models[h].off == o)) {
                if (!fsg_models[h].buf) fsg_nmodels++;
                fsg_models[h].buf = buf; fsg_models[h].off = o;
                memcpy(fsg_models[h].prm, p + 8, 24);
                found++;
                break;
            }
        ReleaseSRWLockExclusive(&fsg_lock);
    }
    return found;
}
int fs_models(void) { return (int)fsg_nmodels; }

const float *fs_is_flag(const void *buf, UINT off) {
    if (!fsg_nmodels || !buf) return NULL;
    const float *r = NULL;
    AcquireSRWLockShared(&fsg_lock);
    unsigned h = fs_hash(buf, off);
    for (unsigned k = 0; k < FS_MODELS && fsg_models[h].buf; k++, h = (h + 1) & (FS_MODELS - 1))
        if (fsg_models[h].buf == buf && fsg_models[h].off == off) { r = fsg_models[h].prm; break; }
    ReleaseSRWLockShared(&fsg_lock);
    return r;
}

static ID3D11Buffer *fsg_gsh, *fsg_gparam, *fsg_slot, *fsg_grid, *fsg_cb, *fsg_stat, *fsg_gather;   // fsg_stat: staging copy of the header (fs_stats)
static int fsg_stat_wait;
static ID3D11ShaderResourceView *fsg_gather_srv, *fsg_gparam_srv, *fsg_slot_srv, *fsg_grid_srv;
static ID3D11UnorderedAccessView *fsg_slot_uav, *fsg_grid_uav;
static ID3D11ComputeShader *fsg_match, *fsg_step;
static float fsg_gp[FS_GATHER * 8 * 2];   // main-view draws from 0, shadow-pass draws from FS_GATHER (appended in fs_run)
static unsigned fsg_ng, fsg_ns, fsg_frame = 100;
// tuning (flagvs.h tune0 / tune1): wind speed scale, air pressure, skin drag, still-air damping; turbulence, shear,
// bending, gust size. Defaults = setting D, "deep rolling waves" (notes/banners.md, the user picked it), retuned
// for the 24 x 36 grid to the same 1.5 flaps a second and 0.5 m tip swing
static float fsg_tune[8] = {5.0f, 0.8f, 0.03f, 0.8f, 0.45f, 0.08f, 0.005f, 0.35f};   // setting D (v7, notes/banners.md)
void fs_tune(const float *eight) { memcpy(fsg_tune, eight, sizeof fsg_tune); }
// v8 (notes/banners.md): substeps near the camera, lift, gust field size (m), eddy size (m)
static float fsg_tune2[4] = {16.0f, 0.2f, 40.0f, 3.0f};
void fs_tune2(const float *four) { memcpy(fsg_tune2, four, sizeof fsg_tune2); }
static int fsg_ready;

static ID3D11Buffer *fs_mkbuf(ID3D11Device *dev, UINT bytes, UINT bind, UINT misc, UINT stride) {
    D3D11_BUFFER_DESC bd = {bytes, D3D11_USAGE_DEFAULT, bind, 0, misc, stride};
    ID3D11Buffer *b = NULL;
    void *zero = calloc(1, bytes);
    D3D11_SUBRESOURCE_DATA sd = {zero, 0, 0};
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, &sd, &b))) b = NULL;
    free(zero);
    return b;
}
static ID3D11ShaderResourceView *fs_mksrv(ID3D11Device *dev, ID3D11Buffer *b, UINT n, int raw) {
    D3D11_SHADER_RESOURCE_VIEW_DESC v;
    memset(&v, 0, sizeof v);
    v.Format = raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
    v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    v.BufferEx.NumElements = n;
    v.BufferEx.Flags = raw ? D3D11_BUFFEREX_SRV_FLAG_RAW : 0;
    ID3D11ShaderResourceView *r = NULL;
    if (b && FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)b, &v, &r))) r = NULL;
    return r;
}
static ID3D11UnorderedAccessView *fs_mkuav(ID3D11Device *dev, ID3D11Buffer *b, UINT n) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC u;
    memset(&u, 0, sizeof u);
    u.Format = DXGI_FORMAT_UNKNOWN;
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = n;
    ID3D11UnorderedAccessView *r = NULL;
    if (b && FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)b, &u, &r))) r = NULL;
    return r;
}

static ID3D11Device *fsg_dev;
int fs_pass;   // the pass the next fs_gather calls come from: 0 the main view, 1 the shadow pass (the main view owns a flag's cloth)
const void *fs_cs_defines;     // tests: D3D_SHADER_MACRO list for the compute shaders (FS_NO_GUST ...; NULL in the DLL)
ID3D11Query *fs_bench_ts[3];   // tests: timestamps before the match pass, between, after the step pass (NULL in the DLL)
#define FS_REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)
int fs_init(ID3D11Device *dev, char *err, int errlen) {
    if (fsg_ready && dev == fsg_dev) return 1;
    // a new device (a settings change, another GPU in the tests): everything of the old one goes
    FS_REL(fsg_gather_srv); FS_REL(fsg_gparam_srv); FS_REL(fsg_slot_srv); FS_REL(fsg_grid_srv); FS_REL(fsg_slot_uav); FS_REL(fsg_grid_uav);
    FS_REL(fsg_gather); FS_REL(fsg_gparam); FS_REL(fsg_slot); FS_REL(fsg_grid); FS_REL(fsg_cb); FS_REL(fsg_match); FS_REL(fsg_step); FS_REL(fsg_stat); FS_REL(fsg_gsh); fsg_stat_wait = 0;
    fsg_ready = 0; fsg_ng = 0; fsg_ns = 0; fsg_frame = 100; fsg_dev = dev;
    pD3DCompile comp = (pD3DCompile)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    ID3DBlob *b[2] = {0}, *e = NULL;
    static const char *const entry[2] = {"match", "step"};
    for (int k = 0; k < 2; k++)
        if (!comp || FAILED(comp(flag_cs_hlsl, sizeof flag_cs_hlsl - 1, "flagcs", (const D3D_SHADER_MACRO *)fs_cs_defines, NULL, entry[k], "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b[k], &e))) {
            if (err) _snprintf(err, errlen, "%s: %s", entry[k], e ? (const char *)ID3D10Blob_GetBufferPointer(e) : "no d3dcompiler_47");
            return 0;
        }
    ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(b[0]), ID3D10Blob_GetBufferSize(b[0]), NULL, &fsg_match);
    ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(b[1]), ID3D10Blob_GetBufferSize(b[1]), NULL, &fsg_step);
    ID3D10Blob_Release(b[0]); ID3D10Blob_Release(b[1]);
    const UINT nslot = FS_SLOTS * FS_SLOT4 + 1 + 2 * FS_NB, ngrid = FS_SLOTS * FS_N * 2;   // slots, header, the lookup buckets
    fsg_gather = fs_mkbuf(dev, FS_GATHER * 112, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0);
    fsg_gsh = fs_mkbuf(dev, FS_GATHER * 112, 0, 0, 0);   // the shadow pass's draws until fs_run
    fsg_gparam = fs_mkbuf(dev, sizeof fsg_gp, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16);
    fsg_slot = fs_mkbuf(dev, nslot * 16, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16);
    fsg_grid = fs_mkbuf(dev, ngrid * 16, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16);
    fsg_cb = fs_mkbuf(dev, 80, D3D11_BIND_CONSTANT_BUFFER, 0, 0);
    fsg_gather_srv = fs_mksrv(dev, fsg_gather, FS_GATHER * 112 / 4, 1);
    fsg_gparam_srv = fs_mksrv(dev, fsg_gparam, FS_GATHER * 2, 0);
    fsg_slot_srv = fs_mksrv(dev, fsg_slot, nslot, 0);
    fsg_grid_srv = fs_mksrv(dev, fsg_grid, FS_SLOTS * FS_N, 0);   // the VS reads the positions only
    fsg_slot_uav = fs_mkuav(dev, fsg_slot, nslot);
    fsg_grid_uav = fs_mkuav(dev, fsg_grid, ngrid);
    fsg_ready = fsg_gsh && fsg_match && fsg_step && fsg_gather_srv && fsg_gparam_srv && fsg_slot_srv && fsg_grid_srv && fsg_slot_uav && fsg_grid_uav && fsg_cb;
    if (!fsg_ready && err) _snprintf(err, errlen, "buffers / views not created");
    return fsg_ready;
}

void fs_gather(ID3D11DeviceContext *c, ID3D11Buffer *inst, UINT byte_off, UINT n, const float *prm) {
    if (!fsg_ready || !inst) return;
    D3D11_BUFFER_DESC bd;
    ID3D11Buffer_GetDesc(inst, &bd);
    if (byte_off >= bd.ByteWidth) return;
    // the shadow pass draws first and each flag once per cascade: its draws wait in the second half and only fill what
    // the main view leaves (2026-11-12 recording: 768 shadow draws filled the list, the flags on screen got no cloth)
    unsigned *cnt = fs_pass ? &fsg_ns : &fsg_ng, base = fs_pass ? FS_GATHER : 0;
    if (n > FS_GATHER - *cnt) n = FS_GATHER - *cnt;
    if (n > (bd.ByteWidth - byte_off) / 112) n = (bd.ByteWidth - byte_off) / 112;
    if (!n) return;
    D3D11_BOX box = {byte_off, 0, 0, byte_off + n * 112, 1, 1};   // the draw's records in one copy
    ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)(fs_pass ? fsg_gsh : fsg_gather), 0, *cnt * 112, 0, 0, (ID3D11Resource *)inst, 0, &box);
    for (UINT k = 0; k < n; k++) {
        float *q = fsg_gp + (base + *cnt) * 8;
        q[0] = prm[0]; q[1] = prm[1]; q[2] = prm[2]; q[3] = prm[3];
        q[4] = prm[4]; q[5] = prm[5]; q[6] = (float)fs_pass; q[7] = 0;
        (*cnt)++;
    }
}


// ---- flight recorder (notes/banners.md): what the game hands the simulation, frame by frame, for an offline replay
// (dlss/proof/flagreplay.c). Each frame: "FWR1", frame, gathered count, dt, t, camera bound; the gathered parameters
// (8 floats each: x range, size, side, pole y / z, pass); the gathered instance records (112 bytes each); the camera
// block (576 bytes, zeros without it) and the wind block (32). Read back through a ring of staging copies, 3 frames late.
enum { FSR_RING = 4, FSR_BYTES = FS_GATHER * 112 + 576 + 32 };
static FILE *fsr_file;
static int fsr_left, fsr_pos, fsr_used;
static ID3D11Buffer *fsr_stg[FSR_RING];
static struct { unsigned frame, ng; float dt, t; int cam; float gp[FS_GATHER * 8]; } fsr_meta[FSR_RING];
int fs_record_start(const wchar_t *path, int frames) {
    if (fsr_file || !fsg_ready) return 0;
    for (int i = 0; i < FSR_RING; i++)
        if (!fsr_stg[i]) {
            D3D11_BUFFER_DESC d = {FSR_BYTES, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
            if (FAILED(ID3D11Device_CreateBuffer(fsg_dev, &d, NULL, &fsr_stg[i]))) return 0;
        }
    fsr_file = _wfopen(path, L"wb");
    if (!fsr_file) return 0;
    fsr_left = frames; fsr_pos = 0; fsr_used = 0;
    return 1;
}
int fs_recording(void) { return fsr_file != NULL; }
static void fsr_flush_one(ID3D11DeviceContext *c, int i) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)fsr_stg[i], 0, D3D11_MAP_READ, 0, &m))) return;
    const unsigned char *b = m.pData;
    unsigned hdr[6] = {0x31525746u, fsr_meta[i].frame, fsr_meta[i].ng, 0, 0, (unsigned)fsr_meta[i].cam};
    memcpy(&hdr[3], &fsr_meta[i].dt, 4); memcpy(&hdr[4], &fsr_meta[i].t, 4);
    fwrite(hdr, 4, 6, fsr_file);
    fwrite(fsr_meta[i].gp, 32, fsr_meta[i].ng, fsr_file);
    fwrite(b, 112, fsr_meta[i].ng, fsr_file);
    fwrite(b + FS_GATHER * 112, 1, 608, fsr_file);
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)fsr_stg[i], 0);
}
static void fsr_frame(ID3D11DeviceContext *c, ID3D11Buffer *wind, ID3D11Buffer *cam, unsigned ng, float dt, float t) {
    if (!fsr_file) return;
    if (fsr_left > 0) {
        int i = fsr_pos % FSR_RING;
        if (fsr_used >= FSR_RING) fsr_flush_one(c, i);   // the oldest copy, 4 frames old: done on the GPU
        else fsr_used++;
        if (ng) { D3D11_BOX box = {0, 0, 0, ng * 112, 1, 1}; ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)fsr_stg[i], 0, 0, 0, 0, (ID3D11Resource *)fsg_gather, 0, &box); }
        if (cam) { D3D11_BOX box = {0, 0, 0, 576, 1, 1}; ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)fsr_stg[i], 0, FS_GATHER * 112, 0, 0, (ID3D11Resource *)cam, 0, &box); }
        if (wind) { D3D11_BOX box = {0, 0, 0, 32, 1, 1}; ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)fsr_stg[i], 0, FS_GATHER * 112 + 576, 0, 0, (ID3D11Resource *)wind, 0, &box); }
        fsr_meta[i].frame = fsg_frame; fsr_meta[i].ng = ng; fsr_meta[i].dt = dt; fsr_meta[i].t = t; fsr_meta[i].cam = cam != NULL;
        memcpy(fsr_meta[i].gp, fsg_gp, (size_t)ng * 32);
        fsr_pos++; fsr_left--;
        return;
    }
    for (int k = 0; k < fsr_used; k++) fsr_flush_one(c, (fsr_pos - fsr_used + k) % FSR_RING);   // the last ones, oldest first
    fclose(fsr_file); fsr_file = NULL;
}

unsigned fs_run(ID3D11DeviceContext *c, ID3D11Buffer *wind, ID3D11Buffer *cam, float dt, float t) {
    if (!fsg_ready) return 0;
    unsigned add = fsg_ns < FS_GATHER - fsg_ng ? fsg_ns : FS_GATHER - fsg_ng;   // the shadow draws after the main view's
    if (add) {
        D3D11_BOX box = {0, 0, 0, add * 112, 1, 1};
        ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)fsg_gather, 0, fsg_ng * 112, 0, 0, (ID3D11Resource *)fsg_gsh, 0, &box);
        memmove(fsg_gp + fsg_ng * 8, fsg_gp + FS_GATHER * 8, (size_t)add * 32);
        fsg_ng += add;
    }
    fsg_ns = 0;
    unsigned ng = fsg_ng;
    if (dt < 1.0f / 240) dt = 1.0f / 240;
    if (dt > 1.0f / 30) dt = 1.0f / 30;
    fsg_frame++;
    float cb[20] = {(float)ng, dt, t, (float)fsg_frame};
    memcpy(cb + 4, fsg_tune, sizeof fsg_tune);
    memcpy(cb + 12, fsg_tune2, sizeof fsg_tune2);
    cb[16] = cam ? 1.0f : 0.0f;   // detail by distance only with the game's camera
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)fsg_cb, 0, NULL, cb, 0, 0);
    fsr_frame(c, wind, cam, ng, dt, t);
    if (ng) {
        D3D11_BOX box = {0, 0, 0, ng * 32, 1, 1};
        ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)fsg_gparam, 0, &box, fsg_gp, 0, 0);
    }
    // the game's compute state, put back afterwards
    ID3D11ComputeShader *ocs = NULL;
    ID3D11ClassInstance *oci[256];
    UINT onci = 256;
    ID3D11Buffer *ocb[3] = {0};
    ID3D11ShaderResourceView *osrv[2] = {0};
    ID3D11UnorderedAccessView *ouav[2] = {0};
    ID3D11DeviceContext_CSGetShader(c, &ocs, oci, &onci);
    ID3D11DeviceContext_CSGetConstantBuffers(c, 0, 3, ocb);
    ID3D11DeviceContext_CSGetShaderResources(c, 0, 2, osrv);
    ID3D11DeviceContext_CSGetUnorderedAccessViews(c, 0, 2, ouav);
    ID3D11Buffer *cbs[3] = {fsg_cb, wind, cam};
    ID3D11ShaderResourceView *srv[2] = {fsg_gather_srv, fsg_gparam_srv};
    ID3D11UnorderedAccessView *uav[2] = {fsg_slot_uav, fsg_grid_uav};
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 3, cbs);
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 2, srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 2, uav, NULL);
    if (fs_bench_ts[0]) ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)fs_bench_ts[0]);
    ID3D11DeviceContext_CSSetShader(c, fsg_match, NULL, 0);
    ID3D11DeviceContext_Dispatch(c, 1, 1, 1);
    if (fs_bench_ts[1]) ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)fs_bench_ts[1]);
    ID3D11DeviceContext_CSSetShader(c, fsg_step, NULL, 0);
    ID3D11DeviceContext_Dispatch(c, FS_SLOTS, 1, 1);
    if (fs_bench_ts[2]) ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)fs_bench_ts[2]);
    ID3D11DeviceContext_CSSetShader(c, ocs, onci ? oci : NULL, onci);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 3, ocb);
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 2, osrv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 2, ouav, NULL);
    if (ocs) ID3D11ComputeShader_Release(ocs);
    for (UINT k = 0; k < onci && k < 256; k++) if (oci[k]) ID3D11ClassInstance_Release(oci[k]);
    if (ocb[2]) ID3D11Buffer_Release(ocb[2]);
    for (int k = 0; k < 2; k++) {
        if (ocb[k]) ID3D11Buffer_Release(ocb[k]);
        if (osrv[k]) ID3D11ShaderResourceView_Release(osrv[k]);
        if (ouav[k]) ID3D11UnorderedAccessView_Release(ouav[k]);
    }
    fsg_ng = 0;
    return ng;
}

void fs_vs_views(ID3D11ShaderResourceView **two) {
    two[0] = fsg_ready ? fsg_slot_srv : NULL;
    two[1] = fsg_ready ? fsg_grid_srv : NULL;
}
ID3D11Buffer *fs_debug_buffer(int which) { return which ? fsg_grid : fsg_slot; }
// the header's running totals (new claims, flags left out of the lookup) for the log, read back without waiting: one
// call copies them, a later one maps the copy once the GPU is done; returns 1 when out[] got new values
int fs_stats(ID3D11DeviceContext *c, unsigned out[2]) {
    if (!fsg_ready) return 0;
    if (!fsg_stat) {
        D3D11_BUFFER_DESC d = {16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        if (FAILED(ID3D11Device_CreateBuffer(fsg_dev, &d, NULL, &fsg_stat))) return 0;
    }
    if (!fsg_stat_wait) {
        D3D11_BOX box = {FS_SLOTS * FS_SLOT4 * 16, 0, 0, FS_SLOTS * FS_SLOT4 * 16 + 16, 1, 1};
        ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)fsg_stat, 0, 0, 0, 0, (ID3D11Resource *)fsg_slot, 0, &box);
        fsg_stat_wait = 1;
        return 0;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)fsg_stat, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return 0;
    memcpy(out, (const unsigned *)m.pData + 1, 8);
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)fsg_stat, 0);
    fsg_stat_wait = 0;
    return 1;
}
