// The flag wave's table of its own vertex shader objects (bridgeprobe.c). fwkind_test.c includes this same file, so the
// test runs the code the DLL runs (it used to be cut out of bridgeprobe.c with sed, with the store copied by hand).
// The game creates its prop vertex shader many times over, and every copy has to be recognised by fw_kind or that
// copy's draws get no cloth. At 4 remembered copies (2026-11-12, Flag Wave) most of 1212's main-view draws were not
// recognised and flickered; 64 since.
// A reference is held on every remembered object (FW_HOLD): the table knows a shader by its address, and an object the
// game has released gives its address to the next one made, which would then be taken for ours - it would get the wind
// block in slot 2 and the cloth's views (audit 2026-11-20; the sampler and camera-buffer tables hold theirs for the
// same reason). A copy pushed out of a full table is let go (FW_DROP).
#pragma once
#define FW_NVS 64
#define FW_KINDS 3   // 0 main view (55ca645c), 1 shadow (eb3006be), 2 the main view's twin (0cae34c7: flagvs.h FW_TWIN)
static ID3D11VertexShader *g_fw_vs[FW_KINDS][FW_NVS];   // our shader objects, by kind
static ID3D11VertexShader *g_fw_own[FW_KINDS][FW_NVS];  // flagwave_ab.txt: the game's own shader for each of ours
#ifndef FW_HOLD   // fwkind_test.c: its shader objects are plain addresses
#define FW_HOLD(vs) ((void)(vs))
#define FW_DROP(vs) ((void)(vs))
#endif
static int fw_kind(ID3D11VertexShader *vs) {
    for (int k = 0; k < FW_KINDS; k++)
        for (int i = 0; i < FW_NVS; i++)
            if (vs && g_fw_vs[k][i] == vs) return k;
    return -1;
}
// the nth copy of kind k the game made (nth from 1) and, for the A/B, the game's own shader for it (made by us: ours
// alone already, or NULL). Past a full table the oldest copy goes: the game's current shaders are the live ones.
static __attribute__((unused)) void fw_store(int k, long nth, ID3D11VertexShader *vs, ID3D11VertexShader *own) {
    unsigned i = (unsigned)(nth - 1) % FW_NVS;
    ID3D11VertexShader *was = g_fw_vs[k][i], *was_own = g_fw_own[k][i];
    FW_HOLD(vs);
    g_fw_vs[k][i] = vs;
    g_fw_own[k][i] = own;
    if (was) FW_DROP(was);
    if (was_own) FW_DROP(was_own);
}
