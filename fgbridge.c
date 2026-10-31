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
#include <d3d12.h>
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
static HWND g_hwnd;           // the game's window (the helper presents into it)
static UINT g_w, g_h, g_dw, g_dh;
static uint32_t g_n;          // the frame being built
static int g_hud_done, g_waited, g_live;
static float g_prev_vp[16];
static int g_have_prev;
static unsigned g_pace_timeouts;
static int g_sr;              // DLSS SR on
static UINT g_rw, g_rh;       // render size (the top-left of the targets)
static float g_jx, g_jy;      // this frame's jitter (render pixels, x right, y down)
static uint32_t g_preset;     // DLSS preset (NVIDIA's number, 0 = its default per mode)
// Sharpening of the DLSS output (games with DLSS built in ship one: NVIDIA took it out of DLSS; Rome 2 DLSS_LOG 45 / 46):
// AMD FidelityFX's RCAS (robust contrast-adaptive sharpening, MIT licence) as a compute pass, from the output into
// g_sharp, then into the back buffer and back into the shared output (frame generation tells the UI from the scene by
// comparing that with the final image: a sharpened final against an unsharpened copy would look like UI everywhere).
// g_sharpness: RCAS's con.x (exp2(-stops)), 0 off.
static ID3D11ComputeShader *g_sharp_cs;
static ID3D11Texture2D *g_sharp;
static ID3D11UnorderedAccessView *g_sharp_uav;
static ID3D11ShaderResourceView *g_srout_srv[2];
static ID3D11Buffer *g_sharp_cb;
static float g_sharpness;
static unsigned g_sharp_frames;
static const char sharp_src[] =
    "Texture2D<float4> src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "cbuffer c : register(b0) { float4 k; }\n"
    "float3 at(int2 p, int2 m) { return src[clamp(p, int2(0, 0), m)].rgb; }\n"
    "[numthreads(8, 8, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "  uint w, h; src.GetDimensions(w, h);\n"
    "  if (id.x >= w || id.y >= h) return;\n"
    "  int2 p = int2(id.xy), m = int2(w - 1, h - 1);\n"
    "  float3 b = at(p + int2(0, -1), m), d = at(p + int2(-1, 0), m), f = at(p + int2(1, 0), m), hh = at(p + int2(0, 1), m);\n"
    "  float4 e4 = src[p];\n"
    "  float3 mn4 = min(min(b, d), min(f, hh)), mx4 = max(max(b, d), max(f, hh));\n"
    "  float3 hitMin = min(mn4, e4.rgb) / (4.0 * mx4 + 1e-6), hitMax = (1.0 - max(mx4, e4.rgb)) / (4.0 * mn4 - 4.0 - 1e-6);\n"
    "  float3 lobeRGB = max(-hitMin, hitMax);\n"
    "  float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * k.x;\n"
    "  dst[p] = float4((lobe * (b + d + f + hh) + e4.rgb) / (4.0 * lobe + 1.0), e4.a);\n"
    "}\n";
