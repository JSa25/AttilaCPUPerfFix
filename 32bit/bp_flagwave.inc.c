// Part of bridgeprobe.c (included there, not compiled on its own): the flag wave: the prop shaders made from ours, the cloth simulation's frame, the VSSetShader / slot hooks.
// ---- flag wave (notes/banners.md): the game's two rigid prop vertex shaders are created from our copies (flagvs.h:
// identical for every model the game ships, flag cloth from the weight pack waves). flagtest.c (dlss/proof) checks them
// against the game's. Installed only when a loaded pack carries the switch file with a 1 in it (fwpack.c) and
// flag_wave is not 0; without such a pack none of this exists in the game: no shader, no hook, no buffer read.
#include "flagvs.h"
// the two shaders as bytecode, compiled by fwvs_gen.c at build time: compiling the HLSL here cost 264 ms of every
// player's game start and needed d3dcompiler_47.dll, which a stripped Windows or Proton may not have.
#if __has_include("flagvs_bc.h")
#include "flagvs_bc.h"          // next to a copy of this file in build/ (reshade_test.c's broken copies)
#else
#include "build/flagvs_bc.h"
#endif
static int g_fw_on;
static volatile LONG g_fw_swapped[3];
// Wind for the shadow shader: the game binds its wind_VS block (cb2) only for the main view's prop shader. VSSetShader
// hook: when the main view's shader is switched away from, its cb2 buffer is kept; when the shadow shader is set, that
// buffer is lent to slot 2 and the game's own slot-2 buffer goes back before any other shader is set (or as soon as the
// game sets slot 2 itself). The game draws the shadows first, so the shadows use the wind buffer's current contents.
// The copies of each shader that are remembered, fw_kind and fw_store: fw_kind.h (fwkind_test.c builds it too).
#define FW_HOLD(vs) ID3D11VertexShader_AddRef(vs)
#define FW_DROP(vs) ID3D11VertexShader_Release(vs)
#include "fw_kind.h"
static int g_fw_ab;
// The context the flag state below belongs to: the one that first set one of our shaders (they exist on the game's
// device only). The three hooks sit in d3d11's table, which every device's context shares, so another device's context
// (a video player's, an overlay's) comes through them too; it must not move g_fw_cur, take its buffer for the wind
// block or drop what was lent.
static ID3D11DeviceContext *g_fw_ctx;
static int g_fw_cur = -1, g_fw_lent;
static ID3D11Buffer *g_fw_wind, *g_fw_saved, *g_fw_stage;
static ID3D11Buffer *g_fw_cam;        // the game's camera block (cb0): without it fs_run simulates every flag at full detail
static unsigned g_fs_last_gather;     // the Present a flag was last gathered in: nothing on screen, nothing to simulate
static unsigned g_fw_mains, g_fw_logged, g_fw_copied;
static void(STDMETHODCALLTYPE *r_vsss)(ID3D11DeviceContext *, ID3D11VertexShader *, ID3D11ClassInstance *const *, UINT);
static void(STDMETHODCALLTYPE *r_vscb)(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
static void fw_unlend(void) {
    if (g_fw_saved) ID3D11Buffer_Release(g_fw_saved);
    g_fw_saved = NULL;
    g_fw_lent = 0;
}
// dev log: the game's wind values (first 3 times, every ~2000 switches away from the main view's prop shader)
static void fw_log_wind(ID3D11DeviceContext *c) {
    if (g_fw_logged >= 3 || !g_fw_wind || !g_log) return;
    if (!g_fw_stage) {
        D3D11_BUFFER_DESC bd;
        ID3D11Buffer_GetDesc(g_fw_wind, &bd);
        bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; bd.MiscFlags = 0; bd.StructureByteStride = 0;
        ID3D11Device *d;
        ID3D11DeviceContext_GetDevice(c, &d);
        if (FAILED(ID3D11Device_CreateBuffer(d, &bd, NULL, &g_fw_stage))) g_fw_stage = NULL, g_fw_logged = 3;
        ID3D11Device_Release(d);
        if (!g_fw_stage) return;
    }
    if (!g_fw_copied) { ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)g_fw_stage, (ID3D11Resource *)g_fw_wind); g_fw_copied = 1; return; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)g_fw_stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return;
    const float *f = m.pData;
    g_log("flag wave: game wind_VS direction %.3f %.3f, gust start %.2f, vegetation multiplier %.3f, h strength %.3f, v strength %.3f",
          f[0], f[1], f[2], f[3], f[4], f[5]);
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)g_fw_stage, 0);
    g_fw_copied = 0;
    g_fw_logged++;
}
// ---- the cloth simulation (flagsim.c, notes/banners.md): flag meshes are found by their marker vertex when the game
// creates its vertex buffers (CreateBuffer hook); every draw with our prop shader set is checked against them and a
// flag's instance records are gathered (a GPU copy); the first time one of our shaders is set in a new frame, the
// previous frame's flags are simulated (compute), and while our shaders are set the results are lent to VS slots t14 /
// t15 (the game's own views there go back before any other shader is set, as with the wind block).
#include "flagsim.c"   // included, not linked: reshade_test.c builds this file as source too
#include "fwpack.c"    // the pack switch: is a flag pack loaded at all
int diag_mods_text(const wchar_t *dir, char *text, int cap);   // diag.c: the launcher's mod list
static int g_fs_ok, g_fs_lent;
static unsigned g_fs_present;
static LARGE_INTEGER g_fs_qpc0, g_fs_qpc_last;
static ID3D11ShaderResourceView *g_fs_saved[2];
static unsigned long long g_fs_ticks, g_fs_draws, g_fs_flagdraws, g_fs_inst, g_fs_runs, g_fs_simmed;
static void(STDMETHODCALLTYPE *r_vssrv)(ID3D11DeviceContext *, UINT, UINT, ID3D11ShaderResourceView *const *);
static HRESULT(STDMETHODCALLTYPE *r_cbuf)(ID3D11Device *, const D3D11_BUFFER_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Buffer **);
static volatile LONG g_fs_found;
// This hook exists only with a flag pack loaded (g_fw_on, set once from the pack switch). The marker vertices tell
// which of that pack's meshes are cloth and how big; they no longer decide whether the flag code runs.
static HRESULT STDMETHODCALLTYPE w_cbuf(ID3D11Device *d, const D3D11_BUFFER_DESC *bd, const D3D11_SUBRESOURCE_DATA *sd, ID3D11Buffer **out) {
    HRESULT hr = r_cbuf(d, bd, sd, out);
    // every new buffer, of any kind: its address may have been a flag buffer's that the game has freed since (flagsim.c)
    if (SUCCEEDED(hr) && out && *out) fs_forget(*out);
    if (SUCCEEDED(hr) && out && *out && bd && sd && sd->pSysMem && (bd->BindFlags & D3D11_BIND_VERTEX_BUFFER)) {
        int f = fs_scan(*out, sd->pSysMem, bd->ByteWidth);
        if (f && InterlockedExchangeAdd(&g_fs_found, f) == 0 && g_log) g_log("flag sim: first flag meshes found in a new vertex buffer (%d)", f);
    }
    return hr;
}
static void fs_unlend(void) {
    for (int i = 0; i < 2; i++) if (g_fs_saved[i]) ID3D11ShaderResourceView_Release(g_fs_saved[i]), g_fs_saved[i] = NULL;
    g_fs_lent = 0;
}
static void fs_frame(ID3D11DeviceContext *c) {
    LARGE_INTEGER now, fq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&fq);
    if (!g_fs_ok) {
        ID3D11Device *d = NULL;
        ID3D11DeviceContext_GetDevice(c, &d);
        char err[600] = "";
        g_fs_ok = d && fs_init(d, err, sizeof err) ? 1 : -1;
        if (d) ID3D11Device_Release(d);
        if (g_log) g_log("flag sim %s%s%s", g_fs_ok > 0 ? "set up" : "NOT set up: ", g_fs_ok > 0 ? "" : err, "");
        g_fs_qpc0 = g_fs_qpc_last = now;
    }
    if (g_fs_ok < 0) return;
    float dt = (float)(now.QuadPart - g_fs_qpc_last.QuadPart) / (float)fq.QuadPart, t = (float)(now.QuadPart - g_fs_qpc0.QuadPart) / (float)fq.QuadPart;
    g_fs_qpc_last = now;
    g_fs_simmed += fs_run(c, g_fw_wind, g_fw_cam, dt, t);
    static unsigned st[2], st_logged[2];   // GPU running totals: new cloths started, flags left out of the shader's lookup
    fs_stats(c, st);
    g_fs_runs++;
    if (g_fs_runs % 1800 == 0 && g_log) {
        double ms = (double)g_fs_ticks / (double)fq.QuadPart * 1000.0 / 1800;
        g_log("flag sim: %d flag meshes known; per frame %.0f prop draws checked, %.1f flag draws, %.1f flag instances simulated; "
              "our CPU time %.3f ms per frame (checks, copies, dispatch); %.2f new cloths and %.2f flags left out of the lookup per frame",
              fs_models(), (double)g_fs_draws / 1800, (double)g_fs_flagdraws / 1800, (double)g_fs_simmed / 1800, ms,
              (double)(st[0] - st_logged[0]) / 1800, (double)(st[1] - st_logged[1]) / 1800);
        // The cloth budget, per pass and per frame. "flag instances simulated" above cannot answer this: fs_run returns
        // min(total, FS_GATHER), so a saturated battle reads exactly FS_GATHER and says nothing about how far over the
        // demand went. The main view fills the list first and the shadow pass takes the remainder, so only main-view
        // frames over the budget cost a flag ON SCREEN its cloth; shadow frames cost only its shadow.
        struct fs_demand dm;
        fs_demand_stats(&dm);
        if (dm.frames) {
            char h0[FS_DM_BUCKETS * 12 + 1], h1[sizeof h0];
            int n0 = 0, n1 = 0;
            for (int i = 0; i < FS_DM_BUCKETS; i++) {
                n0 += _snprintf(h0 + n0, sizeof h0 - n0, "%s%u", i ? "," : "", dm.hist[0][i]);
                n1 += _snprintf(h1 + n1, sizeof h1 - n1, "%s%u", i ? "," : "", dm.hist[1][i]);
            }
            // the shadow pass's demand is still reported, but it takes no gather slots (flagsim.c fs_shadow_gather),
            // so it costs nothing: it is here to show what the main view is being spared
            g_log("flag budget (%u frames, %d slots a pass, buckets of %d from 0): main view asked peak %u, over budget in "
                  "%u frames (%.2f%%), most it ever got %u; shadow asked peak %u (not gathered, costs no slots); "
                  "%u instances dropped so far",
                  dm.frames, FS_GATHER, FS_DM_STEP, dm.peak[0], dm.over[0], 100.0 * dm.over[0] / dm.frames,
                  dm.peak_got_main, dm.peak[1], dm.dropped);
            g_log("flag budget histogram, frames by instances asked for - main view: %s", h0);
            g_log("flag budget histogram, frames by instances asked for - shadow:    %s", h1);
        }
        st_logged[0] = st[0]; st_logged[1] = st[1];
        g_fs_draws = g_fs_flagdraws = g_fs_inst = g_fs_simmed = 0;
        g_fs_ticks = 0;
    }
}
// a draw with one of our shaders set: a flag mesh's instances are gathered for the next frame's simulation
static void fs_draw(ID3D11DeviceContext *c, UINT inst, INT bv, UINT sinst) {
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    ID3D11Buffer *vb[2] = {0};
    UINT st[2] = {0}, of[2] = {0};
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, 2, vb, st, of);
    const float *prm = vb[0] ? fs_is_flag(vb[0], of[0] + (UINT)bv * st[0]) : NULL;
    if (prm && vb[1] && st[1] == 112) {
        // Which list the draw goes in. The shadow pass draws first and each flag once per cascade, so left at 0 its
        // draws fill all FS_GATHER slots of the main view's list and the flags on screen get no cloth (flagsim.c).
        fs_pass = g_fw_cur == 1;
        fs_gather(c, vb[1], of[1] + sinst * 112, inst, prm);
        g_fs_flagdraws++;
        g_fs_inst += inst;
        g_fs_last_gather = g_presents;
    }
    g_fs_draws++;
    if (vb[0]) ID3D11Buffer_Release(vb[0]);
    if (vb[1]) ID3D11Buffer_Release(vb[1]);
    QueryPerformanceCounter(&b);
    g_fs_ticks += (unsigned long long)(b.QuadPart - a.QuadPart);
}
// flagwave_ab.txt (FPS test): the flag wave on in even 10 s windows of UTC time, off in odd (the same windows as the other
// fixes' A/B, so fps_log.csv's seconds can be split); off = the game's own prop shaders, no simulation, no gathering
static int fw_ab_on(void) {
    if (!g_fw_ab) return 1;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int on = !((((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 100000000ULL) & 1);
    static int last = -1;
    if (on != last && g_log) g_log("flag wave A/B: %s", on ? "ON" : "off");
    last = on;
    return on;
}
// Ctrl+Shift+F while the game's window is in front: the flag wave off / on, once per press. heapfix.ini's flag_wave
// decides whether any of this is installed; this is the in-game switch, for looking at one scene both ways.
static volatile LONG g_fw_off;
static void fw_hotkey(void) {
    if (!g_fw_on) return;
    static int was;
    int down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('F') & 0x8000);
    if (down && !was) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid == GetCurrentProcessId() && g_log) g_log("flag wave %s (Ctrl+Shift+F)", InterlockedXor(&g_fw_off, 1) ? "ON" : "OFF");
    }
    was = down;
}
static void STDMETHODCALLTYPE w_vsss(ID3D11DeviceContext *c, ID3D11VertexShader *vs, ID3D11ClassInstance *const *ci, UINT n) {
    int k = fw_kind(vs);
    if (k >= 0 && !g_fw_ctx) g_fw_ctx = c;
    if (c != g_fw_ctx) { r_vsss(c, vs, ci, n); return; }   // another device's context, or none of ours set yet: nothing to do
    // Switched off in game, or the A/B is in an off window: the game's own shader where we have it (A/B), and k = -1
    // skips the wind block, the cloth frame, the lending and fs_draw, so nothing below runs.
    if (k >= 0 && (g_fw_off || (g_fw_ab && !fw_ab_on()))) {
        for (int i = 0; i < FW_NVS; i++) if (g_fw_vs[k][i] == vs && g_fw_own[k][i]) { vs = g_fw_own[k][i]; break; }
        k = -1;
    }
    if ((g_fw_cur == 0 || g_fw_cur == 2) && k != g_fw_cur) {   // leaving a main view prop shader: its wind block is in slot 2 (the twin: 1)
        ID3D11Buffer *b = NULL;
        ID3D11DeviceContext_VSGetConstantBuffers(c, g_fw_cur == 2 ? 1 : 2, 1, &b);
        if (b && b != g_fw_wind) { if (g_fw_wind) ID3D11Buffer_Release(g_fw_wind); g_fw_wind = b; if (g_fw_stage) ID3D11Buffer_Release(g_fw_stage), g_fw_stage = NULL, g_fw_copied = 0; }
        else if (b) ID3D11Buffer_Release(b);
        b = NULL;   // and its camera block (cb0): flags far from the camera simulate less often
        ID3D11DeviceContext_VSGetConstantBuffers(c, 0, 1, &b);
        if (b && b != g_fw_cam) { if (g_fw_cam) ID3D11Buffer_Release(g_fw_cam); g_fw_cam = b; }
        else if (b) ID3D11Buffer_Release(b);
        if (++g_fw_mains % 2000 == 1 || g_fw_copied) fw_log_wind(c);
    }
    if (g_fw_lent && k != 1) {
        r_vscb(c, 2, 1, &g_fw_saved);
        fw_unlend();
    }
    if (g_fs_lent && k < 0) {
        r_vssrv(c, 14, 2, g_fs_saved);
        fs_unlend();
    }
    // The first of our shaders this frame: simulate the last frame's flags. Once the cloth is set up, only while flags
    // are actually being drawn - with none gathered for 12 Presents there is nothing on screen to simulate.
    if (k >= 0 && g_presents != g_fs_present && (g_fs_ok == 0 || g_presents - g_fs_last_gather <= 12)) {
        g_fs_present = g_presents;
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        if (g_fs_lent) { r_vssrv(c, 14, 2, g_fs_saved); fs_unlend(); }   // our views off the VS before they become UAVs
        fs_frame(c);
        QueryPerformanceCounter(&b);
        g_fs_ticks += (unsigned long long)(b.QuadPart - a.QuadPart);
    }
    r_vsss(c, vs, ci, n);
    if (k == 1 && g_fw_wind && !g_fw_lent) {
        ID3D11DeviceContext_VSGetConstantBuffers(c, 2, 1, &g_fw_saved);
        r_vscb(c, 2, 1, &g_fw_wind);
        g_fw_lent = 1;
    }
    if (k >= 0 && g_fs_ok > 0 && !g_fs_lent) {
        ID3D11ShaderResourceView *ours[2];
        fs_vs_views(ours);
        ID3D11DeviceContext_VSGetShaderResources(c, 14, 2, g_fs_saved);
        r_vssrv(c, 14, 2, ours);
        g_fs_lent = 1;
    }
    g_fw_cur = k;
}
static void STDMETHODCALLTYPE w_vscb(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b) {
    if (g_fw_lent && c == g_fw_ctx && start <= 2 && start + n > 2) fw_unlend();   // the game sets slot 2 itself: nothing to give back
    r_vscb(c, start, n, b);
}
static void STDMETHODCALLTYPE w_vssrv(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11ShaderResourceView *const *v) {
    if (g_fs_lent && c == g_fw_ctx && start <= 15 && start + n > 14) fs_unlend();   // the game sets t14 / t15 itself
    r_vssrv(c, start, n, v);
}
