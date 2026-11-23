// batchfix64: draw items with an equal non-zero per-item variant value batch like everything else, ported from
// heapfix/batchfix.c. Six engine draw loops (research/render_sorts_batch_prefetch.md): RIGID_SCENE_NODE_V2::
// add_list_instances<WEIGHTED_INSTANCE> and RIGID_MESH::add_list_instances_depth<T> for five rigid types. Their
// "same material" test also demands tracker+0x20 (the previous item's variant) == 0, and clang copied the reset of
// tracker+0x20 into three places, so after any item with a non-zero variant every item flushes and redoes the
// material setup. Patch, per loop (as on x86):
//   cmpl $0,0x20(trk); jne diff   ->  trk+0x20 == 0 or item+field == trk+0x20 ? same : diff   (material checked before)
//   reset on the "same, batch not full" path      ->  removed (x86: AL == 0, no reset)
//   reset after a flush, before `test flag`       ->  only when flag != 0 (the material is set up again)
//   reset on the "different, nothing pending" path  ->  kept
// At the test the item pointer is in rax, and rcx and the flags are dead on both targets (checked per site).
#include <string.h>
#include "perffix64.h"

struct bsite {
    uint32_t test, same, diff, field, field_at, flush_reset, same_reset;
    uint8_t r13, flag_r14;     // tracker in r13 (else rbx); flag register r14b (else r12b)
    uint8_t field_load[7], field_load_len;
};
#define NB 6
static const struct bsite BS[NB] = {
    {0x354bb9e, 0x354bba5, 0x354bbc0, 0x100, 0x354bd40, 0x354bc64, 0x354bd20, 1, 1, {0x44, 0x8b, 0xb0, 0x00, 0x01, 0x00, 0x00}, 7},   // WEIGHTED
    {0x3391ec0, 0x3391ec6, 0x3391ee0, 0x44, 0x3392035, 0x3391f62, 0x3391fe2, 0, 1, {0x44, 0x8b, 0x70, 0x44}, 4},                      // RIGID_INSTANCE_V2
    {0x351fcd0, 0x351fcd6, 0x351fcf0, 0x44, 0x351fe55, 0x351fd72, 0x351fe2d, 0, 0, {0x44, 0x8b, 0x60, 0x44}, 4},                      // CAMPAIGN_VEGETATION
    {0x3526360, 0x3526366, 0x3526380, 0x44, 0x35264e5, 0x3526402, 0x35264bd, 0, 0, {0x44, 0x8b, 0x60, 0x44}, 4},                      // DECAL_DIRT
    {0x35284c0, 0x35284c6, 0x35284e0, 0x44, 0x3528645, 0x3528562, 0x352861d, 0, 0, {0x44, 0x8b, 0x60, 0x44}, 4},                      // VERTEX_CLOTH_V2
    {0x3537df0, 0x3537df6, 0x3537e10, 0x40, 0x3537f75, 0x3537e92, 0x3537f4d, 0, 0, {0x44, 0x8b, 0x60, 0x40}, 4},                      // DECAL
};
int g_batch_state[NB];
const int batchfix64_count = NB;
const char *const g_batch_label[NB] = {"soldiers", "rigid parts", "campaign vegetation", "decal dirt", "cloth", "decals"};
static const uint8_t TEST_R13[7] = {0x41, 0x83, 0x7d, 0x20, 0x00, 0x75, 0x1b}, TEST_RBX[6] = {0x83, 0x7b, 0x20, 0x00, 0x75, 0x1a};
static const uint8_t RESET_R13[8] = {0x41, 0xc7, 0x45, 0x20, 0, 0, 0, 0}, RESET_RBX[7] = {0xc7, 0x43, 0x20, 0, 0, 0, 0};
static const uint8_t FLAG_R14[3] = {0x45, 0x84, 0xf6}, FLAG_R12[3] = {0x45, 0x84, 0xe4};

static void rel32(uint8_t *at, uint64_t target) { int32_t r = (int32_t)(int64_t)(target - ((uint64_t)(uintptr_t)at + 4)); memcpy(at, &r, 4); }
static __attribute__((unused)) int reach(uint64_t from, uint64_t to) { int64_t d = (int64_t)(to - from); return d > -0x7FFFFF00ll && d < 0x7FFFFF00ll; }

// Writes both stubs of site i at code (64 bytes: the test at 0, the after-flush reset at 32). `same`, `diff` and
// `resume` are the jump targets (the engine addresses, or probes in the test).
void batchfix64_build(uint8_t *code, int i, uint64_t same, uint64_t diff, uint64_t resume) {
    const struct bsite *s = &BS[i];
    uint8_t *p = code;
    if (s->r13) { memcpy(p, (const uint8_t[]){0x41, 0x83, 0x7d, 0x20, 0x00}, 5); p += 5; } else { memcpy(p, (const uint8_t[]){0x83, 0x7b, 0x20, 0x00}, 4); p += 4; }
    *p++ = 0x0f; *p++ = 0x84; rel32(p, same); p += 4;                       // je same
    *p++ = 0x8b; *p++ = 0x88; memcpy(p, &s->field, 4); p += 4;              // mov ecx,[rax+field]
    if (s->r13) { memcpy(p, (const uint8_t[]){0x41, 0x3b, 0x4d, 0x20}, 4); p += 4; } else { memcpy(p, (const uint8_t[]){0x3b, 0x4b, 0x20}, 3); p += 3; }   // cmp ecx,[trk+20h]
    *p++ = 0x0f; *p++ = 0x84; rel32(p, same); p += 4;                       // je same
    *p++ = 0xe9; rel32(p, diff);                                            // jmp diff
    p = code + 32;
    memcpy(p, s->flag_r14 ? FLAG_R14 : FLAG_R12, 3); p += 3;                // test flag,flag
    const uint8_t *rs = s->r13 ? RESET_R13 : RESET_RBX; int rl = s->r13 ? 8 : 7;
    *p++ = 0x74; *p++ = (uint8_t)rl;                                        // jz over the reset
    memcpy(p, rs, rl); p += rl;                                             // movl $0,0x20(trk)
    *p++ = 0xe9; rel32(p, resume);                                          // back to the engine's own test flag
}