static unsigned g_sr_frames, g_sr_timeouts;
static double g_sr_wait_ms, g_sr_wait_max;
static void (*g_log)(const char *fmt, ...);
static DWORD WINAPI watchdog(void *arg);
static volatile DWORD g_present_tid;   // the thread that presents (the game's main thread), for the watchdog

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
    {   // the sharpening pass: without it the output goes out as DLSS made it
        D3D11_TEXTURE2D_DESC sd = {w, h, 1, 1, fmt, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0};
        D3D11_BUFFER_DESC sbd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        ID3DBlob *sb = NULL;
        int sok = SUCCEEDED(comp(sharp_src, strlen(sharp_src), "fgbsharp", NULL, NULL, "cs", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &sb, NULL)) &&
                  SUCCEEDED(ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(sb), ID3D10Blob_GetBufferSize(sb), NULL, &g_sharp_cs)) &&
                  SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &g_sharp)) &&
                  SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)g_sharp, NULL, &g_sharp_uav)) &&
                  SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_srout[0], NULL, &g_srout_srv[0])) &&
                  SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g_srout[1], NULL, &g_srout_srv[1])) &&
                  SUCCEEDED(ID3D11Device_CreateBuffer(dev, &sbd, NULL, &g_sharp_cb));
        if (sb) ID3D10Blob_Release(sb);
        if (!sok) { g_sharp_cs = NULL; log("frame bridge: sharpening not available (back buffer format %u)", fmt); }
    }

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
    g_hwnd = hwnd;
    g_sh->luid_lo = adesc.AdapterLuid.LowPart; g_sh->luid_hi = adesc.AdapterLuid.HighPart;
    g_sh->fg_frames = fg_frames;
    g_sh->render_w = g_rw; g_sh->render_h = g_rh; g_sh->sr_on = g_sr; g_sh->sr_preset = g_preset;
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
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { log("frame bridge: helper not started (%lu) - off", GetLastError()); CloseHandle(job); return 0; }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_helper = pi.hProcess;
    // 10 s by the clock (Sleep(1) lasts 15.6 ms at Windows' default timer, so counting sleeps could take ~2.5 min)
    for (ULONGLONG t0 = GetTickCount64(); !g_sh->helper_ok && !g_sh->error && GetTickCount64() - t0 < 10000 &&
                                          WaitForSingleObject(g_helper, 0) == WAIT_TIMEOUT;)
        Sleep(1);
    if (!g_sh->helper_ok) {   // a slow start must not leave the helper running with the game's window
        TerminateProcess(g_helper, 1);
        WaitForSingleObject(g_helper, 1000);
        log("frame bridge: helper did not start Streamline - off (see its log)");
        return 0;
    }
    g_live = 1;
    HANDLE wd = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    if (wd) CloseHandle(wd);
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
void fgb_set_frame_limit(uint32_t us) { if (g_sh) g_sh->frame_limit_us = us; }   // NVIDIA Reflex frame limit (0 none)
// DLSS preset for every mode, NVIDIA's number (10..13 = J..M, 0 its default per mode); the helper applies it from the next frame
void fgb_set_preset(unsigned preset) { g_preset = preset; if (g_sh) g_sh->sr_preset = preset; }
int fgb_helper_has_presets(void) { return g_sh && (g_sh->helper_caps & FGB_CAP_PRESET); }   // an older add-on's helper: 0
// Sharpening of the DLSS output: RCAS's con (exp2(-stops)), 0 off. Takes effect from the next frame.
void fgb_set_sharpness(float con) { g_sharpness = con; }
unsigned fgb_sharp_frames(void) { return g_sharp_frames; }   // frames sharpened so far
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

// GPU time per frame (AGENT_LOG 109: 120-135 fps without the add-on, ~75 with it, zoomed in on a clump of soldiers, with
// per-object motion on or off): timestamps around the wait for the helper, the HUD-less step, the Present step and the
// frame end, read back 6 frames later without waiting (not ready = counted as not read).
enum { TQ_DIS, TQ_W0, TQ_W1, TQ_H1, TQ_P0, TQ_END, TQ_N };
static ID3D11Query *g_tq[8][TQ_N];
static int g_tq_set[8], g_tq_failed;
static UINT64 g_tq_prev_end;
static uint32_t g_tq_prev_f = (uint32_t)-2;
static double g_gpu_ms[4];   // frame interval, stalled waiting for the helper, HUD-less step, Present step
static unsigned g_gpu_n[4], g_gpu_lost;
static unsigned g_wait_queued, g_wait_frames;   // wait_previous: GPU waits queued, frames

static void tq(int k) {
    unsigned s = g_n % 8;
    if (g_tq_failed) return;
    if (!g_tq[s][k]) {
        D3D11_QUERY_DESC d = {k == TQ_DIS ? D3D11_QUERY_TIMESTAMP_DISJOINT : D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(ID3D11Device_CreateQuery(g_dev, &d, &g_tq[s][k]))) { g_tq_failed = 1; return; }
    }
    if (k == TQ_DIS) { ID3D11DeviceContext_Begin(g_ctx, (ID3D11Asynchronous *)g_tq[s][k]); g_tq_set[s] = 0; }
    else ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g_tq[s][k]);
    g_tq_set[s] |= 1 << k;
}

