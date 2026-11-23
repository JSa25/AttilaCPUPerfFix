# Writes src/flagwave64.c: the 32-bit Flag Wave plugin (Flagwave/dll/flagwave.c, unchanged) as a part of the engine fix
# (no entry of its own: perffix64.c gives it the device and the frame), with the changes Sol Invictus's 64-bit loader
# needs, plus src/flagwave64_tail.inc.
# usage: gen_flagwave64.py <repo root> [--without=N --out=<file>]
#   --without=N leaves change N (1, 2, 3, 5, 10 or 11) out and writes to <file>: the negative controls of flagwave64_test
#   (the test must fail on such a build, which shows that it checks what the change is for).
# The loader's behaviour named below was first read from attila64.exe 1.3.1 and then checked against its published
# source, loader 1.3.6 (attila64/loader/dx11_device.inc).
import os, sys
root = sys.argv[1]
# the dev tree (Flagwave/dll/, x64Review/perffix64/) or the public one, where the plugin source is published as
# 64bit/src/flagwave.c because only the 64-bit build reads it; this file is in <tree>/tools
P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fw = root + '/Flagwave/dll/flagwave.c'
if not os.path.exists(fw): fw = P + '/src/flagwave.c'
without = 0
out = P + '/src/flagwave64.c'
for a in sys.argv[2:]:
    if a.startswith('--without='): without = int(a.split('=')[1])
    elif a.startswith('--out='): out = a.split('=', 1)[1]
src = open(fw, encoding='utf-8').read()

def sub(n, a, b):
    global src
    assert src.count(a) == 1, a
    if n != without: src = src.replace(a, b)

# 1. the 64-bit game also makes the sm_4_1 main-view prop shader: same inputs, constants and t0 as sm_3_0 / sm_4_0
#    (55ca645c); it differs only in its target (vs_4_1) and one load's swizzle
sub(1, 'int k = h == 0x55ca645c65c0fbe6ull ? 0 :',
    'int k = h == 0x55ca645c65c0fbe6ull || h == 0x30a8b1615efd57cbull ? 0 :')
# 2. the loader clears VS t0-t15 in one call (dx_ctx_clear_res; it empties its own record of those slots too) and sets
#    a shader only when it changes (dx_ctx_vs): with ours still set, the views are lent again at once (the game's
#    shaders never use t14 / t15)
sub(2, '''    if (g_fs_lent && start <= 15 && start + n > 14) fs_unlend();
    r_vssrv(c, start, n, v);
''', '''    int hit = start <= 15 && start + n > 14;
    if (g_fs_lent && hit) fs_unlend();
    r_vssrv(c, start, n, v);
    if (hit && g_cur >= 0 && g_fs_ok > 0 && !g_fs_lent) {
        ID3D11ShaderResourceView *ours[2];
        fs_vs_views(ours);
        ID3D11DeviceContext_VSGetShaderResources(c, 14, 2, g_srv_saved);
        r_vssrv(c, 14, 2, ours);
        g_fs_lent = 1;
    }
''')
# 3. no hot patch on x64: d3d11 has no "mov edi, edi" point, and a rel32 jump may not reach our DLL
sub(3, '''static int hot_patch(void *fn) {
    if (g_hot >= 2''', '''static int hot_patch(void *fn) {
    if (sizeof(void *) == 8) return 0;
    if (g_hot >= 2''')
# 10. the hooks go into the table of the context the game was GIVEN, not the device's own. With the upscaling add-on
#     in, that is ctxwrap64's wrapper, whose table is ours and stays as we leave it. x64 d3d11 puts its own functions
#     back into the real context's table many times a second (PORTING.md "Not known", settled 2026-11-22): in the
#     20:02 run of that day the draw hook was gone after the first battle frame and nothing called it again - 18 flag
#     meshes found, one gather, then no cloth for the rest of the battle. Without the add-on there is no wrapper and
#     the real context comes through here, as before.
sub(10, '''    if (!g_ctx) {
        ID3D11Device_GetImmediateContext(d, &g_ctx);
        if (g_ctx) hook_context(g_ctx);
    }''', '''    if (!g_ctx) {
        if (g_ctx_given) { g_ctx = g_ctx_given; ID3D11DeviceContext_AddRef(g_ctx); }
        else ID3D11Device_GetImmediateContext(d, &g_ctx);
        if (g_ctx) hook_context(g_ctx);
    }''')
