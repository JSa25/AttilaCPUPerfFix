// Frame bridge, game side (dev only; DLSS_PLAN plan B, AGENT_LOG 94/95): on the game's own D3D11 device, the frame's
// HUD-less colour, depth (R32F), camera motion vectors (RG16F, pixels, previous minus current) and the final colour go
// into textures shared by name with the 64-bit helper (helper64.exe bridge), which presents them through NVIDIA
// Streamline with DLSS Frame Generation into the game's window. Protocol: dlss/proof/fgb_proto.h.
// fgb_hudless() runs where the UI is about to start (the entry of stage +0x1C9A900 in Attila), fgb_present() at Present.
// With DLSS SR on (fgb_set_sr; AGENT_LOG 96) the game renders render_w x render_h into the top-left of its targets with
// the sub-pixel offset fgb_jitter() gives (the caller shifts its viewports), and fgb_hudless() puts the helper's DLSS
// output (full size) into the back buffer before the UI is drawn.
#define COBJMACROS
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "fgb_proto.h"

static const GUID iid_dxgi_resource1 = {0x30961379, 0x4609, 0x4a41, {0x99, 0x8e, 0x54, 0xfe, 0x56, 0x7e, 0xe0, 0xc1}};
static const GUID iid_device5 = {0x8ffde202, 0xa0e7, 0x45df, {0x9e, 0x01, 0xe8, 0x37, 0x80, 0x1b, 0x5e, 0xa0}};
static const GUID iid_context4 = {0x917600da, 0xf58c, 0x4c33, {0x98, 0xd8, 0x3e, 0x15, 0xb3, 0x90, 0xfa, 0x24}};
static const GUID iid_fence = {0xaffde9d1, 0x1df7, 0x4bb7, {0x8a, 0x34, 0x0f, 0x46, 0x25, 0x1d, 0xab, 0x80}};

// depth -> R32F and camera motion vectors from depth: world = inv(vp) x ndc, previous pixel = prev_vp x world
static const char *cs_src =
    "cbuffer B : register(b0) { row_major float4x4 inv_vp; row_major float4x4 prev_vp; float2 size; float2 jit; };\n"
    "Texture2D<float> dep : register(t0);\n"
    "RWTexture2D<float> d32 : register(u0);\n"
    "RWTexture2D<float2> mv : register(u1);\n"
    "[numthreads(8, 8, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "  if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;\n"
    "  float z = dep[id.xy];\n"
    "  d32[id.xy] = z;\n"
    "  float2 cur = float2(id.xy) + 0.5 - jit;\n"   // where this pixel's surface is without the jitter
    "  float2 ndc = float2(cur.x / size.x * 2 - 1, 1 - cur.y / size.y * 2);\n"
    "  float4 w = mul(inv_vp, float4(ndc, z, 1)); w /= w.w;\n"
    "  float4 p = mul(prev_vp, float4(w.xyz, 1)); p.xy /= p.w;\n"
    "  float2 prev = float2((p.x * 0.5 + 0.5) * size.x, (0.5 - p.y * 0.5) * size.y);\n"
    "  mv[id.xy] = prev - cur; }\n";

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11DeviceContext4 *g_ctx4;
static ID3D11Fence *g_fence, *g_fsr, *g_fdone;   // game: 2n+1 / 2n+2; helper SR queue: n+1; helper present queue: n+1
static ID3D11Texture2D *g_final, *g_hud, *g_d32, *g_mv, *g_srout[2], *g_dcopy;
static ID3D11UnorderedAccessView *g_d32_uav, *g_mv_uav;
static ID3D11ShaderResourceView *g_dcopy_srv;
static ID3D11ComputeShader *g_cs;
static ID3D11Buffer *g_cb;
static HANDLE g_handles[9], g_map, g_wake, g_wake2, g_helper;   // wake: the helper SR thread, wake2: its present thread
static FgbShared *g_sh;
static UINT g_w, g_h, g_dw, g_dh;
static uint32_t g_n;          // the frame being built
static int g_hud_done, g_waited, g_live;
static float g_prev_vp[16];
static int g_have_prev;
static unsigned g_pace_timeouts;
static int g_sr;              // DLSS SR on
static UINT g_rw, g_rh;       // render size (the top-left of the targets)
static float g_jx, g_jy;      // this frame's jitter (render pixels, x right, y down)
static unsigned g_sr_frames, g_sr_timeouts;
static double g_sr_wait_ms, g_sr_wait_max;
static void (*g_log)(const char *fmt, ...);

