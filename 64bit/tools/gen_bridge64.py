# Writes out/gen/bridge64.c and out/gen/fgmotion64.c: the 32-bit upscaling add-on's game side (heapfix/bridgeprobe.c
# with its bp_*.inc.c parts, heapfix/fgmotion.c; both unchanged in heapfix/) as a part of perffix64, with the changes
# Sol Invictus's 64-bit loader and the Linux game file need, plus src/bridge64_tail.inc (the 64-bit glue: install,
# game hooks, device / swap chain / Present from d3d64.c). heapfix/fgbridge.c is compiled as it is (src/fgbridge64.c).
# usage: gen_bridge64.py <repo root> <out dir> [--without=N]
#   --without=N leaves change N out: the negative controls of bridge64_test (each must make the test fail).
# Every change must match exactly once: a change upstream in heapfix/ stops the build here instead of building
# something half-changed.
import os, re, sys
root, outdir = sys.argv[1], sys.argv[2]
without = 0
for a in sys.argv[3:]:
    if a.startswith('--without='): without = int(a.split('=')[1])
hf = os.path.join(root, 'heapfix' if os.path.isdir(os.path.join(root, 'heapfix')) else '32bit')   # dev tree / public 32bit/

def inline(text, depth=0):
    def rep(m):
        assert depth < 4
        return '// ---- inlined: %s\n%s\n// ---- end of %s' % (m.group(1), inline(open(os.path.join(hf, m.group(1)), encoding='utf-8').read(), depth + 1), m.group(1))
    return re.sub(r'^#include "(bp_[a-z0-9_]+\.inc\.c)"[^\n]*$', rep, text, flags=re.M)

src = inline(open(os.path.join(hf, 'bridgeprobe.c'), encoding='utf-8').read())

def sub(n, a, b, count=1):
    global src
    assert src.count(a) == count, (n, src.count(a), a[:80])
    if n != without: src = src.replace(a, b)

# 1. the 32-bit install and its x86 camera stub go (set-camera FUN_1177bb10, the pack switch, install_bridgeprobe):
#    the 64-bit ones are in the tail
cut = src.index("// The renderer's set-camera FUN_1177bb10")
src = src[:cut] + '#include "bridge64_tail.inc"\n'
# 2. the back buffer at the HUD-less point is the game's own: Sol's back end draws into a texture of its own and copies
#    it into the swap chain's buffer at Present (dx_present), so the swap chain's buffer holds the previous frame there.
#    The tail learns the texture from that copy (g_game_bb); until then (the first frame) the swap chain's buffer.
sub(2, '''static ID3D11Resource *backbuffer(void) {
    ID3D11Resource *bb = NULL;
    if (g_sc) IDXGISwapChain_GetBuffer(g_sc, 0, &iid_resource, (void **)&bb);
    return bb;
}''', '''static ID3D11Resource *g_game_bb;   // 64-bit: the loader's own back buffer texture (bridge64_tail.inc: w_copyres)
static ID3D11Resource *backbuffer(void) {
    ID3D11Resource *bb = g_game_bb;
    if (bb) ID3D11Resource_AddRef(bb);
    else if (g_sc) IDXGISwapChain_GetBuffer(g_sc, 0, &iid_resource, (void **)&bb);
    return bb;
}''')
# 3. ... and at Present the frame is the swap chain's buffer: the loader's copy, its fps overlay and its menu button are
#    in it by then, and the F8 panel draws there
sub(3, '''        DXGI_SWAP_CHAIN_DESC d;
        ID3D11Resource *bb = backbuffer();
        if (bb && SUCCEEDED(IDXGISwapChain_GetDesc(sc, &d))) {''', '''        DXGI_SWAP_CHAIN_DESC d;
        ID3D11Resource *bb = NULL;
        IDXGISwapChain_GetBuffer(sc, 0, &iid_resource, (void **)&bb);
        if (bb && SUCCEEDED(IDXGISwapChain_GetDesc(sc, &d))) {''')
# 4. DXGI's Present and CreateSwapChain are called as d3d64.c hands them over: x64 has no hot-patch point to look
#    behind (fgb_unhooked reads x86 code)
sub(4, 'unhooked((void *)g_present_real, &cache, &gave_up, "Present")', '((void)cache, (void)gave_up, (void *)g_present_real)')
sub(4, '''unhooked(
        (void *)g_create_real, &cache, &gave_up, "CreateSwapChain")''', '((void)cache, (void)gave_up, (void *)g_create_real)')