static void tq_read(uint32_t f) {
    unsigned s = f % 8;
    int set = g_tq_set[s];
    g_tq_set[s] = 0;
    if ((set & 1 << TQ_DIS) == 0 || (set & 1 << TQ_END) == 0) return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT64 t[TQ_N] = {0};
    int ok = ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g_tq[s][TQ_DIS], &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
             !dj.Disjoint && dj.Frequency;
    for (int k = 1; k < TQ_N && ok; k++)
        if (set & 1 << k) ok = ID3D11DeviceContext_GetData(g_ctx, (ID3D11Asynchronous *)g_tq[s][k], &t[k], 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
    if (!ok) { g_gpu_lost++; return; }
    double ms = 1000.0 / (double)dj.Frequency;
    if (g_tq_prev_f == f - 1) { g_gpu_ms[0] += (double)(t[TQ_END] - g_tq_prev_end) * ms; g_gpu_n[0]++; }
    g_tq_prev_end = t[TQ_END];
    g_tq_prev_f = f;
    if ((set & 3 << TQ_W0) == 3 << TQ_W0) { g_gpu_ms[1] += (double)(t[TQ_W1] - t[TQ_W0]) * ms; g_gpu_n[1]++; }
    if (set & 1 << TQ_H1) { g_gpu_ms[2] += (double)(t[TQ_H1] - t[TQ_W1]) * ms; g_gpu_n[2]++; }
    if (set & 1 << TQ_P0) { g_gpu_ms[3] += (double)(t[TQ_END] - t[TQ_P0]) * ms; g_gpu_n[3]++; }
}

// per frame averages since the last call: GPU frame interval, stall on the helper, HUD-less step, Present step (ms)
void fgb_gpu_stats(double ms[4], unsigned *frames, unsigned *lost) {
    for (int i = 0; i < 4; i++) { ms[i] = g_gpu_n[i] ? g_gpu_ms[i] / g_gpu_n[i] : 0; g_gpu_ms[i] = 0; }
    *frames = g_gpu_n[0]; *lost = g_gpu_lost;
    memset(g_gpu_n, 0, sizeof g_gpu_n); g_gpu_lost = 0;
}
void fgb_wait_stats(unsigned *queued, unsigned *frames) {   // GPU waits for the helper queued / frames (wait_previous)
    *queued = g_wait_queued; *frames = g_wait_frames;
    g_wait_queued = g_wait_frames = 0;
}

// the GPU must not overwrite the shared textures before the helper has copied the previous frame (fence 2n)
// AGENT_LOG 109: with a Wait in the game's queue every frame, the game ran ~75 fps where it ran 120-135 without the
// add-on, its main thread blocked in D3D11 calls 3x as long, and the GPU was half idle: the driver holds the game's
// commands behind the Wait. Usually the helper has read the frame long before: the Wait is only queued when it has not.
static void wait_previous(void) {
    if (g_waited) return;
    g_waited = 1;
    tq(TQ_DIS);
    if (!g_n) return;
    g_wait_frames++;
    if (ID3D11Fence_GetCompletedValue(g_fdone) >= g_n) { tq(TQ_W1); return; }   // the helper has read frame n-1 already
    g_wait_queued++;
    tq(TQ_W0);
    ID3D11DeviceContext4_Wait(g_ctx4, g_fdone, g_n);
    tq(TQ_W1);
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
    if (g_tq_set[g_n % 8] & 1 << TQ_W1) tq(TQ_H1);
    g_hud_ms += ms_since(t0);
}
// This frame's depth copy for DLSS and motion vectors (camera, then the per-object pass): once per frame, at the first
// call - fgb_motion_vectors() (another mod's early effects, right after the scene's geometry) or the HUD-less point.
// Returns 1 if they are made. Before writing, the GPU waits until the helper has read the previous frame's.
static unsigned g_mv_frame = ~0u;
static D3D11_TEXTURE2D_DESC g_dfail;   // the depth buffer the copy failed for: not tried (and logged) again every frame
int fgb_make_vectors(ID3D11Resource *depth, const float vp[16]) {
    if (!fgb_active()) return 0;
    if (g_mv_frame == g_n) return 1;
    // depth: own copy (the game's depth may be bound as the depth target right now), read as R24 by the compute pass
    D3D11_TEXTURE2D_DESC dd;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)depth, &dd);
    if (!memcmp(&dd, &g_dfail, sizeof dd)) return 0;
    wait_previous();
    int sr = g_sr;
    UINT rw = sr ? g_rw : g_w, rh = sr ? g_rh : g_h;
    float jx = sr ? g_jx : 0, jy = sr ? g_jy : 0;
    if (!g_dcopy || dd.Width != g_dw || dd.Height != g_dh) {
        D3D11_TEXTURE2D_DESC game = dd;
        if (g_dcopy_srv) ID3D11ShaderResourceView_Release(g_dcopy_srv);
        if (g_dcopy) ID3D11Texture2D_Release(g_dcopy);
        g_dcopy = NULL; g_dcopy_srv = NULL;
        dd.MiscFlags = 0; dd.CPUAccessFlags = 0; dd.Usage = D3D11_USAGE_DEFAULT;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {0};
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        if (dd.Format != DXGI_FORMAT_R24G8_TYPELESS || FAILED(ID3D11Device_CreateTexture2D(g_dev, &dd, NULL, &g_dcopy)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_dcopy, &sd, &g_dcopy_srv))) {
            if (g_dcopy) ID3D11Texture2D_Release(g_dcopy);
            g_dcopy = NULL;
            g_dfail = game;
            g_log("frame bridge: depth format %u with %u samples not supported - no motion vectors", game.Format, game.SampleDesc.Count);
            return 0;
        }
        g_dw = dd.Width; g_dh = dd.Height;
    }
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_dcopy, depth);
    float inv[16];
    if (!inverse4(vp, inv)) return 0;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
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
    g_mv_frame = g_n;
    return 1;
}