int fgb_active(void) { return g_live && g_sh && !g_sh->error && WaitForSingleObject(g_helper, 0) == WAIT_TIMEOUT; }

static int inverse4(const float *m, float *out) {   // Gauss-Jordan, row-major
    float a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabsf(a[r][c]) > fabsf(a[p][c])) p = r;
        if (fabsf(a[p][c]) < 1e-12f) return 0;
        for (int k = 0; k < 8; k++) { float t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        float d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            float f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = a[r][c + 4];
    return 1;
}

static ID3D11Texture2D *shared_tex(UINT w, UINT h, DXGI_FORMAT f, UINT bind, const wchar_t *name, HANDLE *keep) {
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0,
                              D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
    ID3D11Texture2D *t = NULL;
    IDXGIResource1 *r1 = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t))) return NULL;
    if (FAILED(ID3D11Texture2D_QueryInterface(t, &iid_dxgi_resource1, (void **)&r1)) ||
        FAILED(IDXGIResource1_CreateSharedHandle(r1, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name, keep))) {
        if (r1) IDXGIResource1_Release(r1);
        ID3D11Texture2D_Release(t);
        return NULL;
    }
    IDXGIResource1_Release(r1);
    return t;
}

// dev test (fgbridge_on.txt "fakegpu20" / "fakegpu40"): the helper treats the GPU as allowing limit - 1 generated frames
static uint32_t g_fg_test_limit;
void fgb_set_fg_test_limit(uint32_t limit) { g_fg_test_limit = limit; }
// dev test (fgbridge_on.txt "fakeamd"): the game's GPU is treated as not NVIDIA
static int g_test_not_nvidia;
void fgb_set_test_not_nvidia(int on) { g_test_not_nvidia = on; }

