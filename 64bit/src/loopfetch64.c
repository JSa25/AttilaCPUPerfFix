// loopfetch64: prefetch ahead in the battle render loops, ported from heapfix/loopfetch.c
// (research/render_sorts_batch_prefetch.md, part 3). The loops walk an array of item pointers and stall on the first
// read through each item. At the loop top a stub does `if (i + 4 < end) prefetcht0 [array[i + 4] + off]` (never a
// real load through an item), then the loop top's own 8 bytes, and returns behind them.
// Engine loops (the same six functions batchfix64 patches further down):
//   weighted  RIGID_SCENE_NODE_V2::add_list_instances<WEIGHTED_INSTANCE>  top 0x354bb70  mov rax, [rsp+0xc8]
//             list at [rsp+0xc8] (array at +8), index rbx, end at [rsp+0x7c], first read item+0
//   rigid x5  RIGID_MESH::add_list_instances_depth<...>  tops 0x3391ea0 (index r12), 0x351fcb0, 0x3526340, 0x35284a0,
//             0x3537dd0 (index r14)   mov rax, [r13+8] ; mov rax, [rax+idx*8]
//             list r13, end at [rsp+0x50], first read item+0x44 (0x40 in the last)
// rax is dead at every top but the animation texture's (the top's own bytes load it) and so are the flags (a cmp
// follows before any branch).
//   soldiers in the shadow passes and rigid parts in the main view (added 2026-11-15): see the table below.
//   the main-view loops of the other four types that carry a variant (added 2026-11-20): campaign vegetation, decal
//   dirt, cloth and decals. Same template as "rigid parts, main view": the first thing each pass does with an item is
//   read its variant (item+0x44, decals +0x40). These are the x64 forms of the x86 loops aff90 / afc10 / af720
//   (x86 prefetch offsets +0x3C and +0x40 are these variant fields).
// Loops of the family that get no prefetch, from the code:
//   - billboards, vegetation, texture blend, grass (depth and main view, 8 loops): they never read through the item
//     in the loop (no variant: the item pointer is only handed on to mesh->vt[0xc0] when the mesh asks for it), so
//     the loop top has nothing to wait for;
//   - the ten add_list_instances_depth_visibility_check loops: they do read each item (its bounds), but they run only
//     for point-light shadow maps (scene view type 0x49, DEFERRED_LIGHTING_VIEW::POINTLIGHT_SHADOW_MAP_VIEW; the
//     dispatch is in each mesh type's draw_instances). Not patched; a profile of a night battle would say.
// Not a render loop (added 2026-11-22, as heapfix/loopfetch.c's "animation batching" site, x86 0x182E0DA, since
// 2026-11-17): WEIGHTED_MESH_ANIMATION_TEXTURE_CACHE::fill_animation_texture (0x3416d50), the loop that gives every
// soldier its place in the animation texture (top 0x3416de0, list at [rsp+0x58], index eax, end at [rsp+0x160]). It
// reads and writes item+0x6c..+0x87 and +0xb8 (x86 +0x5c..+0x70 and +0xa4), never the item's first line, so as on x86
// two lines of item i+8 are fetched: +0x70 and +0xb8. Here rax holds the index at the top and stays untouched; rcx
// (loaded by the top's own bytes) carries the stub's work. The loop runs on one thread whatever the game's
// enable_tbb_fill_animation_texture says: only the texture writes after it are shared out.
#include <string.h>
#include "perffix64.h"