static void fgb_hudless_(ID3D11Resource *backbuffer, ID3D11Resource *depth, const float vp[16]) {
    if (!fgb_active()) return;
    wait_previous();
    int sr = g_sr;
    UINT rw = sr ? g_rw : g_w, rh = sr ? g_rh : g_h;
    float jx = sr ? g_jx : 0, jy = sr ? g_jy : 0;
    D3D11_BOX box = {0, 0, 0, rw, rh, 1};
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_hud, 0, 0, 0, 0, backbuffer, 0, &box);
    if (!fgb_make_vectors(depth, vp)) return;
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
            TerminateProcess(g_helper, 1);   // the watchdog stops with g_live = 0: a frozen helper would hold fgb_stop 5 s
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
    if (g_sharpness > 0 && g_sharp_cs) {   // the game's compute bindings in the slots used, put back afterwards
        float k[4] = {g_sharpness, 0, 0, 0};
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_sharp_cb, 0, NULL, k, 0, 0);
        ID3D11ComputeShader *ocs = NULL; ID3D11ClassInstance *oci[1]; UINT onci = 1;
        ID3D11ShaderResourceView *osrv = NULL; ID3D11UnorderedAccessView *ouav = NULL; ID3D11Buffer *ocb = NULL;
        ID3D11DeviceContext_CSGetShader(g_ctx, &ocs, oci, &onci);
        ID3D11DeviceContext_CSGetShaderResources(g_ctx, 0, 1, &osrv);
        ID3D11DeviceContext_CSGetUnorderedAccessViews(g_ctx, 0, 1, &ouav);
        ID3D11DeviceContext_CSGetConstantBuffers(g_ctx, 0, 1, &ocb);
        ID3D11DeviceContext_CSSetShader(g_ctx, g_sharp_cs, NULL, 0);
        ID3D11DeviceContext_CSSetShaderResources(g_ctx, 0, 1, &g_srout_srv[g_n % 2]);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(g_ctx, 0, 1, &g_sharp_uav, NULL);
        ID3D11DeviceContext_CSSetConstantBuffers(g_ctx, 0, 1, &g_sharp_cb);
        ID3D11DeviceContext_Dispatch(g_ctx, (g_w + 7) / 8, (g_h + 7) / 8, 1);
        ID3D11ShaderResourceView *nsrv = NULL;
        ID3D11UnorderedAccessView *nuav = NULL;
        ID3D11DeviceContext_CSSetShaderResources(g_ctx, 0, 1, &nsrv);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(g_ctx, 0, 1, &nuav, NULL);
        ID3D11DeviceContext_CSSetShader(g_ctx, ocs, onci ? oci : NULL, onci);
        ID3D11DeviceContext_CSSetShaderResources(g_ctx, 0, 1, &osrv);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(g_ctx, 0, 1, &ouav, NULL);
        ID3D11DeviceContext_CSSetConstantBuffers(g_ctx, 0, 1, &ocb);
        if (ocs) ID3D11ComputeShader_Release(ocs);
        for (UINT i = 0; i < onci; i++) if (oci[i]) ID3D11ClassInstance_Release(oci[i]);
        if (osrv) ID3D11ShaderResourceView_Release(osrv);
        if (ouav) ID3D11UnorderedAccessView_Release(ouav);
        if (ocb) ID3D11Buffer_Release(ocb);
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_srout[g_n % 2], (ID3D11Resource *)g_sharp);   // frame generation's HUD-less image
        ID3D11DeviceContext_CopyResource(g_ctx, backbuffer, (ID3D11Resource *)g_sharp);
        g_sharp_frames++;
    } else ID3D11DeviceContext_CopyResource(g_ctx, backbuffer, (ID3D11Resource *)g_srout[g_n % 2]);
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
    g_present_tid = GetCurrentThreadId();
    LARGE_INTEGER t0, tp;
    QueryPerformanceCounter(&t0);
    wait_previous();
    tq(TQ_P0);
    if (!g_hud_done) ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_hud, backbuffer);   // no HUD-less point this frame
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_final, backbuffer);
    tq(TQ_END);
    if (!g_tq_failed && g_tq[g_n % 8][TQ_DIS]) ID3D11DeviceContext_End(g_ctx, (ID3D11Asynchronous *)g_tq[g_n % 8][TQ_DIS]);
    if (g_n >= 6) tq_read(g_n - 6);
    ID3D11DeviceContext4_Signal(g_ctx4, g_fence, 2ull * g_n + 2);
    ID3D11DeviceContext_Flush(g_ctx);
    g_sh->reset = !g_hud_done;
    // a frame without the scene (menu, pause, loading): the next one counts the camera as not moved, not as moved
    // since the last scene frame, seconds ago (one frame of huge wrong motion vectors: a smear after every menu)
    if (!g_hud_done) g_have_prev = 0;
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