int fgb_start(ID3D11Device *dev, ID3D11DeviceContext *ctx, HWND hwnd, UINT w, UINT h, DXGI_FORMAT fmt, const wchar_t *helper_exe,
              const wchar_t *sl_dir, const wchar_t *log_dir, uint32_t fg_frames, void (*log)(const char *fmt, ...)) {
    g_log = log;
    // NVIDIA only (DLSS runs on NVIDIA RTX GPUs): the game's own adapter is checked before anything is made or started, so
    // on an AMD or Intel GPU (or a laptop whose game runs on the integrated GPU) the game runs exactly as without the bridge
    IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; DXGI_ADAPTER_DESC adesc;
    static const GUID iid_dxgi_device = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
    HRESULT ahr = ID3D11Device_QueryInterface(dev, &iid_dxgi_device, (void **)&xd);
    if (SUCCEEDED(ahr)) ahr = IDXGIDevice_GetAdapter(xd, &ad);
    if (SUCCEEDED(ahr)) ahr = IDXGIAdapter_GetDesc(ad, &adesc);
    if (ad) IDXGIAdapter_Release(ad);
    if (xd) IDXGIDevice_Release(xd);
    if (FAILED(ahr)) { log("frame bridge: the game's GPU could not be identified - off"); return 0; }
    if (adesc.VendorId != 0x10DE || g_test_not_nvidia) {
        log("frame bridge: the game runs on a GPU that is not NVIDIA (vendor 0x%04X%s) - off, the game renders as usual", adesc.VendorId,
            g_test_not_nvidia ? ", test" : "");
        return 0;
    }
    g_dev = dev; g_ctx = ctx; g_w = w; g_h = h;
    ID3D11Device_AddRef(dev);
    ID3D11DeviceContext_AddRef(ctx);
    ID3D11Device5 *d5 = NULL;
    if (FAILED(ID3D11DeviceContext_QueryInterface(ctx, &iid_context4, (void **)&g_ctx4)) ||
        FAILED(ID3D11Device_QueryInterface(dev, &iid_device5, (void **)&d5))) { log("frame bridge: no D3D11.4 device - off"); return 0; }
    DWORD pid = GetCurrentProcessId();
    wchar_t n[11][64];
    static const wchar_t *const what[11] = {L"map", L"wake", L"fence", L"fsr", L"fdone", L"final", L"hudless", L"depth", L"mvec",
                                            L"srout0", L"srout1"};
    for (int i = 0; i < 11; i++) _snwprintf(n[i], 64, L"Local\\fgb_%lu_%ls", (unsigned long)pid, what[i]);
    // one fence per signaller (a fence's value must only grow): this context, the helper's SR queue, its present queue
    ID3D11Fence **fences[3] = {&g_fence, &g_fsr, &g_fdone};
    int ok = 1;
    for (int i = 0; i < 3 && ok; i++)
        ok = SUCCEEDED(ID3D11Device5_CreateFence(d5, 0, D3D11_FENCE_FLAG_SHARED, &iid_fence, (void **)fences[i])) &&
             SUCCEEDED(ID3D11Fence_CreateSharedHandle(*fences[i], NULL, GENERIC_ALL, n[2 + i], &g_handles[i]));
    ID3D11Device5_Release(d5);
    UINT ro = D3D11_BIND_SHADER_RESOURCE, rw = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ok = ok && (g_final = shared_tex(w, h, fmt, ro, n[5], &g_handles[3])) && (g_hud = shared_tex(w, h, fmt, ro, n[6], &g_handles[4])) &&
         (g_d32 = shared_tex(w, h, DXGI_FORMAT_R32_FLOAT, rw, n[7], &g_handles[5])) &&
         (g_mv = shared_tex(w, h, DXGI_FORMAT_R16G16_FLOAT, rw | D3D11_BIND_RENDER_TARGET, n[8], &g_handles[6])) &&   // + the motion pass
         (g_srout[0] = shared_tex(w, h, fmt, rw, n[9], &g_handles[7])) && (g_srout[1] = shared_tex(w, h, fmt, rw, n[10], &g_handles[8])) &&
         SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)g_d32, NULL, &g_d32_uav)) &&
         SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)g_mv, NULL, &g_mv_uav));
    if (!ok) { log("frame bridge: shared fence / textures not created - off"); return 0; }
    pD3DCompile comp = (pD3DCompile)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    ID3DBlob *b = NULL, *err = NULL;
    if (!comp || FAILED(comp(cs_src, strlen(cs_src), "fgb", NULL, NULL, "cs", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err)) ||
        FAILED(ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &g_cs))) {
        log("frame bridge: compute shader not built - off");
        return 0;
    }
    ID3D10Blob_Release(b);
    g_rw = w; g_rh = h;
    D3D11_BUFFER_DESC cbd = {144, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    if (FAILED(ID3D11Device_CreateBuffer(dev, &cbd, NULL, &g_cb))) return 0;

    // each named object must be new: one made first by another process under our name would be read and written by us
    int taken = 0;
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(FgbShared), n[0]);
    taken |= GetLastError() == ERROR_ALREADY_EXISTS;
    g_sh = g_map && !taken ? MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FgbShared)) : NULL;
    g_wake = CreateEventW(NULL, FALSE, FALSE, n[1]);
    taken |= GetLastError() == ERROR_ALREADY_EXISTS;
    wchar_t n2[72];
    _snwprintf(n2, 72, L"%ls2", n[1]);
    g_wake2 = CreateEventW(NULL, FALSE, FALSE, n2);
    taken |= GetLastError() == ERROR_ALREADY_EXISTS;
    if (taken) { log("frame bridge: a shared object with the bridge's name already exists - off"); return 0; }
    if (!g_sh || !g_wake || !g_wake2) return 0;
    memset(g_sh, 0, sizeof *g_sh);
    g_sh->width = w; g_sh->height = h; g_sh->format = fmt; g_sh->hwnd = (uint32_t)(UINT_PTR)hwnd;
    g_sh->luid_lo = adesc.AdapterLuid.LowPart; g_sh->luid_hi = adesc.AdapterLuid.HighPart;
    g_sh->fg_frames = fg_frames;
    g_sh->render_w = g_rw; g_sh->render_h = g_rh; g_sh->sr_on = g_sr;
    g_sh->fg_test_limit = g_fg_test_limit;
    g_sh->proto = FGB_PROTO;

    // the helper: helper64.exe bridge <pid> <streamline dir> <log dir>, killed with this process (job object)
    wchar_t cmd[3 * MAX_PATH + 64];
    _snwprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\" bridge %lu \"%ls\" \"%ls\"", helper_exe, (unsigned long)pid, sl_dir, log_dir);
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { log("frame bridge: helper not started (%lu) - off", GetLastError()); return 0; }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_helper = pi.hProcess;
    for (int i = 0; i < 10000 && !g_sh->helper_ok && !g_sh->error && WaitForSingleObject(g_helper, 0) == WAIT_TIMEOUT; i++) Sleep(1);
    if (!g_sh->helper_ok) { log("frame bridge: helper did not start Streamline - off (see its log)"); return 0; }
    g_live = 1;
    if (fg_frames > g_sh->fg_max) {   // NVIDIA's rules for this GPU (the helper asked Streamline)
        log("frame bridge: frame generation %ux asked, this GPU allows %ux at most - using that", fg_frames + 1, g_sh->fg_max + 1);
        g_sh->fg_frames = g_sh->fg_max;
    }
    log("frame bridge on: %ux%u format %u, frame generation %ux (this GPU allows %s)", w, h, fmt, g_sh->fg_frames + 1,
        !g_sh->fg_max ? "none" : g_sh->fg_max == 1 ? "2x" : g_sh->fg_max == 2 ? "up to 3x" : "up to 4x");
    return 1;
}