// Each loop: the top's own bytes (olen of them are replaced by the jump), then the stub's two parts: `head` computes
// index + 4 and compares it with the loop's end (a jae over `body` follows it), `body` loads item index + 4 through the
// array and prefetches the field the loop reads first.
// top: the first replaced instruction. lead: how many bytes before it the loop's own jump target is (0: the top itself).
struct lf_site { uint32_t top; uint8_t olen, orig[9], hlen, head[12], blen, body[28]; const char *name; uint8_t lead; };
#define NLF 13
const int loopfetch64_count = NLF;
#ifdef LOOPFETCH64_TEST
static struct lf_site LF[NLF] = {   // the test's control build changes one entry
#else
static const struct lf_site LF[NLF] = {
#endif
    // weighted, main view: list at [rsp+0xc8], index rbx, end at [rsp+0x7c], first read item+0
    {0x354bb70, 8, {0x48, 0x8b, 0x84, 0x24, 0xc8, 0x00, 0x00, 0x00}, 7, {0x8d, 0x43, 0x04, 0x3b, 0x44, 0x24, 0x7c},
     20, {0x48, 0x8b, 0x84, 0x24, 0xc8, 0, 0, 0, 0x48, 0x8b, 0x40, 0x08, 0x48, 0x8b, 0x44, 0xd8, 0x20, 0x0f, 0x18, 0x08}, "soldiers, main view", 0},
    {0x3391ea0, 8, {0x49, 0x8b, 0x45, 0x08, 0x4a, 0x8b, 0x04, 0xe0}, 9, {0x41, 0x8d, 0x44, 0x24, 0x04, 0x3b, 0x44, 0x24, 0x50},
     13, {0x49, 0x8b, 0x45, 0x08, 0x4a, 0x8b, 0x44, 0xe0, 0x20, 0x0f, 0x18, 0x48, 0x44}, "rigid parts, depth", 0},
    {0x351fcb0, 8, {0x49, 0x8b, 0x45, 0x08, 0x4a, 0x8b, 0x04, 0xf0}, 8, {0x41, 0x8d, 0x46, 0x04, 0x3b, 0x44, 0x24, 0x50},
     13, {0x49, 0x8b, 0x45, 0x08, 0x4a, 0x8b, 0x44, 0xf0, 0x20, 0x0f, 0x18, 0x48, 0x44}, "campaign vegetation, depth", 0},
    {0x3526340, 8, {0x49, 0x8b, 0x45, 0x08, 0x4a, 0x8b, 0x04, 0xf0}, 8, {0x41, 0x8d, 0x46, 0x04, 0x3b, 0x44, 0x24, 0x50},
     13, {0x49, 0x8b, 0x45, 0x08, 0x4a, 0x8b, 0x44, 0xf0, 0x20, 0x0f, 0x18, 0x48, 0x44}, "decal dirt, depth", 0},
    {0x35284a0, 8, {0x49, 0x8b, 0x45, 0x08, 0x4a, 0x8b, 0x04, 0xf0}, 8, {0x41, 0x8d, 0x46, 0x04, 0x3b, 0x44, 0x24, 0x50},
     13, {0x49, 0x8b, 0x45, 0x08, 0x4a, 0x8b, 0x44, 0xf0, 0x20, 0x0f, 0x18, 0x48, 0x44}, "cloth, depth", 0},
    {0x3537dd0, 8, {0x49, 0x8b, 0x45, 0x08, 0x4a, 0x8b, 0x04, 0xf0}, 8, {0x41, 0x8d, 0x46, 0x04, 0x3b, 0x44, 0x24, 0x50},
     13, {0x49, 0x8b, 0x45, 0x08, 0x4a, 0x8b, 0x44, 0xf0, 0x20, 0x0f, 0x18, 0x48, 0x40}, "decals, depth", 0},
    // weighted, depth (the shadow passes): RIGID_SCENE_NODE_V2::add_list_instances_depth<WEIGHTED_INSTANCE>, top 0x354b700
    // mov rax, [rsp+0x70] ; mov rax, [rax+8]: list at [rsp+0x70], index r13, end at [rsp+0x60], first read item+0
    {0x354b700, 9, {0x48, 0x8b, 0x44, 0x24, 0x70, 0x48, 0x8b, 0x40, 0x08}, 8, {0x41, 0x8d, 0x45, 0x04, 0x3b, 0x44, 0x24, 0x60},
     17, {0x48, 0x8b, 0x44, 0x24, 0x70, 0x48, 0x8b, 0x40, 0x08, 0x4a, 0x8b, 0x44, 0xe8, 0x20, 0x0f, 0x18, 0x08}, "soldiers, depth", 0},
    // rigid parts, main view: RIGID_MESH::add_list_instances<RIGID_INSTANCE_V2>, top 0x3392440
    // mov rax, [r15+8] ; mov r13, [rax+rcx*8]: list r15, index rcx, end at [rsp+0x70], first read item+0x44
    {0x3392440, 8, {0x49, 0x8b, 0x47, 0x08, 0x4c, 0x8b, 0x2c, 0xc8}, 7, {0x8d, 0x41, 0x04, 0x3b, 0x44, 0x24, 0x70},
     13, {0x49, 0x8b, 0x47, 0x08, 0x48, 0x8b, 0x44, 0xc8, 0x20, 0x0f, 0x18, 0x48, 0x44}, "rigid parts, main view", 0},
    // campaign vegetation, main view: RIGID_MESH::add_list_instances<RIGID_CAMPAIGN_VEGETATION_INSTANCE>, top 0x3520240
    // mov rax, [r15+8] ; mov rbx, [rax+r13*8]: list r15, index r13, end at [rsp+0x6c], first read item+0x44
    {0x3520240, 8, {0x49, 0x8b, 0x47, 0x08, 0x4a, 0x8b, 0x1c, 0xe8}, 8, {0x41, 0x8d, 0x45, 0x04, 0x3b, 0x44, 0x24, 0x6c},
     13, {0x49, 0x8b, 0x47, 0x08, 0x4a, 0x8b, 0x44, 0xe8, 0x20, 0x0f, 0x18, 0x48, 0x44}, "campaign vegetation, main view", 0},
    // decal dirt, main view: RIGID_MESH::add_list_instances<RIGID_DECAL_DIRT_INSTANCE>, top 0x35268f0
    // mov rax, [rbx+8] ; mov rbx, [rax+r12*8]: list rbx, index r12, end at [rsp+0x84], first read item+0x44
    {0x35268f0, 8, {0x48, 0x8b, 0x43, 0x08, 0x4a, 0x8b, 0x1c, 0xe0}, 12, {0x41, 0x8d, 0x44, 0x24, 0x04, 0x3b, 0x84, 0x24, 0x84, 0x00, 0x00, 0x00},
     13, {0x48, 0x8b, 0x43, 0x08, 0x4a, 0x8b, 0x44, 0xe0, 0x20, 0x0f, 0x18, 0x48, 0x44}, "decal dirt, main view", 0},
    // cloth, main view: RIGID_MESH::add_list_instances<RIGID_VERTEX_CLOTH_INSTANCE_V2>, top 0x3528a30 (as campaign vegetation)
    {0x3528a30, 8, {0x49, 0x8b, 0x47, 0x08, 0x4a, 0x8b, 0x1c, 0xe8}, 8, {0x41, 0x8d, 0x45, 0x04, 0x3b, 0x44, 0x24, 0x6c},
     13, {0x49, 0x8b, 0x47, 0x08, 0x4a, 0x8b, 0x44, 0xe8, 0x20, 0x0f, 0x18, 0x48, 0x44}, "cloth, main view", 0},
    // decals, main view: RIGID_MESH::add_list_instances<RIGID_DECAL_INSTANCE>; the loop's jump target is 0x3538330
    // (mov [rsp+0x70], rbp), the two instructions replaced follow it at 0x3538335:
    // mov rax, [r14+8] ; mov rax, [rax+rbp*8]: list r14, index rbp, end at [rsp+0x48], first read item+0x40
    {0x3538335, 8, {0x49, 0x8b, 0x46, 0x08, 0x48, 0x8b, 0x04, 0xe8}, 7, {0x8d, 0x45, 0x04, 0x3b, 0x44, 0x24, 0x48},
     13, {0x49, 0x8b, 0x46, 0x08, 0x48, 0x8b, 0x44, 0xe8, 0x20, 0x0f, 0x18, 0x48, 0x40}, "decals, main view", 5},
    // animation texture: mov eax, eax ; mov rcx, [rsp+0x58]. head: lea ecx, [rax+8] ; cmp ecx, [rsp+0x160]. body:
    // mov rcx, [rsp+0x58] ; mov rcx, [rcx+8] ; mov rcx, [rcx+rax*8+0x40] (item i+8) ; prefetcht0 [rcx+0x70] ;
    // prefetcht0 [rcx+0xb8]
    {0x3416de0, 7, {0x89, 0xc0, 0x48, 0x8b, 0x4c, 0x24, 0x58}, 10, {0x8d, 0x48, 0x08, 0x3b, 0x8c, 0x24, 0x60, 0x01, 0x00, 0x00},
     25, {0x48, 0x8b, 0x4c, 0x24, 0x58, 0x48, 0x8b, 0x49, 0x08, 0x48, 0x8b, 0x4c, 0xc1, 0x40, 0x0f, 0x18, 0x49, 0x70,
          0x0f, 0x18, 0x89, 0xb8, 0x00, 0x00, 0x00}, "animation texture", 0},
};

// Writes site i's stub at code (64 bytes); it ends in a jmp rel32 to `back` (the loop top + olen in the engine).
// Returns the stub's length.
int loopfetch64_build(uint8_t *code, int i, uint64_t back) {
    const struct lf_site *s = &LF[i];
    uint8_t *p = code;
    memcpy(p, s->head, s->hlen); p += s->hlen;
    *p++ = 0x73; *p++ = s->blen;                    // jae 1f
    memcpy(p, s->body, s->blen); p += s->blen;
    memcpy(p, s->orig, s->olen); p += s->olen;      // 1: the loop top's own bytes
    *p++ = 0xe9;
    int32_t rel = (int32_t)(int64_t)(back - ((uint64_t)(uintptr_t)p + 4));
    memcpy(p, &rel, 4); p += 4;
    return (int)(p - code);
}

#ifndef LOOPFETCH64_TEST
_Static_assert(NLF <= LOOPFETCH_MAX, "the stub block has room for every loop");
// code: NLF * 64 bytes of executable memory within +-2 GB of the engine. Returns the loops patched (a loop whose bytes
// differ or that a paused thread is inside is left alone), -1 without near memory.
int install_loopfetch64(uint8_t *code) {
    int64_t d = (int64_t)((uintptr_t)code - 0x3400000);
    if (!code || d < -0x7FF00000ll || d > 0x7FF00000ll) return -1;
    int n = 0;
    for (int i = 0; i < NLF; i++) {
        const struct lf_site *s = &LF[i];
        if (memcmp((void *)(uintptr_t)s->top, s->orig, s->olen) || thread_inside(s->top, s->olen)) continue;
        uint8_t *st = code + 64 * i, j[9] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
        loopfetch64_build(st, i, (uint64_t)s->top + s->olen);
        int32_t rel = (int32_t)((int64_t)(uintptr_t)st - ((int64_t)s->top + 5));
        memcpy(j + 1, &rel, 4);
        if (patch_code((void *)(uintptr_t)s->top, j, s->olen)) n++;
    }
    return n;
}
#endif