// The game's GPU waits on the helper's fences (wait_previous, the DLSS pickup in fgb_hudless) end only when the helper
// signals them. With the helper gone (quit mid-frame, crashed) they would hang the game's GPU (driver reset): the fences
// are signalled past any frame here, through a D3D12 device on the game's GPU (a D3D11 fence has no CPU Signal).
static void release_gpu_waits(void) {
    static const GUID iid_dxgi_device = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
    static const GUID iid_d3d12_device = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
    static const GUID iid_d3d12_fence = {0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};
    HMODULE m = LoadLibraryExA("d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    PFN_D3D12_CREATE_DEVICE create = m ? (PFN_D3D12_CREATE_DEVICE)(void *)GetProcAddress(m, "D3D12CreateDevice") : NULL;
    IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; ID3D12Device *d = NULL;
    int n = 0;
    if (create && SUCCEEDED(ID3D11Device_QueryInterface(g_dev, &iid_dxgi_device, (void **)&xd)) && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)) &&
        SUCCEEDED(create((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0, &iid_d3d12_device, (void **)&d))) {
        HANDLE fences[2] = {g_handles[1], g_handles[2]};   // fsr, fdone
        for (int i = 0; i < 2; i++) {
            ID3D12Fence *f = NULL;
            if (fences[i] && SUCCEEDED(ID3D12Device_OpenSharedHandle(d, fences[i], &iid_d3d12_fence, (void **)&f))) {
                n += SUCCEEDED(ID3D12Fence_Signal(f, 1ull << 62));
                ID3D12Fence_Release(f);
            }
        }
        ID3D12Device_Release(d);
    }
    if (ad) IDXGIAdapter_Release(ad);
    if (xd) IDXGIDevice_Release(xd);
    g_log("frame bridge: %d of 2 helper fences released for the game's GPU", n);
}