void fgb_set_fg(uint32_t frames_to_generate) { if (g_sh) g_sh->fg_frames = frames_to_generate > g_sh->fg_max ? g_sh->fg_max : frames_to_generate; }
unsigned fgb_fg_max(void) { return g_sh ? g_sh->fg_max : 0; }   // generated frames this GPU allows (0 = none)

// per-object motion (fgmotion.c): draws over the camera motion vectors, after the camera pass and before the inputs are
// published; gets the game's depth and this frame's jitter
static void (*g_motion_pass)(ID3D11Resource *depth, float jx, float jy);
void fgb_set_motion(void (*pass)(ID3D11Resource *depth, float jx, float jy)) { g_motion_pass = pass; }
ID3D11Texture2D *fgb_mv_texture(void) { return g_mv; }
unsigned fgb_shown(void) { return g_sh ? g_sh->shown : 0; }   // frames the helper has shown (generated ones included)

// DLSS SR on / off and the render size (<= the output size; equal = DLAA). Takes effect from the next frame.
void fgb_set_sr(int on, UINT render_w, UINT render_h) {
    g_sr = on;
    g_rw = on ? render_w : g_w; g_rh = on ? render_h : g_h;
    if (g_sh) { g_sh->render_w = g_rw; g_sh->render_h = g_rh; g_sh->sr_on = on; }
}

static float halton(unsigned i, unsigned b) {
    float f = 1, r = 0;
    for (; i; i /= b) { f /= b; r += f * (i % b); }
    return r;
}

// this frame's sub-pixel offset in render pixels (x right, y down; 0 with SR off): Halton(2, 3), 8 x (output / render)^2
// phases (NVIDIA's DLSS guide), the same value the helper passes to DLSS as the jitter
void fgb_jitter(float *jx, float *jy) {
    if (!g_sr || !fgb_active()) { *jx = *jy = 0; return; }
    unsigned phases = (unsigned)(8.0f * ((float)g_w / g_rw) * ((float)g_h / g_rh) + 0.5f);
    unsigned i = g_n % (phases ? phases : 8) + 1;
    *jx = g_jx = halton(i, 2) - 0.5f;
    *jy = g_jy = halton(i, 3) - 0.5f;
}

