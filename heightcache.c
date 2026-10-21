// Terrain height query cache (campaign map). FUN_118f9150 (thiscall: ECX = terrain, stack = pointer to
// (x, y) floats and a char flag, ret 8, float result in ST0) walks a quadtree of terrain patches and for
// the patches under the point samples 3 corners x 4 grid cells each: ~10% of the campaign main thread
// (captures/camp_x87b), called from many campaign systems every frame.
// Per (terrain, x, y, flag) the main thread remembers the last result, together with a hash of every
// other value the function reads before its patch evaluation (hq_state); a changed value, the water
// branch, or a pause of more than a second (loading) means no cached answer.
// Verified in game (captures/camp_hq): 575 queries per frame, 82.8% repeats, 0 mismatches.
//   verify mode: the original always runs; a remembered key counts as a hit, and as a mismatch if the
//                original now returns different bits (= some input other than the key changed).
//   cache mode:  a hit returns the remembered result without running the original.
// Other threads (the flag makes it take a critical section, so it is called off the main thread too)
// always run the original. Counters: g_hq[0] calls, [1] main-thread calls, [2] hits, [3] mismatches.
#include <windows.h>
#include <string.h>

#define RVA_HEIGHTQ 0x18F9150
static const unsigned char HEIGHTQ_PROLOGUE[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};  // push ebp; mov ebp,esp; and esp,-8

volatile LONG g_hq[4];
static int g_hq_mode;  // 1 verify, 2 cache
static DWORD g_hq_main;
static unsigned char g_hq_tramp[16];
typedef float(__thiscall *heightq_fn)(void *, const float *, int);

#define HQ_SIZE (1 << 16)
struct hq_ent { void *terrain; unsigned x, y, flag, result, state; };
static struct hq_ent g_hq_tab[HQ_SIZE];
static unsigned char *g_hq_base;
static DWORD g_hq_last_tick;

#define AT(p, off, type) (*(type *)((unsigned char *)(p) + (off)))
typedef char(__thiscall *vbool_fn)(void *);

// Everything besides (x, y, flag) the original reads before its patch evaluation (FUN_118f9150 and the
// getters it calls: FUN_100c0130, FUN_10161660, FUN_1059d310, FUN_100c0100, vtable +0x2C4 of
// DAT_12a21d18, FUN_107c0b20), hashed. Returns 0 when the result must not be cached: the water branch
// (vtable +0x2C4 true and this+0x128 set) also uses a global map scale (FUN_118ee960) not checked here.
static unsigned hq_state(void *terrain) {
    unsigned char *b = g_hq_base;
    unsigned char early = AT(b + 0x2A78008, 0x48, unsigned char);  // FUN_100c0130(0x12A78008)
    void *td = AT(terrain, 0x794, void *);
    if (!td) return 1;
    void *mode_obj = AT(td, 0x18, void *);                          // FUN_10161660(td)
    unsigned char mode = mode_obj ? AT(mode_obj, 0x7C, unsigned char) : 0xEE;  // FUN_1059d310
    unsigned ydiv = AT(b + 0x2A77BF8, 0x48, unsigned);              // FUN_100c0100(0x12A77BF8)
    void *game = AT(b + 0x2A21D18, 0, void *);                      // DAT_12a21d18
    char sw = ((vbool_fn)(*(void ***)game)[0x2C4 / 4])(game);
    void *water = AT(terrain, 0x128, void *);
    if (sw && water) return 0;
    unsigned wf = AT(td, 0x7C, unsigned);                           // FUN_107c0b20(td)
    void *root = AT(terrain, 0x79C, void *);
    void *tree = root ? AT(root, 0x18, void *) : NULL;
    unsigned h = 2166136261u;
    unsigned v[] = {early, (unsigned)(size_t)td, mode, ydiv, (unsigned char)sw, (unsigned)(size_t)water, wf,
                    (unsigned)(size_t)root, (unsigned)(size_t)tree};
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) h = (h ^ v[i]) * 16777619u;
    return h | 1;
}

float __cdecl heightq_wrap(void *terrain, const float *pos, int flag) {
    heightq_fn orig = (heightq_fn)(void *)g_hq_tramp;
    g_hq[0]++;
    if (GetCurrentThreadId() != g_hq_main) return orig(terrain, pos, flag);
    g_hq[1]++;
    DWORD now = GetTickCount();
    if (now - g_hq_last_tick > 1000) memset(g_hq_tab, 0, sizeof g_hq_tab);  // a pause: loading screen, new map
    g_hq_last_tick = now;
    unsigned state = hq_state(terrain);
    if (!state) return orig(terrain, pos, flag);
    unsigned x, y, f = (unsigned)flag & 0xFF;
    memcpy(&x, &pos[0], 4);
    memcpy(&y, &pos[1], 4);
    unsigned h = (x * 0x9E3779B1u ^ y * 0x85EBCA77u ^ (unsigned)(size_t)terrain * 0xC2B2AE3Du ^ f) >> 16;
    struct hq_ent *e = &g_hq_tab[h & (HQ_SIZE - 1)];
    int hit = e->terrain == terrain && e->x == x && e->y == y && e->flag == f && e->state == state;
    if (hit) {
        g_hq[2]++;
        if (g_hq_mode == 2) { float r; memcpy(&r, &e->result, 4); return r; }
    }
    float r = orig(terrain, pos, flag);
    unsigned rb;
    memcpy(&rb, &r, 4);
    if (hit && rb != e->result) g_hq[3]++;
    e->terrain = terrain; e->x = x; e->y = y; e->flag = f; e->result = rb; e->state = state;
    return r;
}

__attribute__((naked)) static void heightq_stub(void) {
    __asm__ volatile(
        "pushl 8(%esp)\n\t"   // flag
        "pushl 8(%esp)\n\t"   // pos (was at +4, +4 for the push)
        "pushl %ecx\n\t"      // terrain
        "call _heightq_wrap\n\t"
        "addl $12, %esp\n\t"  // the float result stays in ST0
        "ret $8\n\t");
}

// mode 1 verify, 2 cache. Must run on the game's main thread. Returns 1 if installed.
int install_heightcache(HMODULE m, int mode, void (*log)(const char *fmt, ...)) {
    unsigned char *site = (unsigned char *)m + RVA_HEIGHTQ;
    if (memcmp(site, HEIGHTQ_PROLOGUE, 6) != 0) { log("height cache: code differs from this game version - not installed"); return 0; }
    g_hq_mode = mode;
    g_hq_main = GetCurrentThreadId();
    g_hq_base = (unsigned char *)m;
    DWORD old;
    if (!VirtualProtect(g_hq_tramp, sizeof g_hq_tramp, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(g_hq_tramp, site, 6);
    g_hq_tramp[6] = 0xE9;
    DWORD rel = (DWORD)((site + 6) - (g_hq_tramp + 11));
    memcpy(g_hq_tramp + 7, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), g_hq_tramp, sizeof g_hq_tramp);
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE9;
    rel = (DWORD)((unsigned char *)heightq_stub - (site + 5));
    memcpy(site + 1, &rel, 4);
    site[5] = 0x90;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    log("height cache installed (%s)", mode == 2 ? "cache" : "verify only");
    return 1;
}
