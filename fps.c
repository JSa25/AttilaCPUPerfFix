// Frame hook on the game's per-frame function (FUN_100c1950, called once per main-loop iteration).
// Release builds use it only to run the per-frame safety checks (campfix.c, x87fix.c).
// Dev builds also timestamp every call: while <game folder>\fps_on.txt exists, a background thread
// writes one line per second to fps_log.csv, and a summary for the whole recording when it stops.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>

#define RVA_FRAME 0x0C1950
static const unsigned char FRAME_PROLOGUE[10] = {0x56, 0x8B, 0xF1, 0x80, 0xBE, 0xC8, 0x00, 0x00, 0x00, 0x00};

static unsigned char g_tramp[16];
void *g_fps_tramp_ptr = g_tramp;

void (*g_frame_check)(void);  // set by heapfix.c: per-frame checks on the main thread (campfix.c, x87fix.c)
volatile LONG g_campfix_state = -1;  // campfix.c: -1 not installed, 0 off, 1 on (A/B runs alternate)

void fps_record(void) {
    if (g_frame_check) g_frame_check();
}

// Saves every register and flag, records the frame, then runs the original function via the trampoline.
__attribute__((naked)) static void fps_stub(void) {
    __asm__ volatile(
        "pushal\n\t"
        "pushfl\n\t"
        "call _fps_record\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *_g_fps_tramp_ptr\n\t");
}


// Returns 1 if the frame hook was installed.
int install_fps(HMODULE m, const wchar_t *dir, void (*log)(const char *fmt, ...)) {
    unsigned char *site = (unsigned char *)m + RVA_FRAME;
    if (memcmp(site, FRAME_PROLOGUE, sizeof FRAME_PROLOGUE) != 0) {
        log("frame function at +0x%X does not match this game version - FPS logging off", RVA_FRAME);
        return 0;
    }
    DWORD old;
    if (!VirtualProtect(g_tramp, sizeof g_tramp, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(g_tramp, site, sizeof FRAME_PROLOGUE);  // push esi; mov esi,ecx; cmp byte [esi+0C8h],0
    g_tramp[10] = 0xE9;
    DWORD rel = (DWORD)((site + 10) - (g_tramp + 15));
    memcpy(g_tramp + 11, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), g_tramp, sizeof g_tramp);
    if (!VirtualProtect(site, 10, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE9;
    rel = (DWORD)((unsigned char *)fps_stub - (site + 5));
    memcpy(site + 1, &rel, 4);
    memset(site + 5, 0x90, 5);  // never executed (the jmp leaves first); keeps the disassembly tidy
    VirtualProtect(site, 10, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 10);
    (void)dir;
    log("frame hook ready (per-frame safety checks)");
    return 1;
}
