// Bridge cost probe (dev builds only; AGENT_LOG 228): which part of the DLSS add-on's frame costs the game its frames.
// The settings sweep (bridgeprobe.c: sweep.txt, "probe=<mode>[+<mode>] delay=<us>") changes one part of the bridge for
// a window; in every window each frame's main-thread time is split by step (fgbridge.c) and logged as one "probe" line
// of key=value pairs (ms per frame). tools/bridge_probe.py writes the sweep and reads those lines out of heapfix.log.
#pragma once
enum {
    FGB_PR_IDLE = 1,      // nothing handed to the helper: no copies, Signal, Flush or pacing (the picture freezes)
    FGB_PR_NOPICKUP = 2,  // DLSS inputs handed over, its output not waited for or copied back (the render stays in a corner)
    FGB_PR_PREV = 4,      // the previous frame's DLSS output goes into this frame, no CPU wait (pipelined; no sharpening)
    FGB_PR_NOPACE = 8,    // no pacing wait at Present
    FGB_PR_NOHIDP = 16,   // bridgeprobe.c: the game's hidden swapchain not presented (AGENT_LOG 109's fix switched off)
};
// main-thread steps: the whole frame, the injected delay, the HUD-less step and its parts, the Present step and its
// parts, then bridgeprobe.c's own: the F11 panel, the hidden swapchain's Present, the frame end (motion pass hooks)
enum { PS_FRAME, PS_DELAY, PS_HUD, PS_HPREV, PS_HCOPY, PS_HMV, PS_HSUBMIT, PS_HWAIT, PS_HBACK,
       PS_PRES, PS_PCOPY, PS_PSUBMIT, PS_PACE, PS_PANEL, PS_HIDP, PS_END, PS_N };
void fgb_probe_set(unsigned flags, unsigned delay_us);   // the window's mode; delay_us: a busy wait at the HUD-less point
void fgb_probe_mark(void);                               // the window's measurements start 30 frames from here
void fgb_probe_end(void);                                // modes off, nothing measured
unsigned fgb_probe_flags(void);
void fgb_probe_lap(int stage, LARGE_INTEGER *t);         // the time since *t goes to the stage; *t = now
void fgb_probe_commit(void);                             // once per frame, after its last step (main thread)
int fgb_probe_line(char *out, int cap);                  // the window so far (main thread)