# 11. On that table the 16 state hooks must NOT go in. They exist only to take the draw slot back from d3d11, which
#     cannot reach this table at all - and the add-on hooks seven of those slots itself and puts its own hook back on
#     top at every render stage, treating a hook of its own module (us, now) as a stale pointer of its own rather
#     than another mod's (heapfix/bp_dlss.inc.c ctx_slot, "Our own module's hooks are not foreign"). Each such slot
#     then calls the other for ever: the game died at the first render stage, twice, with a stack overflow reported
#     as 0xC0000005 in ntdll (2026-11-23 01:09). The draw slot is safe: mp_hook does take a hook of ours for another
#     mod's and leaves it to us ("- left to it" in perffix64.log). Keep it that way - a new state hook here would
#     bring the crash back.
sub(11, '    if (!g_hot) hook_state_calls(vt);   // only for the vtable hook (the hot patch needs no help)',
    '    if (!g_hot && !g_ctx_given) hook_state_calls(vt);   // not on the add-on\'s own table: it hooks some of those slots itself (change 11)')
# 5. Present takes our views off the VS for the simulation, and they come back when the game next sets one of our
#    shaders. The 32-bit game sets its shaders for every draw group; the loader skips a set of the shader that is set
#    already (dx_ctx_vs), so one of ours can stay set across a Present with nothing that would lend the views again.
#    A draw with our shader set and the views not lent lends them first.
sub(5, '''static void fw_draw(ID3D11DeviceContext *c, UINT inst, INT bv, UINT sinst) {
    if (g_cur < 0 || g_fs_ok <= 0 || c != g_ctx) return;
''', '''static void fw_draw(ID3D11DeviceContext *c, UINT inst, INT bv, UINT sinst) {
    if (g_cur < 0 || g_fs_ok <= 0 || c != g_ctx) return;
    if (!g_fs_lent) {
        ID3D11ShaderResourceView *ours[2];
        fs_vs_views(ours);
        ID3D11DeviceContext_VSGetShaderResources(c, 14, 2, g_srv_saved);
        r_vssrv(c, 14, 2, ours);
        g_fs_lent = 1;
    }
''')

# 6-8: what the 32-bit engine fix's flag code (heapfix/bp_flagwave.inc.c, bp_create.inc.c, fw_kind.h) gained after
#    Flagwave/dll/flagwave.c stopped changing on 2026-11-13; ported 2026-11-22. (The pack switch is in the tail.)
# 6. every new buffer first forgets what was known at its address (audit finding 1, 2026-11-21): the game frees its
#    buffers and the next one made can get a freed flag buffer's address, whose props would then be taken for flags
sub(6, '''    HRESULT hr = r_cbuf(d, bd, sd, out);
    if (SUCCEEDED(hr) && out && *out && bd && sd''', '''    HRESULT hr = r_cbuf(d, bd, sd, out);
    if (SUCCEEDED(hr) && out && *out) fs_forget(*out);   // its address may have been a flag buffer's that the game has freed since
    if (SUCCEEDED(hr) && out && *out && bd && sd''')
# 7. the main view's twin, 0cae34c7 (dx11_sm_3_0_rigidterrainblend main: 55ca645c with constant blocks 1 and 2
#    swapped, flagvs.h FW_TWIN): made from ours as well, its wind block is cb1
sub(7, 'static ID3DBlob *g_blob[2];', 'static ID3DBlob *g_blob[3];')
sub(7, 'static ID3D11VertexShader *g_vs[2][FW_NVS], *g_own[2][FW_NVS];\nstatic volatile LONG g_made[2];',
    'static ID3D11VertexShader *g_vs[3][FW_NVS], *g_own[3][FW_NVS];\nstatic volatile LONG g_made[3];\n'
    'static const char *const FW_KN[3] = {"main view", "shadow", "main view twin"};')
sub(7, '''    for (int k = 0; k < 2; k++)
        for (int i = 0; i < FW_NVS; i++)
            if (vs && g_vs[k][i] == vs) return k;''', '''    for (int k = 0; k < 3; k++)
        for (int i = 0; i < FW_NVS; i++)
            if (vs && g_vs[k][i] == vs) return k;''')
sub(7, ': h == 0xeb3006be08a96700ull ? 1 : -1;', ': h == 0xeb3006be08a96700ull ? 1 : h == 0x0cae34c7869ca505ull ? 2 : -1;')
sub(7, 'wlog("our %s shader refused (0x%08lx): the game\'s own", k ? "shadow" : "main view",',
    'wlog("our %s shader refused (0x%08lx): the game\'s own", FW_KN[k],')
sub(7, 'if (nth == 1) wlog("%s shader made from ours", k ? "shadow" : "main view");',
    'if (nth == 1) wlog("%s shader made from ours", FW_KN[k]);')
sub(7, 'wlog("%s shader made from ours %ld times%s", k ? "shadow" : "main view", nth,',
    'wlog("%s shader made from ours %ld times%s", FW_KN[k], nth,')