// The 5-byte `jmp rel32` written at engine address `site` (offset from the site, not from this buffer), nop-padded to 8.
void batchfix64_jmp(uint8_t out[8], uint64_t site, uint64_t target) {
    int32_t r = (int32_t)(int64_t)(target - (site + 5));
    out[0] = 0xe9; memcpy(out + 1, &r, 4); memset(out + 5, 0x90, 3);
}

// Loop i as found. `at` maps an engine address to where its bytes are (the engine itself, or a copy in the test).
// SITE_OURS = the game's own bytes at every place the patch reads or writes (ours to take); SITE_LOADER = the loader's
// "parts with the same shader set stay in one draw" patch (Sol Invictus's: the jne of the test and the whole
// same-path reset are nops, everything else as shipped); SITE_DIFFERS = anything else.
int batchfix64_classify(int i, const uint8_t *(*at)(uint32_t va)) {
    const struct bsite *s = &BS[i];
    const uint8_t *rs = s->r13 ? RESET_R13 : RESET_RBX, *tb = s->r13 ? TEST_R13 : TEST_RBX;
    int rl = s->r13 ? 8 : 7, tl = s->r13 ? 7 : 6;
    const uint8_t *t = at(s->test), *sr = at(s->same_reset);
    if (memcmp(at(s->flush_reset), rs, rl) || memcmp(at(s->flush_reset + rl), s->flag_r14 ? FLAG_R14 : FLAG_R12, 3) ||
        memcmp(at(s->field_at), s->field_load, s->field_load_len) || memcmp(t, tb, tl - 2)) return SITE_DIFFERS;
    if (!memcmp(t + tl - 2, tb + tl - 2, 2) && !memcmp(sr, rs, rl)) return SITE_OURS;
    int nops = t[tl - 2] == 0x90 && t[tl - 1] == 0x90;
    for (int k = 0; k < rl; k++) nops &= sr[k] == 0x90;
    return nops ? SITE_LOADER : SITE_DIFFERS;
}

_Static_assert(NB <= BATCH_MAX, "the stub block has room for every loop");
#ifndef BATCHFIX64_TEST
static const uint8_t *engine_at(uint32_t va) { return (const uint8_t *)(uintptr_t)va; }

// code: 4 KB of executable memory within +-2 GB of the engine (the caller allocates it before the pause).
// Returns the number of loops patched; a loop whose bytes differ is left as it is.
int install_batchfix64(uint8_t *code) {
    if (!code || !reach((uintptr_t)code, 0x3300000) || !reach((uintptr_t)code + 4096, 0x3600000)) return -1;
    int n = 0;
    for (int i = 0; i < NB; i++) {
        const struct bsite *s = &BS[i];
        g_batch_state[i] = batchfix64_classify(i, engine_at);
        if (g_batch_state[i] != SITE_OURS) continue;
        if (thread_inside(s->test, 7) || thread_inside(s->flush_reset, 8) || thread_inside(s->same_reset, 8)) { g_batch_state[i] = SITE_BUSY; continue; }
        int tl = s->r13 ? 7 : 6, rl = s->r13 ? 8 : 7;
        uint8_t *st = code + 64 * i;
        batchfix64_build(st, i, s->same, s->diff, s->flush_reset + rl);
        uint8_t jt[8], jr[8], nop[8];
        batchfix64_jmp(jt, s->test, (uintptr_t)st);
        batchfix64_jmp(jr, s->flush_reset, (uintptr_t)st + 32);
        static const uint8_t NOP8[8] = {0x0f, 0x1f, 0x84, 0, 0, 0, 0, 0}, NOP7[7] = {0x0f, 0x1f, 0x80, 0, 0, 0, 0};
        memcpy(nop, rl == 8 ? NOP8 : NOP7, rl);
        // resets first, test last: every partial state is the original behaviour (with the original test the same path
        // only runs with trk+0x20 == 0, so the two reset changes alone change nothing)
        if (patch_code((void *)(uintptr_t)s->same_reset, nop, rl)) {
            if (patch_code((void *)(uintptr_t)s->flush_reset, jr, rl)) {
                if (patch_code((void *)(uintptr_t)s->test, jt, tl)) { n++; continue; }
            }
        }
        g_batch_state[i] = SITE_FAILED;
    }
    return n;
}
#endif
