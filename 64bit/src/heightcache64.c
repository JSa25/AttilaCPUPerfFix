// heightcache64: terrain height query cache for the campaign map, ported from heapfix/heightcache.c.
// Engine: WARSCAPE::TERRAIN_SCENE_NODE::height(VECTOR_2 const&, bool) at 0x32ed0a0 (SysV: rdi this, rsi &pos, dl flag,
// float in xmm0). On the campaign it has no cache (the engine's get_cached_height is battle only: battle_map()+0x8c is
// "is campaign"), so every query walks the quadtree and samples the tiles; the mouse-over ray alone asks up to 4 heights
// per world unit every frame (research/height_uikeep.md). Per (node, x, y, flag) the engine thread remembers the last
// result with a hash of every other value height() reads before evaluating the tiles; a changed value, the editor
// branch, a pause of more than a second, or a tile upgrade / climate change (both hooked) means no cached answer.
//   verify mode (PERFFIX_HQ=verify): the original always runs; a remembered key counts as a hit, and as a mismatch if
//                the original now returns different bits.
//   cache mode (default): a hit returns the remembered result without running the original.
// Other threads always run the original. Battles are left to the engine's own grid.
#include <string.h>
#include "perffix64.h"

#define HQ_SITE 0x32ed0a0u
#define HQ_LEN 0x382u
#define HQ_FNV 0xAACE7599u
#define UPG_SITE 0x32f1830u   // upgrade_tile(VECTOR_2 const&, CA::String const&)
#define UPG_LEN 0x70au
#define UPG_FNV 0xFC3DD6AAu
#define CLI_SITE 0x32f1680u   // set_tile_climate(VECTOR_2 const&, int)
#define CLI_LEN 0x1b0u
#define CLI_FNV 0xA35EBD44u
static const uint8_t HQ_PRO[14] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x48};
static const uint8_t UPG_PRO[17] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xc8, 0x00, 0x00, 0x00};
static const uint8_t CLI_PRO[14] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x58};

volatile LONG64 g_hq[4];   // calls, engine-thread calls, hits, mismatches
volatile LONG g_hq_clears;
static int g_hq_mode;      // 1 verify, 2 cache
static DWORD g_hq_main, g_hq_last_tick;
volatile unsigned char g_hq_dirty;   // set by the upgrade/climate hooks
static uint8_t *g_tramp;   // 3 x 32 bytes: height, upgrade_tile, set_tile_climate originals
void *g_hq_upg_orig, *g_hq_cli_orig;
typedef __attribute__((sysv_abi)) float (*height_fn)(void *self, const float *pos, unsigned flag);
typedef __attribute__((sysv_abi)) char (*vbool_fn)(void *);

#define HQ_SIZE (1 << 16)
struct hq_ent { void *self; uint32_t x, y, flag, result; uint64_t state; };
static struct hq_ent g_tab[HQ_SIZE];

#define AT(p, off, type) (*(type *)((uint8_t *)(p) + (off)))
#ifdef HEIGHTCACHE64_TEST
// the test points these at its own fake globals
uint8_t *g_hq_early = 0; uint32_t *g_hq_zscale = 0; void **g_hq_engine = 0;
#else
static uint8_t *const g_hq_early = (uint8_t *)0x5260d4a;      // terrain_disable_height_tests+0x62: height() returns 0
static uint32_t *const g_hq_zscale = (uint32_t *)0x525d5bc;   // CAMPAIGN_Z_SCALE+0x64: the campaign y divisor
static void **const g_hq_engine = (void **)0x52516d0;          // WS_CONTEXT::m_e_engine
#endif

// Everything besides (x, y, flag) height() reads before its tile evaluation, hashed; 0 = don't cache.
static uint64_t hq_state(void *self) {
    if (*g_hq_early) return 0;                              // the original returns 0 at once
    void *td = AT(self, 0xb68, void *);
    if (!td) return 0;
    void *bm = AT(td, 0x28, void *);                        // TERRAIN_SETUP::battle_map()
    if (!bm || !AT(bm, 0x8c, uint8_t)) return 0;            // battle: the engine's own grid answers
    void *eng = *g_hq_engine;
    if (!eng) return 0;
    char ed = ((vbool_fn)(*(void ***)eng)[0x590 / 8])(eng);   // WS_ENGINE_IMP::is_in_editor_mode
    void *edit = AT(self, 0x1d0, void *);
    if (ed && edit) return 0;                               // editor height: never cached (x86's water rule)
    void *root = AT(self, 0xb78, void *);
    void *tree = root ? AT(root, 0x20, void *) : NULL;
    void *node = tree ? AT(tree, 0x18, void *) : NULL;
    void *tiles = AT(self, 0x2f0, void *) ? AT(AT(self, 0x2f0, void *), 8, void *) : NULL;
    uint64_t v[] = {(uintptr_t)td, (uintptr_t)bm, *g_hq_zscale, (uint8_t)ed, (uintptr_t)edit, AT(td, 0x70, uint32_t),
                    (uintptr_t)root, (uintptr_t)tree, (uintptr_t)node, (uintptr_t)tiles};
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) h = (h ^ v[i]) * 1099511628211ull;
    return h | 1;
}

