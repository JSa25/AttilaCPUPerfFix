// Sound crash guard (reports/2026-11-11_massive_battle_aaron, AGENT_LOG 170): the game's sound engine (Wwise, inside
// empire.retail.dll) gives each mixer an output buffer from its fixed lower-engine pool (FUN_1196fdb0: max 16 MB).
// When that pool is full, the mixer's setup (FUN_119cade0) sets the buffer pointer and frame count to 0 but keeps
// the old channel count, and the voice is still mixed: FUN_119d8db0 (gain-ramp mix, cdecl(src, dst, gain0, gain1,
// frames)) then writes to address 0 and the game closes (empire+0x19D8EBB, read of 00000000; seen in huge MK1212
// battles and once leaving a battle). Here the mix's 5 call sites go through mg_mix, which skips a mix whose source
// or destination is a null pointer (plus a small offset): that voice is silent for one buffer instead.
#include <windows.h>
#include <string.h>
#include "gamerva.h"

#define RVA_MIX 0x19D8DB0u           // FUN_119d8db0, checked by its code (absolute addresses unrelocated)
#define LEN_MIX 0x124u
#define FNV_MIX 0x98A8F259u          // 1.6.0 Steam 69CE1786
static const DWORD SITES[] = {0x19CB6C5u, 0x19CB758u, 0x19D2443u, 0x19D251Au, 0x19D25B6u};  // call FUN_119d8db0
#define NSITES (sizeof SITES / sizeof SITES[0])
#define LOW 0x10000u                 // no buffer lives in the first 64 KB: Windows never maps it

typedef void(__cdecl *mix_fn)(float *src, float *dst, float gain0, float gain1, unsigned frames);
static mix_fn g_mix;
volatile LONG g_soundguard_skips;  // diag.c / the dev log can report it; the first skip is logged once
static void (*g_sg_log)(const char *fmt, ...);

static void __cdecl mg_mix(float *src, float *dst, float gain0, float gain1, unsigned frames) {
    if ((UINT_PTR)src < LOW || (UINT_PTR)dst < LOW) {
        if (InterlockedIncrement(&g_soundguard_skips) == 1 && g_sg_log)
            g_sg_log("sound: a sound had no buffer (the game's sound memory pool is full) - that sound went silent for a moment instead of crashing the game");
        return;
    }
    g_mix(src, dst, gain0, gain1, frames);
}

int install_soundguard(HMODULE m, void (*log)(const char *fmt, ...)) {
    unsigned char *b = (unsigned char *)m;
    DWORD mix = game_rva(b, RVA_MIX);  // not in the GOG table: 0 there, so the check below fails safely
    if (!mix || fnv1a_unreloc(b, mix, LEN_MIX) != FNV_MIX) {
        log("sound crash guard (+0x%X) differs from this game version - not installed", RVA_MIX);
        return 0;
    }
    for (unsigned i = 0; i < NSITES; i++) {
        unsigned char *s = b + SITES[i];
        if (s[0] != 0xE8 || s + 5 + *(LONG *)(s + 1) != b + mix) {
            log("sound crash guard: call site +0x%lX differs - not installed", SITES[i]);
            return 0;
        }
    }
    g_mix = (mix_fn)(b + mix);
    g_sg_log = log;
    for (unsigned i = 0; i < NSITES; i++) {
        unsigned char *s = b + SITES[i];
        DWORD old;
        if (!VirtualProtect(s, 5, PAGE_EXECUTE_READWRITE, &old)) {
            log("sound crash guard: VirtualProtect failed (%lu) - %u of %u call sites guarded", GetLastError(), i, (unsigned)NSITES);
            return 0;  // shortcut: sites already retargeted stay guarded; mg_mix forwards to the same function
        }
        LONG rel = (LONG)((unsigned char *)mg_mix - (s + 5));
        memcpy(s + 1, &rel, 4);  // the call stays a call, to mg_mix
        VirtualProtect(s, 5, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    log("sound crash guard: a sound without a buffer goes silent for a moment instead of crashing the game");
    return 1;
}