sub(7, '''    if (g_cur == 0 && k != 0) {   // leaving the main view's prop shader: its wind block (cb2) is the one to keep
        ID3D11Buffer *b = NULL;
        ID3D11DeviceContext_VSGetConstantBuffers(c, 2, 1, &b);''', '''    if ((g_cur == 0 || g_cur == 2) && k != g_cur) {   // leaving a main view prop shader: its wind block (cb2; the twin's cb1) is the one to keep
        ID3D11Buffer *b = NULL;
        ID3D11DeviceContext_VSGetConstantBuffers(c, g_cur == 2 ? 1 : 2, 1, &b);''')
sub(7, '"%.2f new cloths and %.2f flags left out of the lookup per frame; our shaders made %ld (main view) / %ld (shadow) times; "',
    '"%.2f new cloths and %.2f flags left out of the lookup per frame; our shaders made %ld (main view) / %ld (shadow) / %ld (twin) times; "')
sub(7, 'g_made[0], g_made[1], g_fr_flag,', 'g_made[0], g_made[1], g_made[2], g_fr_flag,')
sub(7, 'if (!g_ctx || !(g_made[0] | g_made[1]) ||', 'if (!g_ctx || !(g_made[0] | g_made[1] | g_made[2]) ||')
sub(7, '''    static const char *const entry[2] = {"main_vs", "shadow_vs"};
    for (int k = 0; k < 2; k++) {
        ID3DBlob *err = NULL;
        if (!comp || FAILED(comp(flag_hlsl, sizeof flag_hlsl - 1, "flagvs", NULL, NULL, entry[k],''', '''    static const char *const entry[3] = {"main_vs", "shadow_vs", "main_vs"};
    static const D3D_SHADER_MACRO twin[2] = {{"FW_TWIN", "1"}, {NULL, NULL}};   // as heapfix/fwvs_gen.c
    for (int k = 0; k < 3; k++) {
        ID3DBlob *err = NULL;
        if (!comp || FAILED(comp(flag_hlsl, sizeof flag_hlsl - 1, "flagvs", k == 2 ? twin : NULL, NULL, entry[k],''')
sub(7, 'g_blob[0] = g_blob[1] = NULL;', 'g_blob[0] = g_blob[1] = g_blob[2] = NULL;')
sub(7, '*made = (unsigned)(g_made[0] + g_made[1]);', '*made = (unsigned)(g_made[0] + g_made[1] + g_made[2]);')
# 8. the hooks sit in d3d11's table, which every device's context shares: another device's context (an overlay's, a
#    video's) passes through untouched (no wind block taken from it, nothing lent to it or taken back from it); and a
#    reference is held on each remembered shader, so that one the game released cannot hand its address to a new
#    object that would then be taken for ours (fw_kind.h, audit S1-S3)
sub(8, 'static void STDMETHODCALLTYPE w_vsss(ID3D11DeviceContext *c, ID3D11VertexShader *vs, ID3D11ClassInstance *const *ci, UINT n) {\n',
    'static void STDMETHODCALLTYPE w_vsss(ID3D11DeviceContext *c, ID3D11VertexShader *vs, ID3D11ClassInstance *const *ci, UINT n) {\n'
    '    if (c != g_ctx) { r_vsss(c, vs, ci, n); return; }   // not the context of the device our shaders were made on\n')
sub(8, 'static void STDMETHODCALLTYPE w_vscb(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b) {\n',
    'static void STDMETHODCALLTYPE w_vscb(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11Buffer *const *b) {\n'
    '    if (c != g_ctx) { r_vscb(c, start, n, b); return; }\n')
sub(8, 'static void STDMETHODCALLTYPE w_vssrv(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11ShaderResourceView *const *v) {\n',
    'static void STDMETHODCALLTYPE w_vssrv(ID3D11DeviceContext *c, UINT start, UINT n, ID3D11ShaderResourceView *const *v) {\n'
    '    if (c != g_ctx) { r_vssrv(c, start, n, v); return; }\n')
