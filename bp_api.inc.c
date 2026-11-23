// Part of bridgeprobe.c (included there in this order, not compiled on its own): the three exports other mods call: fgb_motion_vectors, fgb_state, fgb_d3d11_original.
// For other mods (fgb_motion_api.h; a UI / effects mod's temporal effects, so players do not pay for a second
// per-object pass): this frame's motion vectors, made at the first call. NULL when the bridge is not running, this
// frame has no 3D scene camera yet, or it is a native-size frame while DLSS SR is on (no vectors made for it).
ID3D11Texture2D *__cdecl fgb_motion_vectors(FgbMotionInfo *info) {
    if (!info || info->size < sizeof *info) return NULL;
    info->version = FGB_MOTION_API_VERSION;
    info->copy_hook = g_copy_patched;
    if (GetCurrentThreadId() != g_pt_main) return NULL;
    g_mvx_calls++;
    if (g_mvx_last != g_frame) { g_mvx_last = g_frame; g_mvx_frames++; }
    ID3D11Texture2D *t = fgb_mv_texture();
    if (g_live_state != 1 || !fgb_active() || !g_main_depth || !t || g_vp_frame != g_frame || (g_sr && !g_frame_scaled) ||
        !fgb_make_vectors(g_main_depth, g_vp)) { g_mvx_none++; return NULL; }
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(t, &d);
    info->frame = fgb_frames();
    info->per_object = g_motion && g_motion_on;   // the player's choice in the F11 panel decides, for both mods
    info->render_w = g_sr ? g_rw : d.Width; info->render_h = g_sr ? g_rh : d.Height;
    info->tex_w = d.Width; info->tex_h = d.Height;
    info->jitter_x = g_sr ? g_jx : 0; info->jitter_y = g_sr ? g_jy : 0;
    ID3D11Texture2D_AddRef(t);
    return t;
}

// For other mods (fgb_motion_api.h): the state above without making vectors (the 4K UI mod's passes need the render size)
int __cdecl fgb_state(FgbState *s) {
    if (!s || s->size < sizeof *s) return 0;
    s->version = FGB_STATE_VERSION;
    s->running = g_live_state == 1 && fgb_active();
    s->sr_mode = s->running ? (unsigned)g_sr : 0;
    s->tex_w = g_live_w; s->tex_h = g_live_h;
    s->render_w = s->running && g_sr ? g_rw : g_live_w; s->render_h = s->running && g_sr ? g_rh : g_live_h;
    s->scaled = s->running && g_jit_on && (g_rw != g_live_w || g_rh != g_live_h);
    s->jitter_x = s->running && g_sr ? g_jx : 0; s->jitter_y = s->running && g_sr ? g_jy : 0;
    s->frame_generation = s->running ? g_fg : 0;
    s->per_object = g_motion && g_motion_on;
    s->frame = fgb_frames();
    return 1;
}

// For other mods (fgb_motion_api.h): d3d11's own function for a context vtable slot - what the add-on's hook there calls
void *__cdecl fgb_d3d11_original(unsigned slot) {
    if (!g_ctx || slot >= sizeof(ID3D11DeviceContextVtbl) / sizeof(void *)) return NULL;
#define HK(m, w, r) {offsetof(ID3D11DeviceContextVtbl, m) / sizeof(void *), (void *)w, (void **)&r}
    static const struct { size_t slot; void *ours; void **orig; } hooked[] = {
        HK(RSSetViewports, w_rsvp, r_rsvp), HK(RSGetViewports, w_rsgetvp, r_rsgetvp), HK(OMSetRenderTargets, w_omrt, r_omrt),
        HK(OMSetRenderTargetsAndUnorderedAccessViews, w_omrtuav, r_omrtuav), HK(OMSetDepthStencilState, w_omdss, r_omdss),
        HK(PSSetConstantBuffers, w_pscb, r_pscb), HK(Map, w_map, r_map), HK(Unmap, w_unmap, r_unmap),
        HK(UpdateSubresource, w_updsub, r_updsub), HK(PSSetSamplers, w_pssamp, r_pssamp), HK(DrawIndexedInstanced, w_dii, r_dii), HK(DrawIndexed, w_di, r_di),
        HK(DrawInstanced, w_dinst, r_dinst), HK(Draw, w_draw, r_draw)};
#undef HK
    void *fn = (*(void ***)g_ctx)[slot];
    for (unsigned i = 0; i < sizeof hooked / sizeof *hooked; i++)
        if (hooked[i].slot == slot && fn == hooked[i].ours) fn = *hooked[i].orig;
    HMODULE owner = NULL, d3d = GetModuleHandleW(L"d3d11.dll");
    if (!fn || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)fn, &owner))
        return NULL;
    return owner == d3d ? fn : NULL;
}