# 5. Present is not hooked here: d3d64.c owns the slot (its fps log, the panel's and Flag Wave's frame point) and calls
#    bridge64_present; hooking it again would put this hook in front and skip d3d64's
sub(5, '''        // with ReShade: Present is already hooked on the real swapchain (rs_swapchain), so patch_once leaves the wrapper's
        patch_once(&vt[offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *)], (void *)w_present, (void **)&r_present);
        if (!g_present_real) g_present_real = dxgi_own(vt, offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *), (void *)r_present, "Present");
''', '''        // 64-bit: Present comes through d3d64.c (bridge64_present)
''')
# 6. the record copy for the motion pass: three memcpy calls of the 64-bit game, retargeted when the add-on was installed
#    (install_bridge64, with the other threads paused); here only whether that happened
sub(6, '''                unsigned char *call = (unsigned char *)(UINT_PTR)(g_pt_base + game_rva((void *)(UINT_PTR)g_pt_base, 0x17DABAF));
                unsigned target = g_pt_base + game_rva((void *)(UINT_PTR)g_pt_base, 0x1A27CA0);
                DWORD prot;
                if (call[0] != 0xE8 || (unsigned)(UINT_PTR)call + 5 + *(int *)(call + 1) != target) {
                    g_log("motion pass: the record copy call at +0x17DABAF is not the expected one - off");
                } else if (fgm_init(g_dev, g_ctx, fgb_mv_texture(), g_live_w, g_live_h, g_log) &&
                           VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &prot)) {
                    *(int *)(call + 1) = (int)((UINT_PTR)fgm_copy - ((UINT_PTR)call + 5));
                    VirtualProtect(call, 5, prot, &prot);
                    FlushInstructionCache(GetCurrentProcess(), call, 5);
                    fgb_set_motion(fgm_pass);''', '''                if (!g_copy64) {
                    g_log("motion pass: the game's record copies are not the expected code - off");
                } else if (fgm_init(g_dev, g_ctx, fgb_mv_texture(), g_live_w, g_live_h, g_log)) {
                    fgb_set_motion(fgm_pass);''')
sub(6, 'static int g_copy_patched;', 'static int g_copy_patched, g_copy64;   // g_copy64: calls retargeted to fgm_copy64 (bridge64_tail.inc)')
# 9. the 32-bit flag wave's cloth code is in perffix64 already, compiled once for Flag Wave (src/flagsim64.c); the
#    bridge's flag parts only call into it, and never run here (g_fw_on stays 0: Flag Wave has its own path on 64-bit)
sub(9, '#include "flagsim.c"   // included, not linked: reshade_test.c builds this file as source too',
    '#include "flagsim.h"   // 64-bit: flagsim.c is linked once (src/flagsim64.c, for Flag Wave)')
open(os.path.join(outdir, 'bridge64.c'), 'w', encoding='utf-8', newline='\n').write(
    '// GENERATED by tools/gen_bridge64.py from heapfix/bridgeprobe.c and its parts - do not edit\n'
    '#define Eip Rip   // gamerva.h (paused_write) and fgbridge.c read a thread\'s position: rip / rsp on x64\n'
    '#define Esp Rsp\n' + src)

# ---- fgmotion.c: the 64-bit WEIGHTED_INSTANCE (fill_animation_texture 0x3416d50 writes the texture index at +0x6c, reads
# the pose at +0x70; the record copies read the record at +0x88; 32-bit: +0x5c, +0x60, +0x74; the pose's bone count
# stays at +4), and the sm_4_1 builds of the four skinning shaders (the 64-bit game makes sm_4_1 shaders: Flag Wave's
# 30a8b161). Their input signatures are the sm_4_0 ones' (bridge64_test compares them).
src = open(os.path.join(hf, 'fgmotion.c'), encoding='utf-8').read()
sub(7, '(const unsigned char *)src - 0x74', '(const unsigned char *)src - 0x88')
sub(7, '+ 0x5C)', '+ 0x6C)', 2)
sub(7, '*(const unsigned char *const *)(item + 0x60)', '*(const unsigned char *const *)(item + 0x70)')
sub(7, '+ 0x74', '+ 0x88', 3)
sub(8, '    int skin4 = sig == 0x520dbe181fbd767aull || sig == 0x3d52a1037cfa96abull;\n'
       '    if (!m_on || (!skin4 && sig != 0xa51431ebf7d15a22ull && sig != 0x4b23a601f0f7b19bull) || n > 16) return;',
    '    int skin4 = sig == 0x520dbe181fbd767aull || sig == 0x3d52a1037cfa96abull || sig == 0x50a453c23f7ccb27ull || sig == 0xb2abf30c55179bedull;\n'
    '    if (!m_on || (!skin4 && sig != 0xa51431ebf7d15a22ull && sig != 0x4b23a601f0f7b19bull && sig != 0xa4eaf1c7fa343592ull && sig != 0xee6b5c452f3afd62ull) || n > 16) return;')
open(os.path.join(outdir, 'fgmotion64.c'), 'w', encoding='utf-8', newline='\n').write(
    '// GENERATED by tools/gen_bridge64.py from heapfix/fgmotion.c - do not edit\n' + src)
print('bridge64: bridge64.c and fgmotion64.c written%s' % (' (without change %d)' % without if without else ''))