UINT fgb_render_w(void) { return g_rw; }
UINT fgb_render_h(void) { return g_rh; }

// the GPU must not overwrite the shared textures before the helper has copied the previous frame (fence 2n)
static void wait_previous(void) {
    if (g_waited) return;
    g_waited = 1;
    if (g_n) ID3D11DeviceContext4_Wait(g_ctx4, g_fdone, g_n);   // the helper has read frame n-1
}

// main-thread time of the bridge per frame (AGENT_LOG 102: the game ran at 40 fps with the bridge vs 110 without)
static LARGE_INTEGER g_cpu_qf;
static double g_hud_ms, g_present_ms, g_pace_ms;
static unsigned g_cpu_frames;
static double ms_since(LARGE_INTEGER t0) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!g_cpu_qf.QuadPart) QueryPerformanceFrequency(&g_cpu_qf);
    return (double)(t.QuadPart - t0.QuadPart) * 1000.0 / g_cpu_qf.QuadPart;
}
void fgb_cpu_stats(double *hud_ms, double *present_ms, double *pace_ms, unsigned *frames) {
    *hud_ms = g_hud_ms; *present_ms = g_present_ms; *pace_ms = g_pace_ms; *frames = g_cpu_frames;
    g_hud_ms = g_present_ms = g_pace_ms = 0; g_cpu_frames = 0;
}
static void fgb_hudless_(ID3D11Resource *backbuffer, ID3D11Resource *depth, const float vp[16]);
static void depth_upscale(void);
void fgb_hudless(ID3D11Resource *backbuffer, ID3D11Resource *depth, const float vp[16]) {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    fgb_hudless_(backbuffer, depth, vp);
    g_hud_ms += ms_since(t0);
}
static void fgb_hudless_(ID3D11Resource *backbuffer, ID3D11Resource *depth, const float vp[16]) {
    if (!fgb_active()) return;
    wait_previous();
    int sr = g_sr;
    UINT rw = sr ? g_rw : g_w, rh = sr ? g_rh : g_h;
    float jx = sr ? g_jx : 0, jy = sr ? g_jy : 0;
    D3D11_BOX box = {0, 0, 0, rw, rh, 1};
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_hud, 0, 0, 0, 0, backbuffer, 0, &box);
    // depth: own copy (the game's depth is bound as the depth target right now), read as R24 by the compute pass
    D3D11_TEXTURE2D_DESC dd;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)depth, &dd);
    if (!g_dcopy || dd.Width != g_dw || dd.Height != g_dh) {
        if (g_dcopy_srv) ID3D11ShaderResourceView_Release(g_dcopy_srv);
        if (g_dcopy) ID3D11Texture2D_Release(g_dcopy);
        g_dcopy = NULL; g_dcopy_srv = NULL;
        dd.MiscFlags = 0; dd.CPUAccessFlags = 0; dd.Usage = D3D11_USAGE_DEFAULT;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {0};
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        if (dd.Format != DXGI_FORMAT_R24G8_TYPELESS || FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &g_dcopy)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_dcopy, &sd, &g_dcopy_srv))) {
            g_log("frame bridge: depth format %u not supported - no motion vectors", dd.Format);
            return;
        }
        g_dw = dd.Width; g_dh = dd.Height;
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_dcopy, depth);
    float inv[16];
    if (!inverse4(vp, inv)) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    float *c = m.pData;
    memcpy(c, inv, 64);
    memcpy(c + 16, g_have_prev ? g_prev_vp : vp, 64);
    c[32] = (float)rw; c[33] = (float)rh; c[34] = jx; c[35] = jy;
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cb, 0);
    // the game's compute bindings in the slots we use, put back afterwards
    ID3D11ComputeShader *ocs = NULL; ID3D11ClassInstance *oci[1]; UINT onci = 1;
    ID3D11ShaderResourceView *osrv = NULL; ID3D11UnorderedAccessView *ouav[2] = {0}; ID3D11Buffer *ocb = NULL;
    ID3D11DeviceContext_CSGetShader(g_ctx, &ocs, oci, &onci);
    ID3D11DeviceContext_CSGetShaderResources(g_ctx, 0, 1, &osrv);
    ID3D11DeviceContext_CSGetUnorderedAccessViews(g_ctx, 0, 2, ouav);
    ID3D11DeviceContext_CSGetConstantBuffers(g_ctx, 0, 1, &ocb);
    ID3D11UnorderedAccessView *uav[2] = {g_d32_uav, g_mv_uav};
    ID3D11DeviceContext_CSSetShader(g_ctx, g_cs, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(g_ctx, 0, 1, &g_dcopy_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g_ctx, 0, 2, uav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
    ID3D11DeviceContext_Dispatch(g_ctx, (rw + 7) / 8, (rh + 7) / 8, 1);
    ID3D11DeviceContext_CSSetShader(g_ctx, ocs, onci ? oci : NULL, onci);
    ID3D11DeviceContext_CSSetShaderResources(g_ctx, 0, 1, &osrv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(g_ctx, 0, 2, ouav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(g_ctx, 0, 1, &ocb);
    if (ocs) ID3D11ComputeShader_Release(ocs);
    for (UINT i = 0; i < onci; i++) if (oci[i]) ID3D11ClassInstance_Release(oci[i]);
    if (osrv) ID3D11ShaderResourceView_Release(osrv);
    for (int i = 0; i < 2; i++) if (ouav[i]) ID3D11UnorderedAccessView_Release(ouav[i]);
    if (ocb) ID3D11Buffer_Release(ocb);
    if (g_motion_pass) g_motion_pass(depth, jx, jy);
    memcpy(g_sh->vp, vp, 64);
    memcpy(g_sh->prev_vp, g_have_prev ? g_prev_vp : vp, 64);
    memcpy(g_prev_vp, vp, 64);
    g_have_prev = 1;
    g_hud_done = 1;
    if (!sr) return;
    // DLSS SR round trip: inputs ready (2n+1) -> the helper's DLSS output n % 2 (SR fence n+1) -> into the back buffer, then the UI
    g_sh->jitter_x = jx; g_sh->jitter_y = jy;
    ID3D11DeviceContext4_Signal(g_ctx4, g_fence, 2ull * g_n + 1);
    ID3D11DeviceContext_Flush(g_ctx);
    __atomic_store_n(&g_sh->sr_frames, g_n + 1, __ATOMIC_RELEASE);
    SetEvent(g_wake);
    SetEvent(g_wake2);
    // the GPU wait below must never wait for a helper that is gone (a D3D11 fence cannot be signalled from the CPU):
    // wait on the CPU until the helper has submitted its DLSS pass (its SR queue signals n+1 when done)
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
    QueryPerformanceFrequency(&qf);
    for (int i = 0; __atomic_load_n(&g_sh->sr_done, __ATOMIC_ACQUIRE) < g_n + 1; i++) {
        QueryPerformanceCounter(&q1);
        if (g_sh->error || ((i & 63) == 0 && (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart > 250)) {
            g_sr_timeouts++;
            g_log("frame bridge: no DLSS output from the helper for frame %u - bridge off", g_n);
            g_live = 0;
            return;
        }
        if (i < 2000) YieldProcessor(); else Sleep(0);
    }
    QueryPerformanceCounter(&q1);
    double ms = (q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
    g_sr_wait_ms += ms;
    if (ms > g_sr_wait_max) g_sr_wait_max = ms;
    g_sr_frames++;
    ID3D11DeviceContext4_Wait(g_ctx4, g_fsr, g_n + 1);
    ID3D11DeviceContext_CopyResource(g_ctx, backbuffer, (ID3D11Resource *)g_srout[g_n % 2]);
    if (rw < g_w || rh < g_h) depth_upscale();
}

// Below native size (AGENT_LOG 103) the game's depth buffer holds the scene shrunk into its top-left corner, but what the
// game draws after the HUD-less point is full size and depth tested against it (unit icons, outlines: half of the unit
// icons were hidden with Performance). So the depth goes back to full size here: a full-screen pass writes each pixel's
// depth from the copy taken above (nearest render pixel), stencil and colour untouched.
static const char up_src[] =
    "Texture2D<float> d : register(t0);\n"
    "cbuffer c : register(b1) { float4 pad[8]; float4 sz; }\n"
    "float4 vs(uint i : SV_VertexID) : SV_Position { float2 t = float2((i << 1) & 2, i & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "float ps(float4 p : SV_Position) : SV_Depth { uint w, h; d.GetDimensions(w, h); return d.Load(int3(p.xy * sz.xy / float2(w, h), 0)); }\n";
static ID3D11VertexShader *g_up_vs;
static ID3D11PixelShader *g_up_ps;
static ID3D11DepthStencilState *g_up_dss;
static ID3D11RasterizerState *g_up_rs;
static int g_up_failed;
static unsigned g_up_frames;

static void depth_upscale(void) {
    if (g_up_failed || !g_dcopy_srv) return;
    if (!g_up_vs) {
        pD3DCompile comp = (pD3DCompile)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
        ID3DBlob *v = NULL, *f = NULL;
        D3D11_DEPTH_STENCIL_DESC dd = {TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_ALWAYS, FALSE, 0, 0, {0}, {0}};
        D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
        if (!comp || FAILED(comp(up_src, strlen(up_src), "fgb_up", NULL, NULL, "vs", "vs_5_0", 0, 0, &v, NULL)) ||
            FAILED(comp(up_src, strlen(up_src), "fgb_up", NULL, NULL, "ps", "ps_5_0", 0, 0, &f, NULL)) ||
            FAILED(ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v), NULL, &g_up_vs)) ||
            FAILED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(f), ID3D10Blob_GetBufferSize(f), NULL, &g_up_ps)) ||
            FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &dd, &g_up_dss)) || FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_up_rs))) {
            g_log("frame bridge: depth upscale pass not built - the UI may be hidden below native size");
            g_up_failed = 1;
        }
        if (v) ID3D10Blob_Release(v);
        if (f) ID3D10Blob_Release(f);
        if (g_up_failed) return;
    }
    ID3D11DeviceContext *c = g_ctx;
    ID3D11RenderTargetView *rtv[8] = {0}; ID3D11DepthStencilView *dsv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(c, 8, rtv, &dsv);
    if (!dsv) { for (int i = 0; i < 8; i++) if (rtv[i]) ID3D11RenderTargetView_Release(rtv[i]); return; }
    D3D11_VIEWPORT vp[16]; UINT nvp = 16;
    ID3D11DeviceContext_RSGetViewports(c, &nvp, vp);
    ID3D11RasterizerState *rs = NULL; ID3D11DepthStencilState *dss = NULL; UINT sref = 0;
    ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL; ID3D11GeometryShader *gs = NULL; ID3D11HullShader *hs = NULL; ID3D11DomainShader *ds = NULL;
    ID3D11InputLayout *il = NULL; D3D11_PRIMITIVE_TOPOLOGY topo; ID3D11ShaderResourceView *srv = NULL; ID3D11Buffer *cb = NULL;
    ID3D11DeviceContext_RSGetState(c, &rs);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &dss, &sref);
    ID3D11DeviceContext_VSGetShader(c, &vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(c, &gs, NULL, NULL);
    ID3D11DeviceContext_HSGetShader(c, &hs, NULL, NULL);
    ID3D11DeviceContext_DSGetShader(c, &ds, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(c, &il);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &topo);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 1, &srv);
    ID3D11DeviceContext_PSGetConstantBuffers(c, 1, 1, &cb);

    D3D11_VIEWPORT full = {0, 0, (float)g_w, (float)g_h, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, dsv);
    ID3D11DeviceContext_RSSetViewports(c, 1, &full);
    ID3D11DeviceContext_RSSetState(c, g_up_rs);
    ID3D11DeviceContext_OMSetDepthStencilState(c, g_up_dss, 0);
    ID3D11DeviceContext_VSSetShader(c, g_up_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, g_up_ps, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &g_dcopy_srv);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 1, 1, &g_cb);
    ID3D11DeviceContext_Draw(c, 3, 0);
    if (!g_up_frames++) g_log("frame bridge: depth buffer back to full size after DLSS (render %ux%u)", g_rw, g_rh);

    UINT nrt = 0;
    for (UINT i = 0; i < 8; i++) if (rtv[i]) nrt = i + 1;
    ID3D11DeviceContext_OMSetRenderTargets(c, nrt, rtv, dsv);
    ID3D11DeviceContext_RSSetViewports(c, nvp, vp);
    ID3D11DeviceContext_RSSetState(c, rs);
    ID3D11DeviceContext_OMSetDepthStencilState(c, dss, sref);
    ID3D11DeviceContext_VSSetShader(c, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, ps, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, gs, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, hs, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, ds, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(c, il);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, topo);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &srv);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 1, 1, &cb);
    for (int i = 0; i < 8; i++) if (rtv[i]) ID3D11RenderTargetView_Release(rtv[i]);
    ID3D11DepthStencilView_Release(dsv);
