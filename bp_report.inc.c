// Part of bridgeprobe.c (included there in this order, not compiled on its own): the 30 s report in heapfix.log.
int bp_bridge_running(void) { return g_live && g_live_state == 1; }   // heapfix.c: how often to report
static void bp_report(void (*log)(const char *fmt, ...)) {
    log("bridge probe: %u presents (%u not on the main thread), sync interval 0/1/2/3/more: %u/%u/%u/%u/%u, captures %d%s",
        g_presents, g_present_other_thread, g_sync[0], g_sync[1], g_sync[2], g_sync[3], g_sync[4], g_captures,
        g_sc ? "" : " - no swapchain seen");
    log("  D3D11CreateDevice calls seen %d, device %s", g_creates, !g_ctx ? "not known yet" : g_dev_from_sc ? "taken from the swapchain" : "from D3D11CreateDevice");
    if (g_live)
        log("  frame bridge %s: %u frames handed over, %u pacing timeouts, HUD-less copies %u, native frames %u, no camera %u, no depth %u",
            g_live_state == 1 ? "running" : g_live_state == 2 ? "off" : "not started", fgb_frames(), fgb_pace_timeouts(), g_br_hud,
            g_br_native, g_br_novp, g_br_nodepth);
    if (g_fb_frames || g_nested[0] || g_nested[1] || g_nested[2])
        log("  fallback: %u frames presented through the mod's own swapchain, %u not S_OK (last 0x%08lX); nested Presents: "
            "game's %u, mod's own %u, other %u (each sent straight to DXGI)", g_fb_frames, g_fb_notok, (unsigned long)g_fb_last, g_nested[0], g_nested[1], g_nested[2]);
    if (g_rs) log("  ReShade: effects before the UI in %u frames%s", g_rs_frames, g_rs_sc ? "" : " (the game's swapchain was not ReShade's wrapper)");
    if (g_sr) {
        unsigned sf, st; double sa, sm;
        void fgb_sr_stats(unsigned *, unsigned *, double *, double *);
        fgb_sr_stats(&sf, &st, &sa, &sm);
        log("  DLSS render %ux%u: %u frames jittered / scaled, viewport calls seen %u (%u changed); camera blocks %u, %u pixel shader "
            "copies written; SR round trips %u, game CPU wait avg %.3f ms max %.3f ms, %u timeouts",
            g_rw, g_rh, g_jit_frames, g_jit_vp_calls, g_jit_vp_jittered, g_ncbuf, g_cam_writes, sf, sa, sm, st);
        g_jit_frames = g_jit_vp_calls = g_jit_vp_jittered = g_cam_writes = 0;
        unsigned nb = 0;
        for (unsigned i = 0; i < g_nlod; i++) nb += g_lod[i].ours != NULL;
        if (g_nlod) log("  texture mip bias %.2f: %u of %u game samplers biased, %u sampler binds swapped%s", g_lod_bias, nb, g_nlod,
                        g_lod_swaps, g_lod_full ? ", table full" : "");
        if (g_nlod) log("  texture mip bias, slots the game did not set again: %u biased going in, %u put back going out", g_lod_in, g_lod_out);
        g_lod_swaps = g_lod_in = g_lod_out = 0;
        if (g_ps_bloom_ds) log("  bloom source pass with the jitter taken out: %u draws%s", g_bloom_draws, g_bloom_off ? " (off)" : "");
        g_bloom_draws = 0;
        unsigned fgb_sharp_frames(void);
        static unsigned sharp0;
        unsigned sh = fgb_sharp_frames();
        log("  sharpness %s: %u frames sharpened; DLSS preset %s%s; V-Sync %s%s", SHARP_KEYS[g_sharp], sh - sharp0, PRESET_KEYS[g_preset],
            // "too old" only for a helper that runs without the feature; one that never started is said so (improvement)
            g_live_state != 1 ? " (the add-on is not running)" : fgb_helper_has_presets() ? "" : " (the add-on's helper is too old to apply it)",
            VSYNC_KEYS[g_vsync], g_vsync || g_live_state != 1 || fgb_helper_has_vsync() ? "" : " (the add-on's helper keeps it on)");
        sharp0 = sh;
    }
    if (g_live_state == 1) {
        void fgb_cpu_stats(double *, double *, double *, unsigned *);
        double hm, pm, wm;
        unsigned fr;
        fgb_cpu_stats(&hm, &pm, &wm, &fr);
        if (fr) log("  frame bridge main-thread time per frame: HUD-less step %.3f ms (incl. the DLSS pickup and the motion pass), "
                    "Present step %.3f ms, of which waiting for the helper %.3f ms", hm / fr, pm / fr, wm / fr);
        void fgb_gpu_stats(double ms[4], unsigned *frames, unsigned *lost);
        double g4[4];
        unsigned gf, gl;
        fgb_gpu_stats(g4, &gf, &gl);
        void fgb_wait_stats(unsigned *queued, unsigned *frames);
        unsigned wq, wf;
        fgb_wait_stats(&wq, &wf);
        if (gf) log("  frame bridge GPU per frame (%u frames timed, %u not read): frame interval %.3f ms, a wait for the helper queued in %u of %u "
                    "frames (stalled %.3f ms each), HUD-less step %.3f ms (copies, motion vectors, motion pass, DLSS pickup), Present step %.3f ms",
                    gf, gl, g4[0], wq, wf, g4[1], g4[2], g4[3]);
        if (fr) log("  game calls through the add-on's hooks per frame: Map %.0f, Unmap %.0f, UpdateSubresource %.0f, PSSetConstantBuffers %.0f, "
                    "OMSetRenderTargets %.0f, OMSetDepthStencilState %.0f, RSSetViewports %.0f, DrawIndexedInstanced %.0f",
                    (double)g_hk[HK_MAP] / fr, (double)g_hk[HK_UNMAP] / fr, (double)g_hk[HK_UPDSUB] / fr, (double)g_hk[HK_PSCB] / fr,
                    (double)g_hk[HK_OMRT] / fr, (double)g_hk[HK_OMDSS] / fr, (double)g_hk[HK_RSVP] / fr, (double)g_hk[HK_DRAW] / fr);
        memset(g_hk, 0, sizeof g_hk);
        {   // TSC rate from QPC between reports, then the time in the real Map / OMSetRenderTargets
            static LARGE_INTEGER q0; static unsigned long long c0;
            LARGE_INTEGER q, qf; unsigned long long cn = __rdtsc();
            QueryPerformanceCounter(&q); QueryPerformanceFrequency(&qf);
            if (q0.QuadPart && q.QuadPart > q0.QuadPart) g_tsc_ms = (cn - c0) * (unsigned long long)qf.QuadPart / 1000 / (unsigned long long)(q.QuadPart - q0.QuadPart);
            q0 = q; c0 = cn;
        }
        if (fr && g_tsc_ms) {
            char sl[900];
            int len = 0;
            for (int t = 0; t < 5; t++) {
                int best = -1;
                for (unsigned i = 0; i < g_nslow; i++) if (g_slow[i].n && (best < 0 || g_slow[i].ticks > g_slow[best].ticks)) best = (int)i;
                if (best < 0) break;
                SlowMap *k = &g_slow[best];
                len += snprintf(sl + len, sizeof sl - len, "%s map type %u flags 0x%X on %s usage %u bind 0x%X cpu 0x%X %s %u: %u calls %.3f ms per frame",
                                t ? ";" : "", k->type, k->flags, k->dim == 1 ? "buffer" : k->dim == 3 ? "texture2d" : "other", k->usage, k->bind, k->cpu,
                                k->dim == 3 ? "size (w<<16|h)" : "bytes", k->size, k->n, (double)k->ticks / g_tsc_ms / fr);
                k->n = 0;
            }
            log("  main thread in the game's D3D11 calls per frame: Map %.3f ms, OMSetRenderTargets %.3f ms; Maps over 0.2 ms:%s",
                (double)g_map_ticks / g_tsc_ms / fr, (double)g_omrt_ticks / g_tsc_ms / fr, len ? sl : " none");
        }
        g_map_ticks = g_omrt_ticks = 0;
        g_nslow = 0;
        static int owners_done;
        if (!owners_done++) hook_owners(log);
        log("  set-camera hook calls per frame: %.0f", (double)g_cam_calls / fr);
        g_cam_calls = 0;
    }
    if (g_motion) fgm_report(log);
    if (g_mvx_calls) {
        log("  motion vectors for other mods: asked in %u frames (%u calls), none given %u times", g_mvx_frames, g_mvx_calls, g_mvx_none);
        g_mvx_frames = g_mvx_calls = g_mvx_none = 0;
    }
    for (int i = 0; i < 8 && g_callers[i].n; i++) {
        HMODULE mod = NULL;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(UINT_PTR)g_callers[i].ret, &mod);
        GetModuleFileNameA(mod, name, MAX_PATH);
        const char *b = strrchr(name, '\\');
        log("  Present called from %s+0x%X, first game return address on the stack +0x%X: %u times", b ? b + 1 : name,
            g_callers[i].ret - (unsigned)(UINT_PTR)mod, g_callers[i].game_ret, g_callers[i].n);
    }
}
