// Campaign map: float->int conversions done the old x87 way, switching the FPU rounding mode
// (FSTCW / FLDCW / FISTP / FLDCW) on every call. Each FLDCW serializes the CPU, and these run
// thousands of times a frame (captures/camp_map: ~6% of the main thread in the fog-of-war test,
// ~3% in terrain height sampling, ~2.5% in a per-frame curve table). Each block is replaced in place
// by SSE instructions that give exactly the same integer:
//   FOG1/FOG2 (FUN_10d79aa0, fog-of-war 8x8 sub-cell test): rounding "down" (RC|=0x400), only reached
//     for values > 0, where floor == trunc -> cvttss2si. RC|=0x400 gives down or trunc whatever the
//     game's RC is, and both equal trunc for positive values. Overflow/inf: both give 0x80000000.
//   HEIGHT (FUN_1189b2f0, terrain height grid): rounding "up" (RC|=0x800, = ceil while the game keeps
//     the default RC 00) -> trunc, +1 if the value had a fraction (not for the 0x80000000 overflow
//     value). Every frame fps.c calls campfix_frame(): if the main thread's RC is no longer 00, the
//     original bytes go back (there the game's own result would no longer be ceil).
//   CURVE (FUN_1174a780, per-frame lookup table): FRNDINT rounding "down" then FISTP truncating, of
//     x = clamp(v,0,1)*(n-1) + 0.5 >= 0.5 -> floor(x) == trunc(x) -> cvttss2si.
// The replaced blocks only use their own stack temporaries (dead afterwards) and EAX/XMM0, which the
// following code reloads; see notes in AGENT_LOG.md section 18.
#include <windows.h>
#include <string.h>

struct site {
    const char *name;
    unsigned rva, len;
    const unsigned char *orig, *repl;
    unsigned repl_len;
    int installed;
};

static const unsigned char FOG1_ORIG[36] = {0x50, 0xD9, 0x44, 0x24, 0x08, 0x9B, 0xD9, 0x7C, 0x24, 0x18, 0x8B, 0x44, 0x24, 0x18, 0x0D, 0x00, 0x04, 0x00, 0x00, 0x89, 0x44, 0x24, 0x0C, 0xD9, 0x6C, 0x24, 0x0C, 0xDB, 0x5C, 0x24, 0x10, 0xD9, 0x6C, 0x24, 0x18, 0x58};
static const unsigned char FOG2_ORIG[36] = {0x50, 0xD9, 0x44, 0x24, 0x1C, 0x9B, 0xD9, 0x7C, 0x24, 0x18, 0x8B, 0x44, 0x24, 0x18, 0x0D, 0x00, 0x04, 0x00, 0x00, 0x89, 0x44, 0x24, 0x10, 0xD9, 0x6C, 0x24, 0x10, 0xDB, 0x5C, 0x24, 0x0C, 0xD9, 0x6C, 0x24, 0x18, 0x58};
static const unsigned char HEIGHT_ORIG[36] = {0x50, 0xD9, 0x44, 0x24, 0x18, 0x9B, 0xD9, 0x7C, 0x24, 0x14, 0x8B, 0x44, 0x24, 0x14, 0x0D, 0x00, 0x08, 0x00, 0x00, 0x89, 0x44, 0x24, 0x1C, 0xD9, 0x6C, 0x24, 0x1C, 0xDB, 0x5C, 0x24, 0x20, 0xD9, 0x6C, 0x24, 0x14, 0x58};
static const unsigned char CURVE_ORIG[86] = {0x50, 0xD9, 0x44, 0x24, 0x60, 0x9B, 0xD9, 0x7C, 0x24, 0x5C, 0x8B, 0x44, 0x24, 0x5C, 0x0D, 0x00, 0x04, 0x00, 0x00, 0x89, 0x44, 0x24, 0x68, 0xD9, 0x6C, 0x24, 0x68, 0xD9, 0xFC, 0xD9, 0x5C, 0x24, 0x60, 0xD9, 0x6C, 0x24, 0x5C, 0x58, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x5C, 0xF3, 0x0F, 0x11, 0x44, 0x24, 0x5C, 0x50, 0xD9, 0x44, 0x24, 0x60, 0x9B, 0xD9, 0x7C, 0x24, 0x64, 0x8B, 0x44, 0x24, 0x64, 0x0D, 0x00, 0x0C, 0x00, 0x00, 0x89, 0x44, 0x24, 0x50, 0xD9, 0x6C, 0x24, 0x50, 0xDB, 0x5C, 0x24, 0x4C, 0xD9, 0x6C, 0x24, 0x64, 0x58};