#define REL(x) if (x) (x)->lpVtbl->Release(x)
    REL(rs); REL(dss); REL(vs); REL(ps); REL(gs); REL(hs); REL(ds); REL(il); REL(srv); REL(cb);
#undef REL
}

void fgb_sr_stats(unsigned *frames, unsigned *timeouts, double *wait_avg_ms, double *wait_max_ms) {
    *frames = g_sr_frames; *timeouts = g_sr_timeouts;
    *wait_avg_ms = g_sr_frames ? g_sr_wait_ms / g_sr_frames : 0; *wait_max_ms = g_sr_wait_max;
    g_sr_frames = 0; g_sr_wait_ms = g_sr_wait_max = 0;
}

void fgb_present(ID3D11Resource *backbuffer) {
    if (!fgb_active()) return;
    LARGE_INTEGER t0, tp;
    QueryPerformanceCounter(&t0);
    wait_previous();
    if (!g_hud_done) ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_hud, backbuffer);   // no HUD-less point this frame
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_final, backbuffer);
    ID3D11DeviceContext4_Signal(g_ctx4, g_fence, 2ull * g_n + 2);
    ID3D11DeviceContext_Flush(g_ctx);
    g_sh->reset = !g_hud_done;
    __atomic_store_n(&g_sh->frames, g_n + 1, __ATOMIC_RELEASE);
    SetEvent(g_wake);
    SetEvent(g_wake2);
    // at most one frame ahead of the helper (the Reflex sleep in the helper paces it)
    ULONGLONG tick0 = GetTickCount64();
    QueryPerformanceCounter(&tp);
    for (int i = 0; __atomic_load_n(&g_sh->done, __ATOMIC_ACQUIRE) + 1 < g_n + 1; i++) {
        if (g_sh->error || ((i & 63) == 0 && GetTickCount64() - tick0 > 250)) { g_pace_timeouts++; break; }
        if (i < 200) YieldProcessor(); else Sleep(0);
    }
    g_pace_ms += ms_since(tp);
    g_present_ms += ms_since(t0);
    g_cpu_frames++;
    g_n++;
    g_hud_done = g_waited = 0;
    g_jx = g_jy = 0;   // a frame that never asked fgb_jitter() is not jittered
}

unsigned fgb_frames(void) { return g_n; }
unsigned fgb_pace_timeouts(void) { return g_pace_timeouts; }

void fgb_stop(void) {
    if (!g_sh) return;
    __atomic_store_n(&g_sh->frames, FGB_QUIT, __ATOMIC_RELEASE);
    SetEvent(g_wake);
    SetEvent(g_wake2);
    if (g_helper) WaitForSingleObject(g_helper, 5000);
    g_live = 0;
}
