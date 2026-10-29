/* Frame bridge protocol (DLSS_PLAN plan B): the 32-bit game side (heapfix/fgbridge.c) shares textures and three fences
   with the 64-bit helper (helper64.exe bridge ...), by name, plus this small block of shared memory.
   Names (pid = the game's process id): Local\fgb_<pid>_map (this block), _wake (event), fences _fence / _fsr / _fdone,
   textures _final, _hudless, _depth, _mvec, _srout0, _srout1. One fence per signaller, so each value only grows:
     _fence  game:   2n+1 = frame n's HUD-less colour, depth, motion vectors written; 2n+2 = its final colour (with UI)
     _fsr    helper SR queue: n+1 = DLSS SR output of frame n written to _srout<n % 2> (the game's GPU waits for it,
             copies it into the back buffer, and the UI draws on top)
     _fdone  helper present queue: n+1 = frame n read; the game's GPU waits for it before writing frame n+1
   A frame without a HUD-less point (menus) has no SR: 2n+2 covers everything and the frame has reset set.
   The helper runs SR on its own thread and queue so a blocking Present never delays the next frame's SR. */
#pragma once
#include <stdint.h>

#define FGB_PROTO 4
#define FGB_QUIT 0xFFFFFFFFu

typedef struct {
    uint32_t proto, width, height, format, hwnd, luid_lo;
    int32_t luid_hi;
    volatile uint32_t frames;      /* game: frames published (frame n is ready when frames == n + 1); FGB_QUIT ends */
    volatile uint32_t fg_frames;   /* game: frame generation 0 off, 1 = 2x, 3 = 4x */
    volatile uint32_t reset;       /* game: frame n has no history (camera cut, no HUD-less copy this frame) */
    volatile uint32_t sr_frames;   /* game: SR inputs of frame n published (sr_frames == n + 1) */
    volatile uint32_t sr_on;       /* game: DLSS SR on (render_w x render_h in the top-left of the shared inputs) */
    volatile uint32_t render_w, render_h;
    volatile float jitter_x, jitter_y;   /* game: frame n's sub-pixel offset (pixels, x right, y down) */
    volatile uint32_t done;        /* helper: frames presented */
    volatile uint32_t helper_ok;   /* helper: Streamline and the shared objects are up */
    volatile uint32_t error;       /* helper: failed (message in its log) */
    volatile uint32_t sr_done;     /* helper: SR of frame n taken (sr_done == n + 1; _fsr n+1 follows on the GPU) */
    volatile uint32_t shown;       /* helper: frames shown so far (generated ones included; the F12 panel's readout) */
    volatile uint32_t fg_max;      /* helper, before helper_ok: generated frames this GPU allows (NVIDIA's rules, from
                                      Streamline: 0 = no frame generation (RTX 20 / 30), 1 = 2x (RTX 40), 3 = up to 4x) */
    uint32_t fg_test_limit;        /* game, dev test only: 0 = none, else the GPU is treated as allowing limit - 1 */
    volatile uint32_t frame_limit_us;   /* game: NVIDIA Reflex frame limit in microseconds per frame as Reflex counts
                                           them (shown frames, generated ones included); 0 = none. Was padding: a game
                                           side that does not know it leaves it 0 */
    uint32_t pad1[8];
    float vp[16], prev_vp[16];     /* game: view-projection of frame n and n-1 (row-major, clip = M x [x y z 1]) */
} FgbShared;
_Static_assert(sizeof(FgbShared) == 256, "bridge block layout");
