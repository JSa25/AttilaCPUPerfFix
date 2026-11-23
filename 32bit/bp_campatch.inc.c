// Part of bridgeprobe.c (included there, not compiled on its own): cam_patch(): the pixel shaders' camera for a scaled, jittered scene (bpglue_test.c builds this file too).
// cam_patch: the camera block f as the pixel shaders need it, the scene drawn at rw x rh into the top-left of the w x h
// targets with the sub-pixel jitter (jx, jy) in pixels (x right, y down: vp_adjust's viewport offset). A pixel the shader
// turns into ndc with the screen's size is, in the real view, x = sx ndc + sx - 1 - 2 jx / rw, y = sy ndc + 1 - sy +
// 2 jy / rh. The jitter (Rome 2 DLSS_LOG 44, also at DLAA): lighting, shadows, fog, SSAO and decals turn pixels back into
// positions with these matrices; without it they saw the geometry half a pixel from where the jittered viewport drew it,
// in another direction each frame, and DLSS blurred them (Rome 2: "DLAA is very blurry"). A native integration jitters
// the projection, so both agree.
static void cam_patch(float *f, UINT w, UINT h, UINT rw, UINT rh, float jx, float jy) {
    float sx = (float)w / rw, sy = (float)h / rh, ox = sx - 1 - 2 * jx / rw, oy = 1 - sy + 2 * jy / rh;
    for (int m = 0; m < 2; m++) {   // inverse_projection (byte 272), inv_view_projection (336): rows r_i -> r_i * S
        float *r = f + (m ? 84 : 68);
        for (int i = 0; i < 4; i++, r += 4) { r[3] += r[0] * ox + r[1] * oy; r[0] *= sx; r[1] *= sy; }
    }
    for (int m = 0; m < 2; m++) {   // projection (80), view_projection (144): rows x, y -> S^-1 * M
        float *x = f + (m ? 36 : 20), *y = x + 4, *w4 = x + 12;
        for (int j = 0; j < 4; j++) { x[j] = (x[j] - ox * w4[j]) / sx; y[j] = (y[j] - oy * w4[j]) / sy; }
    }
}
