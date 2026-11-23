// Part of bridgeprobe.c (included there in this order, not compiled on its own): the stage hooks passtime.c calls: bp_enter (the HUD-less point is found here), bp_exit, bp_frame.
__attribute__((force_align_arg_pointer)) static void bp_enter(unsigned id, const void *obj) {
    if (GetCurrentThreadId() != g_pt_main) return;
    if (g_sp < 64) { g_stk[g_sp].id = id; g_stk[g_sp].obj = obj; }
    g_sp++;
    if (id == g_id_view && g_view_sp < 16) g_view_obj[g_view_sp] = obj;
    if (id == g_id_cam && g_pt_args) {   // FUN_1177bb10(camera block, stage): every camera the renderer is given
        const unsigned char *cam = (const unsigned char *)(UINT_PTR)g_pt_args[0];
        const unsigned *stage = (const unsigned *)(UINT_PTR)g_pt_args[1];
        unsigned vt = stage ? stage[0] - g_pt_base : 0, caller = g_pt_args[-1] - g_pt_base;
        MEMORY_BASIC_INFORMATION mb;
        int ok = cam && VirtualQuery(cam, &mb, sizeof mb) && mb.State == MEM_COMMIT &&
                 (const unsigned char *)mb.BaseAddress + mb.RegionSize >= cam + 0x200;
        if (g_live && ok && vt == g_vt_main) { memcpy(g_vp, cam + 0x1B0, 64); g_vp_frame = g_frame; }
        if (g_camera_left && g_cam && ok) {
            unsigned rec[4] = {g_frame | 0x80000000u, vt, caller, 0x200};   // type 2 record: one camera block
            fwrite(rec, 4, 4, g_cam);
            fwrite(cam, 1, 0x200, g_cam);
        }
        if (g_capturing) fprintf(g_cap, "%*sset camera: stage vtable +0x%X, camera @%p, called from +0x%X\n", (int)(g_sp - 1) * 2, "",
                                 vt, (const void *)cam, caller);
        return;
    }
    if (id == g_id_view) g_view_sp++;
    if (id == g_id_stage && (g_capturing || g_motion || g_fw_on)) mp_hook(g_capturing ? "stage entry" : NULL);   // the flag sim needs the draw hook too
    if (id == g_id_stage && g_live_state == 1) jit_install();
    unsigned vt = obj ? *(const unsigned *)obj - g_pt_base : 0;
    if (g_live_state == 1 && id == g_id_stage && (vt == g_vt_fx || vt == g_vt_fx_end)) {   // AMD FSR's reactive mask
        if (vt == g_vt_fx_end) fgb_react_end();
        else {
            ID3D11RenderTargetView *rt = NULL;
            ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 1, &rt, NULL);
            if (rt) { ID3D11Resource *r = NULL; ID3D11RenderTargetView_GetResource(rt, &r); ID3D11RenderTargetView_Release(rt);
                      if (r) { fgb_react_begin(r); ID3D11Resource_Release(r); } }
        }
    }
    if (g_live_state == 1 && id == g_id_stage && vt == g_vt_hud && g_hud_frame != g_frame && fgb_active()) {
        g_hud_frame = g_frame;
        jit_frame_end();
        ID3D11DepthStencilView *dsv = NULL;
        ID3D11Resource *depth = NULL, *bb = backbuffer();
        ID3D11DeviceContext_OMGetRenderTargets(g_ctx, 0, NULL, &dsv);
        if (dsv) { ID3D11DepthStencilView_GetResource(dsv, &depth); ID3D11DepthStencilView_Release(dsv); }
        if (depth && depth != g_main_depth) {   // for fgb_motion_vectors(), called before this point in later frames
            if (g_main_depth) ID3D11Resource_Release(g_main_depth);
            g_main_depth = depth;
            ID3D11Resource_AddRef(depth);
        }
        // ReShade: its effects before the HUD-less copy (frame generation's two images both have them), or with DLSS SR
        // on DLSS's output after the round trip (full size, the depth back at full size too), then handed to the helper
        // as frame generation's HUD-less image
        int sr_trip = g_sr && g_frame_scaled && bb && depth && g_vp_frame == g_frame;   // DLSS SR runs in this frame's fgb_hudless
        if (g_rs && bb && !sr_trip) g_rs_frames += rsl_effects(g_ctx, bb, depth);
        if (!depth) g_br_nodepth++;
        else if (g_vp_frame != g_frame) g_br_novp++;
        else if (bb) {
            g_hud_this_frame = 1;
            if (!g_sr || g_frame_scaled) { fgb_hudless(bb, depth, g_vp); g_br_hud++; }
            else g_br_native++;
            int fgb_depth_failed(void);
            if (g_sr && !g_depth_bad && fgb_depth_failed()) {
                g_depth_bad = 1;
                sr_set(0);
                g_log("DLSS add-on: the game's depth is multisampled (its Anti-Aliasing is on MSAA) - upscaling off for this "
                      "session, the game renders at full size; set Anti-Aliasing to Off in the game's graphics options");
                fgp_set_note(L"Upscaling is off: set the game's Anti-Aliasing to Off and restart the game.");
            }
        }
        if (g_rs && sr_trip && rsl_effects(g_ctx, bb, depth)) { g_rs_frames++; fgb_hud_post(bb); }
        if (g_rs && g_vp_frame == g_frame) rsl_camera(g_vp);
        if (depth) ID3D11Resource_Release(depth);
        if (bb) ID3D11Resource_Release(bb);
    }
    if (!g_capturing || !g_ctx) return;
    char t[600];
    targets(t, sizeof t);
    if (id == g_id_stage) fprintf(g_cap, "%*senter stage vtable +0x%X: %s\n", (int)(g_sp - 1) * 2, "", vt, t);
    else if (id == g_id_scene) fprintf(g_cap, "%*senter scene call %u: %s\n", (int)(g_sp - 1) * 2, "", g_scene_no, t);
    else if (id == g_id_view) fprintf(g_cap, "%*senter camera loop (stage vtable +0x%X, %u blocks)\n", (int)(g_sp - 1) * 2, "", vt,
                                      obj ? *(const unsigned *)((const char *)obj + 0x98) : 0);
    else if (id == g_id_level) fprintf(g_cap, "%*senter render level: %s\n", (int)(g_sp - 1) * 2, "", t);
}