// cvttss2si edx,xmm0; mov [esp+0Ch],edx   (the next game instruction is mov edx,[esp+0Ch])
static const unsigned char FOG1_REPL[] = {0xF3, 0x0F, 0x2C, 0xD0, 0x89, 0x54, 0x24, 0x0C};
// cvttss2si eax,xmm0; mov [esp+8],eax     (next: mov eax,[esp+8])
static const unsigned char FOG2_REPL[] = {0xF3, 0x0F, 0x2C, 0xC0, 0x89, 0x44, 0x24, 0x08};
// cvttss2si eax,xmm1; cmp eax,80000000h; je store; cvtsi2ss xmm0,eax; comiss xmm1,xmm0; jbe store;
// inc eax; store: mov [esp+1Ch],eax       (next: mov eax,[esp+34h]; XMM0 is reloaded before use)
static const unsigned char HEIGHT_REPL[] = {0xF3, 0x0F, 0x2C, 0xC1, 0x3D, 0x00, 0x00, 0x00, 0x80, 0x74, 0x0A,
                                            0xF3, 0x0F, 0x2A, 0xC0, 0x0F, 0x2F, 0xC8, 0x76, 0x01, 0x40,
                                            0x89, 0x44, 0x24, 0x1C};
// cvttss2si ecx,xmm0; mov [esp+48h],ecx   (next: mov ecx,[esp+48h])
static const unsigned char CURVE_REPL[] = {0xF3, 0x0F, 0x2C, 0xC8, 0x89, 0x4C, 0x24, 0x48};

static struct site SITES[] = {
    {"fog test 1", 0xD79AC8, 36, FOG1_ORIG, FOG1_REPL, sizeof FOG1_REPL, 0},
    {"fog test 2", 0xD79B12, 36, FOG2_ORIG, FOG2_REPL, sizeof FOG2_REPL, 0},
    {"terrain height", 0x189B363, 36, HEIGHT_ORIG, HEIGHT_REPL, sizeof HEIGHT_REPL, 0},
    {"curve table", 0x174A8BD, 86, CURVE_ORIG, CURVE_REPL, sizeof CURVE_REPL, 0},
};
#define NSITES (int)(sizeof SITES / sizeof SITES[0])
static unsigned char *g_campfix_base;
static void (*g_campfix_log)(const char *fmt, ...);
static int g_campfix_ab;          // campfix_ab.txt at start: on/off alternate every 10 s
static int g_height_disabled;     // the rounding mode changed: never put the height patch back
extern volatile LONG g_campfix_state;  // fps.c logs it: -1 not installed, 0 off, 1 on

static void write_code(unsigned char *at, const unsigned char *bytes, unsigned n, unsigned total) {
    DWORD old;
    VirtualProtect(at, total, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, n);
    memset(at + n, 0x90, total - n);  // straight-line blocks: no jump lands inside them
    VirtualProtect(at, total, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, total);
}

static unsigned short fpu_cw(void) {
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    return cw;
}

// Called by fps.c at the start of every frame, on the main thread (outside the patched functions).
// Called by fps.c at the start of every frame, on the main thread (outside the patched functions),
// so this is also the one safe place to switch the patches on and off for A/B runs.
void campfix_frame(void) {
    struct site *h = &SITES[2];
    if (h->installed && (fpu_cw() & 0xC00) != 0) {
        write_code(g_campfix_base + h->rva, h->orig, h->len, h->len);
        h->installed = 0;
        g_height_disabled = 1;
        g_campfix_log("campaign fix: the game changed the FPU rounding mode - terrain height patch removed");
    }
    if (!g_campfix_ab) return;
    int want = (GetTickCount() / 10000) % 2 == 0;
    if (want == g_campfix_state) return;
    for (int i = 0; i < NSITES; i++) {
        struct site *s = &SITES[i];
        if (i == 2 && g_height_disabled) continue;
        if (want && !s->installed && memcmp(g_campfix_base + s->rva, s->orig, s->len) == 0) {
            write_code(g_campfix_base + s->rva, s->repl, s->repl_len, s->len);
            s->installed = 1;
        } else if (!want && s->installed) {
            write_code(g_campfix_base + s->rva, s->orig, s->len, s->len);
            s->installed = 0;
        }
    }
    g_campfix_state = want;
}

// Must run on the game's main thread (DllMain). Returns the number of blocks patched.
int install_campfix(HMODULE m, int ab, void (*log)(const char *fmt, ...)) {
    unsigned char *b = (unsigned char *)m;
    g_campfix_base = b;
    g_campfix_log = log;
    g_campfix_ab = ab;
    int n = 0;
    for (int i = 0; i < NSITES; i++) {
        struct site *s = &SITES[i];
        if (memcmp(b + s->rva, s->orig, s->len) != 0) { log("campaign fix: %s differs from this game version - skipped", s->name); continue; }
        if (i == 2 && (fpu_cw() & 0xC00) != 0) { log("campaign fix: FPU rounding mode is not the default - terrain height skipped"); continue; }
        write_code(b + s->rva, s->repl, s->repl_len, s->len);
        s->installed = 1;
        n++;
    }
    if (n) g_campfix_state = 1;
    log("campaign fix: %d of %d x87 rounding-mode blocks replaced (fog of war, terrain height, curve table)%s", n, NSITES,
        ab ? " - A/B mode: on and off alternate every 10 s" : "");
    return n;
}