# 9. every copy of our shaders is remembered, in a table by address (2026-11-22 game run: the 64-bit game made the main
#    view's prop shader 492 times in one battle; the ring of the last 64 forgot most of them, so their draws - with our
#    shader, as w_cvs had made it - got no cloth views and no gathering: every flag but one stood still, 0 flags
#    simulated). A reference is held on each (change 8's part): a released shader cannot hand its address to a new one.
sub(9, 'static ID3D11VertexShader *g_vs[3][FW_NVS], *g_own[3][FW_NVS];', '''#define FW_NH 8192   // table of our shader objects (all kinds), a power of two; filled to 3/4 at most
static struct { ID3D11VertexShader *vs, *own; int k; } g_fwh[FW_NH];   // ours (held) and, for the off switch, the game's own
static LONG g_fwh_n;
static SRWLOCK g_fwh_lock = SRWLOCK_INIT;   // copies are made on loading threads, looked up on the drawing thread
static unsigned fwh_at(const void *p) { return (unsigned)(((uintptr_t)p >> 4) * 2654435761u) & (FW_NH - 1); }
static int fwh_find(const void *vs) {   // its slot, -1 if it is not one of ours
    int r = -1;
    if (!vs) return r;
    AcquireSRWLockShared(&g_fwh_lock);
    for (unsigned h = fwh_at(vs), n = 0; n < FW_NH && g_fwh[h].vs; n++, h = (h + 1) & (FW_NH - 1))
        if (g_fwh[h].vs == vs) { r = (int)h; break; }
    ReleaseSRWLockShared(&g_fwh_lock);
    return r;   // a slot, once filled, keeps its shader until the device goes (flagwave64_device)
}
static int fwh_add(int k, ID3D11VertexShader *vs, ID3D11VertexShader *own) {   // 0: the table is full
    int ok = 0;
    AcquireSRWLockExclusive(&g_fwh_lock);
    if (g_fwh_n < FW_NH / 4 * 3) {
        unsigned h = fwh_at(vs);
        while (g_fwh[h].vs) h = (h + 1) & (FW_NH - 1);
        g_fwh[h].own = own; g_fwh[h].k = k; g_fwh[h].vs = vs;
        g_fwh_n++;
        ok = 1;
    }
    ReleaseSRWLockExclusive(&g_fwh_lock);
    return ok;
}''')
sub(9, '''    for (int k = 0; k < 3; k++)
        for (int i = 0; i < FW_NVS; i++)
            if (vs && g_vs[k][i] == vs) return k;
    return -1;''', '''    int h = fwh_find(vs);
    return h < 0 ? -1 : g_fwh[h].k;''')
sub(9, '''    if (out && *out) {
        g_vs[k][(nth - 1) % FW_NVS] = *out;
        if (nth == 5 || nth == FW_NVS + 1) wlog("%s shader made from ours %ld times%s", FW_KN[k], nth, nth > FW_NVS ? " - more than remembered" : "");
        ID3D11VertexShader *own = NULL;   // the game's own too: drawn while the flags are off
        if (SUCCEEDED(r_cvs(d, bc, len, cl, &own))) g_own[k][(nth - 1) % FW_NVS] = own;
    }''', '''    if (out && *out) {
        ID3D11VertexShader *own = NULL;   // the game's own too: drawn while the flags are off
        if (FAILED(r_cvs(d, bc, len, cl, &own))) own = NULL;
        ID3D11VertexShader_AddRef(*out);
        if (!fwh_add(k, *out, own)) {
            ID3D11VertexShader_Release(*out);
            if (own) ID3D11VertexShader_Release(own);
            static volatile LONG told;
            if (!InterlockedExchange(&told, 1)) wlog("%s shader made from ours %ld times - more than the table holds (%d)", FW_KN[k], nth, FW_NH / 4 * 3);
        }
        if (nth == 5 || nth == 100 || nth == 1000) wlog("%s shader made from ours %ld times", FW_KN[k], nth);
    }''')
sub(9, '        for (int i = 0; i < FW_NVS; i++) if (g_vs[k][i] == vs && g_own[k][i]) { vs = g_own[k][i]; break; }',
    '        int h = fwh_find(vs);\n        if (h >= 0 && g_fwh[h].own) vs = g_fwh[h].own;')

# Always, so that the tail compiles for the negative controls too: the context flagwave64_device was given. Change 10
# is what reads it.
assert src.count('static ID3D11DeviceContext *g_ctx;') == 1
src = src.replace('static ID3D11DeviceContext *g_ctx;',
                  'static ID3D11DeviceContext *g_ctx;\n'
                  '// not static: the --without=10 control sets it and never reads it, which -Wextra would refuse\n'
                  'ID3D11DeviceContext *g_ctx_given;   // the context the game draws through (flagwave64_device)')

# 4. no entry of its own: its DllMain and the 32-bit way to the device (GetProcAddress, CreateDXGIFactory1) go;
#    perffix64.c calls flagwave64_setup / _device / _frame (tail). The test functions stay, not exported.
i = src.index('static HRESULT(STDMETHODCALLTYPE *r_csc)(IDXGIFactory *'); j = src.index('// compiles our shaders')
src = src[:i] + src[j:]
i = src.index('BOOL WINAPI DllMain(')
src = src[:i]
assert src.count('__declspec(dllexport) ') == 5
src = src.replace('__declspec(dllexport) ', '')

tail = open(P + '/src/flagwave64_tail.inc', encoding='utf-8').read()
open(out, 'w', encoding='utf-8', newline='\n').write('// GENERATED by gen_flagwave64.py from Flagwave/dll/flagwave.c - do not edit\n' + src + tail)