__attribute__((sysv_abi)) float hq64(void *self, const float *pos, unsigned flag) {
    height_fn orig = (height_fn)(void *)g_tramp;
    InterlockedIncrement64(&g_hq[0]);
    if (GetCurrentThreadId() != g_hq_main) return orig(self, pos, flag);
    g_hq[1]++;
    DWORD now = GetTickCount();
    if (now - g_hq_last_tick > 1000 || g_hq_dirty) { memset(g_tab, 0, sizeof g_tab); g_hq_dirty = 0; g_hq_clears++; }   // a pause or a tile change
    g_hq_last_tick = now;
    uint64_t state = hq_state(self);
    if (!state) return orig(self, pos, flag);
    uint32_t x, y, f = flag & 0xFF;
    memcpy(&x, &pos[0], 4); memcpy(&y, &pos[1], 4);
    uint64_t hh = ((uint64_t)x * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)y * 0xC2B2AE3D27D4EB4Full) ^ ((uintptr_t)self * 0x165667B19E3779F9ull) ^ f;
    struct hq_ent *e = &g_tab[(hh >> 40) & (HQ_SIZE - 1)];
    int hit = e->self == self && e->x == x && e->y == y && e->flag == f && e->state == state;
    if (hit) {
        g_hq[2]++;
        if (g_hq_mode == 2) { float r; memcpy(&r, &e->result, 4); return r; }
    }
    float r = orig(self, pos, flag);
    uint32_t rb; memcpy(&rb, &r, 4);
    if (hit && rb != e->result) g_hq[3]++;
    e->self = self; e->x = x; e->y = y; e->flag = f; e->result = rb; e->state = state;
    return r;
}

// upgrade_tile / set_tile_climate: mark the table dirty, then run the original with every argument register untouched.
__attribute__((naked)) static void hq_upgrade_hook(void) {
    __asm__ volatile("movb $1, g_hq_dirty(%rip)\n\tjmp *g_hq_upg_orig(%rip)");
}
__attribute__((naked)) static void hq_climate_hook(void) {
    __asm__ volatile("movb $1, g_hq_dirty(%rip)\n\tjmp *g_hq_cli_orig(%rip)");
}

// trampoline at t: the n original bytes (whole instructions, no RIP-relative operands) + jmp [rip+0] to site+n
static void make_tramp(uint8_t *t, uint64_t site, const uint8_t *pro, int n) {
    uint64_t back = site + n;
    memcpy(t, pro, n);
    t[n] = 0xFF; t[n + 1] = 0x25; memset(t + n + 2, 0, 4); memcpy(t + n + 6, &back, 8);
}
static __attribute__((unused)) int jmp_abs(uint32_t site, void *to, int len) {   // jmp [rip+0] ; abs64 ; int3 padding up to len
    uint8_t j[17] = {0xFF, 0x25, 0, 0, 0, 0};
    uint64_t a = (uint64_t)(uintptr_t)to;
    memcpy(j + 6, &a, 8);
    memset(j + 14, 0xCC, 3);
    return patch_code((void *)(uintptr_t)site, j, len);
}

#ifdef HEIGHTCACHE64_TEST
void hq64_setup(uint8_t *tramp, int mode) { g_tramp = tramp; g_hq_mode = mode; g_hq_main = GetCurrentThreadId(); g_hq_last_tick = GetTickCount(); }
#else
// mode 1 verify, 2 cache. Runs on the engine thread (SDL_Init). tramp: 96 bytes of executable memory.
// Returns 1 installed (with both invalidation hooks), 0 bytes differ, -1 a thread inside a prologue, -2 VirtualProtect.
int install_heightcache64(uint8_t *tramp, int mode) {
    if (memcmp((void *)(uintptr_t)HQ_SITE, HQ_PRO, 14) || fnv1a((void *)(uintptr_t)HQ_SITE, HQ_LEN) != HQ_FNV ||
        memcmp((void *)(uintptr_t)UPG_SITE, UPG_PRO, 17) || fnv1a((void *)(uintptr_t)UPG_SITE, UPG_LEN) != UPG_FNV ||
        memcmp((void *)(uintptr_t)CLI_SITE, CLI_PRO, 14) || fnv1a((void *)(uintptr_t)CLI_SITE, CLI_LEN) != CLI_FNV) return 0;
    if (thread_inside(HQ_SITE, 14) || thread_inside(UPG_SITE, 17) || thread_inside(CLI_SITE, 14)) return -1;
    g_tramp = tramp; g_hq_mode = mode; g_hq_main = GetCurrentThreadId(); g_hq_last_tick = GetTickCount();
    make_tramp(tramp, HQ_SITE, HQ_PRO, 14);
    make_tramp(tramp + 32, UPG_SITE, UPG_PRO, 17);
    make_tramp(tramp + 64, CLI_SITE, CLI_PRO, 14);
    g_hq_upg_orig = tramp + 32; g_hq_cli_orig = tramp + 64;
    // invalidation hooks first: the cache must never run without them
    if (!jmp_abs(UPG_SITE, (void *)hq_upgrade_hook, 17) || !jmp_abs(CLI_SITE, (void *)hq_climate_hook, 14)) return -2;
    return jmp_abs(HQ_SITE, (void *)hq64, 14) ? 1 : -2;
}
#endif