// ---- Watchdog (AGENT_LOG 108: the game froze leaving a battle, Windows closed it as not responding). A helper that stops
// finishing frames leaves the game's GPU waiting for it for good (wait_previous), and the game's next Present then blocks:
// nothing on the main thread can notice. This thread does: a frame handed over and nothing finished for 3 s -> the
// helper is stopped and its fences released, the game's GPU goes on, and its next Present finds the helper gone (bridge
// off, fallback). It logs where the main thread is then, and once per stall when the game stops presenting for 5 s.
static void log_thread_stack(DWORD tid, const char *what) {
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
    if (!t) return;
    static unsigned stack[4096];
    unsigned n = 0, eip = 0;
    CONTEXT c = {.ContextFlags = CONTEXT_CONTROL};
    // only a copy while it is suspended: it may hold the loader or heap lock (module lookups and logging come after)
    if (SuspendThread(t) != (DWORD)-1) {
        MEMORY_BASIC_INFORMATION mb;
        if (GetThreadContext(t, &c) && VirtualQuery((void *)(UINT_PTR)c.Esp, &mb, sizeof mb) && mb.State == MEM_COMMIT) {
            UINT_PTR end = (UINT_PTR)mb.BaseAddress + mb.RegionSize;
            n = (unsigned)((end - c.Esp) / 4 < 4096 ? (end - c.Esp) / 4 : 4096);
            memcpy(stack, (void *)(UINT_PTR)c.Esp, n * 4);
            eip = c.Eip;
        }
        ResumeThread(t);
    }
    CloseHandle(t);
    char out[1200];
    int len = 0, found = 0;
    out[0] = 0;
    for (unsigned i = 0; i <= n && found < 24 && len < (int)sizeof out - 80; i++) {
        unsigned v = i ? stack[i - 1] : eip;   // first where it is, then module pointers on its stack (likely return addresses)
        HMODULE m = NULL;
        char name[MAX_PATH];
        if (v < 0x10000 || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               (LPCSTR)(UINT_PTR)v, &m) || !GetModuleFileNameA(m, name, MAX_PATH))
            continue;
        const char *b = strrchr(name, '\\');
        len += snprintf(out + len, sizeof out - len, " %s+0x%X", b ? b + 1 : name, v - (unsigned)(UINT_PTR)m);
        found++;
    }
    g_log("frame bridge watchdog: %s; main thread at%s", what, n ? out : " ? (no context)");
}
static DWORD WINAPI watchdog(void *arg) {
    (void)arg;
    uint32_t done0 = 0, n0 = 0;
    int stuck = 0, still = 0;
    while (g_live && WaitForSingleObject(g_helper, 250) == WAIT_TIMEOUT) {   // every 250 ms while the helper runs
        uint32_t f = __atomic_load_n(&g_sh->frames, __ATOMIC_ACQUIRE), d = __atomic_load_n(&g_sh->done, __ATOMIC_ACQUIRE);
        uint32_t n = __atomic_load_n(&g_n, __ATOMIC_RELAXED);
        if (f == FGB_QUIT) break;
        stuck = f > d && d == done0 ? stuck + 1 : 0;   // counted in samples: a sleep / resume of the PC is one sample
        still = n == n0 ? still + 1 : 0;
        done0 = d; n0 = n;
        if (still == 20 && g_present_tid) log_thread_stack(g_present_tid, "the game has not presented for 5 s");
        if (stuck == 12) {
            char what[160];
            snprintf(what, sizeof what, "the helper has finished nothing for 3 s (frames handed over %u, finished %u, DLSS SR %u/%u) - helper stopped, DLSS off",
                     f, d, g_sh->sr_frames, g_sh->sr_done);
            TerminateProcess(g_helper, 1);
            WaitForSingleObject(g_helper, 1000);
            if (g_present_tid) log_thread_stack(g_present_tid, what);   // where it waits, before it can go on
            else g_log("frame bridge watchdog: %s", what);
            release_gpu_waits();   // the game's GPU goes on; it may have been all that held the main thread
            break;
        }
    }
    return 0;
}

void fgb_stop(void) {
    static int stopped;
    if (!g_sh || stopped) return;
    stopped = 1;
    __atomic_store_n(&g_sh->frames, FGB_QUIT, __ATOMIC_RELEASE);
    SetEvent(g_wake);
    SetEvent(g_wake2);
    if (g_helper && WaitForSingleObject(g_helper, 5000) == WAIT_TIMEOUT) TerminateProcess(g_helper, 1);   // frozen: not left on the window
    g_live = 0;
    release_gpu_waits();
}
// n bytes of a loaded DLL as its file on disk has them (addr inside the DLL), and the file's own base address (the
// loaded header's ImageBase is rewritten to where the DLL is loaded). 0 if anything does not fit.
static int file_bytes(const void *addr, void *out, DWORD n, HMODULE *mod, DWORD *file_base) {
    HMODULE m = NULL;
    wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &m) ||
        !GetModuleFileNameW(m, path, MAX_PATH))
        return 0;
    const unsigned char *b = (const unsigned char *)m;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    DWORD rva = (DWORD)((const unsigned char *)addr - b), off = 0, got = 0, got2 = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (rva >= sec[i].VirtualAddress && rva + n <= sec[i].VirtualAddress + sec[i].SizeOfRawData)
            off = rva - sec[i].VirtualAddress + sec[i].PointerToRawData;
    HANDLE f = off ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) : INVALID_HANDLE_VALUE;
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (SetFilePointer(f, (LONG)off, NULL, FILE_BEGIN) != off || !ReadFile(f, out, n, &got, NULL)) got = 0;
    DWORD base_at = (DWORD)((const unsigned char *)&nt->OptionalHeader.ImageBase - b);
    if (SetFilePointer(f, (LONG)base_at, NULL, FILE_BEGIN) != base_at || !ReadFile(f, file_base, 4, &got2, NULL)) got2 = 0;
    CloseHandle(f);
    *mod = m;
    return got == n && got2 == 4;
}

