// soundguard64: sound crash guard, ported from heapfix/soundguard.c. The game's sound engine (Wwise) still mixes a
// voice whose mixer got no output buffer (its pool was full): the gain-ramp mix then writes to address 0 and the game
// closes (x86: huge 1212 battles). Here a mix whose source or destination is a null pointer (plus a small offset) is
// skipped: that voice is silent for one buffer instead.
// Engine: CAkMixer::MixChannelSIMD(float *src, float *dst, float gain0, float gain1, unsigned frames) at 0x36ff800,
// static (SysV: rdi, rsi, xmm0, xmm1, edx). x86 retargeted its 5 call sites; here the function's entry is hooked, which
// covers every caller. Its first two instructions (xorps xmm2, xmm2 ; mov edx, edx: 5 bytes, position-independent) move
// to a trampoline; the entry becomes a jmp rel32 to a relay near the engine.
// Not established on x64: that the pool-full case happens in this build. The guard costs two compares per mix.
#include <string.h>
#include "perffix64.h"

#define SG_MIX 0x36ff800u
#define SG_LEN 0x120u
#define SG_FNV 0xA8849690u   // FNV-1a of the 0x120 bytes in the ELF (soundguard test prints and checks it)
static const uint8_t SG_PRO[5] = {0x0f, 0x57, 0xd2, 0x89, 0xd2};
#define SG_LOW 0x10000u      // no buffer lives in the first 64 KB: Windows never maps it

typedef __attribute__((sysv_abi)) void (*sg_mix_fn)(float *src, float *dst, float gain0, float gain1, unsigned frames);
sg_mix_fn sg64_orig;                 // the trampoline
volatile LONG g_soundguard_skips;

__attribute__((sysv_abi)) void sg64_mix(float *src, float *dst, float gain0, float gain1, unsigned frames) {
    if ((uintptr_t)src < SG_LOW || (uintptr_t)dst < SG_LOW) { InterlockedIncrement(&g_soundguard_skips); return; }
    sg64_orig(src, dst, gain0, gain1, frames);
}

#ifndef SOUNDGUARD64_TEST
// code: 48 bytes of executable memory within +-2 GB of the engine (16 relay, 32 trampoline).
// 1 installed, 0 the engine's code differs, -1 a thread inside the entry or no near memory, -2 VirtualProtect failed.
int install_soundguard64(uint8_t *code) {
    if (memcmp((void *)(uintptr_t)SG_MIX, SG_PRO, 5) || fnv1a((void *)(uintptr_t)SG_MIX, SG_LEN) != SG_FNV) return 0;
    int64_t d = (int64_t)((uintptr_t)code - SG_MIX);
    if (!code || d < -0x7FF00000ll || d > 0x7FF00000ll || thread_inside(SG_MIX, 5)) return -1;
    uint64_t to = (uint64_t)(uintptr_t)sg64_mix, back = SG_MIX + 5;
    code[0] = 0xFF; code[1] = 0x25; memset(code + 2, 0, 4); memcpy(code + 6, &to, 8);            // relay: jmp [rip+0] -> sg64_mix
    uint8_t *t = code + 16;
    memcpy(t, SG_PRO, 5);
    t[5] = 0xFF; t[6] = 0x25; memset(t + 7, 0, 4); memcpy(t + 11, &back, 8);                     // trampoline: 5 bytes, jmp [rip+0] -> entry+5
    sg64_orig = (sg_mix_fn)(void *)t;
    uint8_t j[5] = {0xe9};
    int32_t rel = (int32_t)((int64_t)(uintptr_t)code - ((int64_t)SG_MIX + 5));
    memcpy(j + 1, &rel, 4);
    return patch_code((void *)(uintptr_t)SG_MIX, j, 5) ? 1 : -2;
}
#endif