__attribute__((force_align_arg_pointer)) static void bp_exit(unsigned id) {
    if (GetCurrentThreadId() != g_pt_main) return;
    if (g_sp) g_sp--;
    if (id == g_id_view && g_view_sp) {
        g_view_sp--;
        const unsigned char *o = g_view_sp < 16 ? g_view_obj[g_view_sp] : NULL;
        if (o && g_camera_left && g_cam) {   // blocks are updated inside the loop: read them after it
            unsigned n = *(const unsigned *)(o + 0x98), rec[4] = {g_frame, *(const unsigned *)o - g_pt_base, n, 0x1C8};
            if (n <= 16) {
                fwrite(rec, 4, 4, g_cam);
                fwrite(o + 0x294, 0x1C8, n, g_cam);
                fwrite(o + 0xD4, 16, n, g_cam);   // the sub-viewport rectangles
            }
        }
    }
    if (id == g_id_scene) g_scene_no++;
    if (!g_capturing || !g_ctx) return;
    char t[600];
    int bb = targets(t, sizeof t);
    unsigned vt = g_sp < 64 && g_stk[g_sp].obj ? *(const unsigned *)g_stk[g_sp].obj - g_pt_base : 0;
    if (id == g_id_cam) return;
    const char *what = id == g_id_stage ? "stage" : id == g_id_scene ? "scene call" : id == g_id_view ? "camera loop" : "render level";
    fprintf(g_cap, "%*sexit %s (vtable +0x%X): %s\n", (int)g_sp * 2, "", what, vt, t);
    if (bb && id == g_id_stage) {
        char w[64];
        snprintf(w, sizeof w, "after stage vtable +0x%X", vt);
        dump_bb(w);
    }
}

__attribute__((force_align_arg_pointer)) static void bp_frame(void) {
    g_frame++;
    g_scene_no = 0;
    if (g_capturing) {   // the captured frame is over
        g_capturing = 0;
        fprintf(g_cap, "== end of captured frame\n");
        mp_capture_end(g_cap);
        fclose(g_cap);
        g_cap = NULL;
    }
    if (g_camera_left && !--g_camera_left && g_cam) { fclose(g_cam); g_cam = NULL; }
    if (g_armed && g_sc) {   // a capture uses the device of the game's swapchain (whatever other devices were made since)
        static const GUID iid_dev = {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};   // ID3D11Device
        ID3D11Device *d = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetDevice(g_sc, &iid_dev, (void **)&d))) {
            if (d != g_dev) {
                if (g_ctx) ID3D11DeviceContext_Release(g_ctx);
                if (g_dev) ID3D11Device_Release(g_dev);
                if (g_staging) ID3D11Texture2D_Release(g_staging);
                g_staging = NULL;
                g_dev = d;
                ID3D11Device_GetImmediateContext(g_dev, &g_ctx);
                g_dev_from_sc = 1;
            } else ID3D11Device_Release(d);
        }
    }
    if (g_armed && g_ctx && !g_camera_left) {
        g_armed = 0;
        g_captures++;
        wchar_t p[MAX_PATH];
        _snwprintf(p, MAX_PATH, L"%ls\\bridge_capture_%d.txt", g_dir, g_captures);
        g_cap = _wfopen(p, L"w");
        _snwprintf(p, MAX_PATH, L"%ls\\bridge_camera_%d.bin", g_dir, g_captures);
        g_cam = _wfopen(p, L"wb");
        if (g_cap) {
            g_capturing = 1;
            g_dumps = 0;
            mp_capture_start();
            if (g_motion) {   // and a picture of the motion vectors after this frame's motion pass
                _snwprintf(p, MAX_PATH, L"%ls\\motion_mv_%d.raw", g_dir, g_captures);
                fgm_dump_next(p);
            }
            g_camera_left = 31;
            fprintf(g_cap, "== bridge capture %d, frame %u; swapchain %ux%u format %u buffers %u swap effect %u windowed %d\n",
                    g_captures, g_frame, g_scd.BufferDesc.Width, g_scd.BufferDesc.Height, g_scd.BufferDesc.Format, g_scd.BufferCount,
                    g_scd.SwapEffect, g_scd.Windowed);
        }
    }
// The capture, tuning, screenshot and vertex-dump triggers are dev-only (the last three since the 6.10.0 release check;
// the capture since 2026-11-21: bridge_go.txt was looked for in the game folder every 32 frames of every game that
// had the add-on or a flag pack).
    if (g_vdump_left && --g_vdump_left == 0) { fclose(g_vdump); g_vdump = NULL; g_log("vertex dump written"); }
    g_vdump_frame++;
}
