// Part of bridgeprobe.c (included there, not compiled on its own): lod_sampler(): the biased copies of the game's material samplers (bpglue_test.c builds this file too).
// ---- texture mip bias below native size (AGENT_LOG 136, Rome 2 DLSS_LOG 39 / 46): the game picks mip levels for the
// render size, so ground and unit textures come out softer than at native. Bias log2(render width / screen width)
// (Quality -0.58, Performance -1), none at DLAA: NVIDIA's further -1.0 lightened the shadows in Rome 2. Material samplers
// only (anisotropic or wrapping; not shadow comparison, min / max or clamped post-process ones), swapped for biased copies
// while the scene is drawn small into the screen's targets (not the shadow maps: alpha-tested leaves cast lighter shadows
// with it, Rome 2). The game's samplers are held (AddRef) so an address is never reused for another; copies are made again
// when the render size changes.
static struct { ID3D11SamplerState *game, *ours; } g_lod[256];
static unsigned g_nlod, g_lod_swaps, g_lod_full;
static int g_lod_off;      // F11 Debug View (test builds): the game's samplers as they are, to compare
static float g_lod_bias;   // the bias the copies were made with
static struct { ID3D11SamplerState *in, *out; } g_lodc[256];   // the last answers, by address (cleared with the copies)
static ID3D11SamplerState *lod_sampler(ID3D11SamplerState *s, float bias) {
    if (!s) return s;
    if (bias != g_lod_bias) {   // a new render size: new copies
        for (unsigned i = 0; i < g_nlod; i++) {
            ID3D11SamplerState_Release(g_lod[i].game);
            if (g_lod[i].ours) ID3D11SamplerState_Release(g_lod[i].ours);
        }
        g_nlod = 0;
        g_lod_bias = bias;
        memset(g_lodc, 0, sizeof g_lodc);
    }
    unsigned ch = ((unsigned)(UINT_PTR)s >> 4) & 255;
    if (g_lodc[ch].in == s) return g_lodc[ch].out;
    for (unsigned i = 0; i < g_nlod; i++) {
        if (g_lod[i].game == s) { g_lodc[ch].in = s; return g_lodc[ch].out = g_lod[i].ours ? g_lod[i].ours : s; }
        if (g_lod[i].ours == s) { g_lodc[ch].in = s; return g_lodc[ch].out = s; }   // our copy, read back and set again by someone saving state
    }
    if (g_nlod == 256) { g_lod_full++; return s; }   // shortcut: 256 game samplers at most, the rest keep their bias
    D3D11_SAMPLER_DESC d;
    ID3D11SamplerState_GetDesc(s, &d);
    ID3D11SamplerState *o = NULL;
    if (d.Filter < D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT && (d.Filter == D3D11_FILTER_ANISOTROPIC || d.AddressU == D3D11_TEXTURE_ADDRESS_WRAP)) {
        d.MipLODBias = fmaxf(-16.0f, d.MipLODBias + bias);
        if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &d, &o))) o = NULL;
    }
    ID3D11SamplerState_AddRef(s);
    g_lod[g_nlod].game = s;
    g_lod[g_nlod++].ours = o;
    return o ? o : s;
}