// The function a vtable slot held before anyone hooked it: the vtable entry as the DLL file has it, moved to where the
// DLL is loaded. NULL if anything does not fit.
void *fgb_vt_original(void **vt, size_t slot) {
    HMODULE m = NULL, fm = NULL;
    DWORD v = 0, file_base = 0;
    if (!file_bytes(&vt[slot], &v, 4, &m, &file_base)) return NULL;
    UINT_PTR fn = (UINT_PTR)v - file_base + (UINT_PTR)m;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)fn, &fm) || fm != m)
        return NULL;
    return (void *)fn;
}

// A Windows function as it is in its DLL, even when another mod has put a jump over its first bytes (inline hook;
// 6.3.2 test: dxgi's Present started with E9, and calling it led through the UI mod back into our Present hook). Works
// for the hot-patch start "mov edi,edi; push ebp; mov ebp,esp" (8B FF 55 8B EC, dxgi's Present and CreateSwapChain):
// those 5 bytes, then a jump to the untouched rest. fn itself if nothing is hooked; NULL if it does not fit.
void *fgb_unhooked(void *fn) {
    static const unsigned char hot[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
    unsigned char disk[8];
    HMODULE m = NULL;
    DWORD file_base = 0;
    if (fn && !memcmp(fn, hot, 5)) return fn;   // its start is intact: not hooked there
    if (!fn || !file_bytes(fn, disk, sizeof disk, &m, &file_base) || memcmp(disk, hot, 5)) return NULL;
    // more than the start replaced? The next instruction must be as in the file (only a few bytes: further on, addresses
    // differ from the file where Windows relocated them)
    if (memcmp((const unsigned char *)fn + 5, disk + 5, 3)) return NULL;
    unsigned char *t = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return NULL;
    memcpy(t, hot, 5);
    t[5] = 0xE9;
    *(int *)(t + 6) = (int)((const unsigned char *)fn + 5 - (t + 10));
    DWORD old;
    VirtualProtect(t, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    return t;
}

void fgb_test_kill_helper(void) { if (g_helper) TerminateProcess(g_helper, 1); }   // bridgetest killhelper

// ---- After the bridge, while the game's swapchain sits on a hidden window (the helper died, the back buffer changed
// size, or a new swapchain after a settings change): each frame is copied into a swapchain of our own, so the game does
// not look frozen. It is on a child window covering the game's client area: a swapchain on the game's window itself
// after the helper's is never shown (bridgetest killhelper / stophelper: flip or blit, presents succeed, the window keeps
// the helper's last frame). create: the factory's original CreateSwapChain (the game's factory slot is hooked), NULL =
// the factory's own.
static IDXGISwapChain *g_fb_sc;
static HWND g_fb_wnd;
static RECT g_fb_rc;
static D3D11_TEXTURE2D_DESC g_fb_src;
static unsigned g_fb_tries;
// the device of the frames shown: the game's back buffer's (a settings change may come with a new D3D11 device; copying
// its back buffer with the first device's context removed the device, 6.3.2 test: 7204 of 7205 presents 0x887A0005)
static ID3D11Device *g_fb_dev;
static ID3D11DeviceContext *g_fb_ctx;
IDXGISwapChain *fgb_fallback_sc(void) { return g_fb_sc; }
HWND fgb_fallback_window(void) { return g_fb_wnd; }
void fgb_fallback_close(void) {
    if (g_fb_sc) IDXGISwapChain_Release(g_fb_sc);
    if (g_fb_wnd) DestroyWindow(g_fb_wnd);
    if (g_fb_ctx) ID3D11DeviceContext_Release(g_fb_ctx);
    if (g_fb_dev) ID3D11Device_Release(g_fb_dev);
    g_fb_sc = NULL;
    g_fb_wnd = NULL;
    g_fb_ctx = NULL;
    g_fb_dev = NULL;
}
typedef HRESULT(STDMETHODCALLTYPE *fgb_create_sc_fn)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
static LRESULT CALLBACK fb_child_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return m == WM_NCHITTEST ? HTTRANSPARENT : DefWindowProcW(h, m, w, l);   // the mouse goes to the game's window under it
}
static IDXGISwapChain *fb_create(const D3D11_TEXTURE2D_DESC *s, fgb_create_sc_fn create) {
    static const GUID iid_dxgi_device = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
    static const GUID iid_factory = {0x7b7166ec, 0x21c7, 0x44ae, {0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69}};
    IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory *f = NULL; IDXGISwapChain *sc = NULL;
    HRESULT hr = ID3D11Device_QueryInterface(g_fb_dev, &iid_dxgi_device, (void **)&xd);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(xd, &ad);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(ad, &iid_factory, (void **)&f);
    if (SUCCEEDED(hr)) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = fb_child_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"fgb_fallback";
        RegisterClassW(&wc);
        GetClientRect(g_hwnd, &g_fb_rc);
        g_fb_wnd = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"fgb_fallback", L"", WS_CHILD | WS_VISIBLE, 0, 0, g_fb_rc.right, g_fb_rc.bottom,
                                   g_hwnd, NULL, wc.hInstance, NULL);
        if (!g_fb_wnd) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr)) {
        if (!create) create = f->lpVtbl->CreateSwapChain;
        // flip model (the window's other swapchain, the helper's, was flip too); its buffers take no _SRGB format
        DXGI_FORMAT ff = s->Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM
                       : s->Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_UNORM : s->Format;
        DXGI_SWAP_CHAIN_DESC d = {{s->Width, s->Height, {0, 1}, ff, 0, 0}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, g_fb_wnd, TRUE,
                                  DXGI_SWAP_EFFECT_FLIP_DISCARD, 0};
        hr = create(f, (IUnknown *)g_fb_dev, &d, &sc);
        if (FAILED(hr)) {   // a format flip model does not take: a blit swapchain
            d.BufferDesc.Format = s->Format; d.BufferCount = 1; d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            hr = create(f, (IUnknown *)g_fb_dev, &d, &sc);
        }
        if (SUCCEEDED(hr)) IDXGIFactory_MakeWindowAssociation(f, g_fb_wnd, DXGI_MWA_NO_WINDOW_CHANGES);   // the game runs its window
    }
    if (FAILED(hr)) {
        sc = NULL;
        if (g_fb_wnd) DestroyWindow(g_fb_wnd);
        g_fb_wnd = NULL;
        g_log("frame bridge: the mod's own swapchain on the game window failed (0x%08lx), retrying", hr);
    }
    if (f) IDXGIFactory_Release(f);
    if (ad) IDXGIAdapter_Release(ad);
    if (xd) IDXGIDevice_Release(xd);
    return sc;
}
// src: the game's back buffer. Returns the swapchain to present (the caller presents it), or NULL: not this frame.
IDXGISwapChain *fgb_fallback_frame(ID3D11Resource *src, fgb_create_sc_fn create) {
    static const GUID iid_resource = {0xdc8e63f3, 0xd12b, 0x4952, {0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d}};
    D3D11_TEXTURE2D_DESC s;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)src, &s);
    ID3D11Device *dev = NULL;
    ID3D11Resource_GetDevice(src, &dev);
    if (s.SampleDesc.Count != 1 || !dev) {
        if (dev) ID3D11Device_Release(dev);
        return NULL;
    }
    if (g_fb_sc && (s.Width != g_fb_src.Width || s.Height != g_fb_src.Height || s.Format != g_fb_src.Format || dev != g_fb_dev)) {
        if (dev != g_fb_dev) g_log("frame bridge: the game's frames come from another D3D11 device now - the mod's swapchain follows");
        fgb_fallback_close();
    }
    if (!g_fb_sc) {
        if (g_fb_tries++ % 120) { ID3D11Device_Release(dev); return NULL; }   // after a failure: again in 120 frames
        if (g_fb_dev) ID3D11Device_Release(g_fb_dev);
        if (g_fb_ctx) ID3D11DeviceContext_Release(g_fb_ctx);
        g_fb_dev = dev;   // the reference GetDevice gave
        ID3D11Device_GetImmediateContext(dev, &g_fb_ctx);
        dev = NULL;
        if (!(g_fb_sc = fb_create(&s, create))) return NULL;
        g_fb_src = s;
        g_fb_tries = 0;
        g_log("frame bridge: the game's frames now go to its window through the mod's own swapchain (%ux%u)", s.Width, s.Height);
    }
    if (dev) ID3D11Device_Release(dev);
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    if (rc.right != g_fb_rc.right || rc.bottom != g_fb_rc.bottom) {   // the game resized its window: the child follows
        g_fb_rc = rc;
        MoveWindow(g_fb_wnd, 0, 0, rc.right, rc.bottom, FALSE);
    }
    ID3D11Resource *bb = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(g_fb_sc, 0, &iid_resource, (void **)&bb))) return NULL;
    ID3D11DeviceContext_CopyResource(g_fb_ctx, bb, src);
    ID3D11Resource_Release(bb);
    return g_fb_sc;
}
