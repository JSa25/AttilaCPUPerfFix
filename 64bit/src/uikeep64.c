// uikeep64: menu stutter fix, ported from heapfix/uikeep.c (research/height_uikeep.md, part B).
// Closing a panel queues its pictures for deletion; the queue step deletes them one at a time, so opening the panel
// again loads the same files again. Here the step's delete keeps the picture and remembers its name; the engine's own
// acquire treats a count of 0 as loaded and counts it up again. Past KEEP names the oldest is deleted the engine's way
// if it is still unused.
// Engine: WARSCAPE::UI_TEXTURE_HANDLER::free_non_visible_sprite() 0x30a5720. The x86 delete function is inlined here, so
// the hook is on the delete block itself:
//   0x30a58a8  mov r15, [rip+m_current_set] ; add r15, 0x18 ; mov rdi, r15     (14 bytes, replaced by jmp [rip+0])
//   0x30a58b6  the delete goes on (mov rsi, r14 ; call get ; free_cached_texture ; remove)
//   0x30a58d7  the unlink of the queue node, common to both ways
// At the site: r13 = &queue node's name, r14 = &descriptor's key, [rsp+0x28] = the step's return address.
// Different from x86, on purpose: only the idle callers keep (the defragger's tick and check, and the release). A
// caller that needs room now (ensure_cached, display close, the flush commands) gets the real delete, and everything
// kept is freed first, so kept pictures never stand in the way of a new one.
#include <string.h>
#include "perffix64.h"

#define UK_SITE 0x30a58a8u
static const uint8_t UK_SITE_B[14] = {0x4c, 0x8b, 0x3d, 0xd9, 0xcc, 0x1a, 0x02, 0x49, 0x83, 0xc7, 0x18, 0x4c, 0x89, 0xff};
#define UK_STEP 0x30a5720u            // free_non_visible_sprite
#define UK_STEP_LEN 0x28au
#define UK_STEP_FNV 0x6D1B747Cu
#define UK_RELEASE 0x30a6c40u         // free_sprite_details: the queueing is relied on
#define UK_RELEASE_LEN 0x15bu
#define UK_RELEASE_FNV 0x20A6971Cu
#define UK_ACQUIRE 0x30a68f0u         // initialise_sprite_details: a count of 0 is reused
#define UK_ACQUIRE_LEN 0x18eu
#define UK_ACQUIRE_FNV 0x811E8994u
#define UK_CALLER_TICK 0x30a81c8u
#define UK_CALLER_CHECK 0x30a8cc4u
#define UK_CALLER_RELEASE 0x30a6d84u
#define KEEP 1024                     // shortcut: a count, not bytes (611 names covered every menu in the x86 test)

typedef __attribute__((sysv_abi)) uint8_t *(*uk_get_fn)(void *table, const void *key);
typedef __attribute__((sysv_abi)) void (*uk_free_fn)(void *details);
typedef __attribute__((sysv_abi)) void (*uk_remove_fn)(void *table, const void *key);
typedef __attribute__((sysv_abi)) void (*uk_copy_fn)(void *dst, const void *src);
typedef __attribute__((sysv_abi)) void (*uk_dtor_fn)(void *s);
typedef __attribute__((sysv_abi)) char (*uk_eq_fn)(const void *a, const void *b);

// the engine's functions and data; the test points these at its own
uk_get_fn uk_desc_get = (uk_get_fn)0x314f210, uk_cache_get = (uk_get_fn)0x314f340;
uk_free_fn uk_free_tex = (uk_free_fn)0x30a41b0;
uk_remove_fn uk_desc_remove = (uk_remove_fn)0x314f470;
uk_copy_fn uk_str_copy = (uk_copy_fn)0x512fb0;
uk_dtor_fn uk_str_dtor = (uk_dtor_fn)0x512d00;
uk_eq_fn uk_str_eq = (uk_eq_fn)0x50def0;
void **uk_current_set = (void **)0x5252588;   // UI_TEXTURE_HANDLER::m_current_set; its descriptor table is at +0x18
void *uk_cache_table = (void *)0x52525e8;     // UI_TEXTURE_HANDLER::m_cache_hash

void *uk64_ret_delete = (void *)(uintptr_t)0x30a58b6, *uk64_ret_keep = (void *)(uintptr_t)0x30a58d7;
volatile unsigned char g_uikeep_on = 1;
unsigned g_uikeep_keep = KEEP;        // how many are kept, 1..KEEP
static struct { uint8_t s[16]; } g_ring[KEEP];   // CA::String is 16 bytes (the queue node holds one at +0x10 of 0x20)
static unsigned g_head, g_count;
volatile LONG g_uk[4];                // kept, kept again, deleted as oldest, already gone

// Deletes the kept picture of that name if nothing took it again and the engine still has it: the step's own test
// (count 0 or less, not pinned) and its own delete. `current` is the descriptor key the engine is about to delete
// itself: that one is left to the engine.
static void free_kept(void *name, const void *current) {
    uint8_t *set = *uk_current_set ? (uint8_t *)*uk_current_set + 0x18 : NULL;
    uint8_t *d = set ? uk_desc_get(set, name) : NULL;
    if (d && d + 8 != current) {
        uint8_t *c = uk_cache_get(uk_cache_table, d + 8);
        int count, pinned;
        if (c) { memcpy(&count, c + 0x44, 4); memcpy(&pinned, c + 0x48, 4); }
        if (c && count <= 0 && !pinned) {
            uint8_t *d2 = uk_desc_get(set, d + 8);
            if (d2) {
                void *details; memcpy(&details, d2 + 0x68, 8);
                uk_free_tex(details);
                uk_desc_remove(set, d + 8);
            }
            InterlockedIncrement(&g_uk[2]);
        } else
            InterlockedIncrement(&g_uk[3]);
    } else if (!d)
        InterlockedIncrement(&g_uk[3]);
    uk_str_dtor(name);
}

static void free_all(const void *current) {
    for (; g_count; g_count--) free_kept(g_ring[(g_head + g_uikeep_keep - g_count) % g_uikeep_keep].s, current);   // oldest first
}

// 1 = keep the picture (the step only takes the name off its queue), 0 = the engine deletes it
__attribute__((sysv_abi)) int uk64_decide(const void *name, uintptr_t caller, const void *current) {
    if (!g_uikeep_on) return 0;   // shortcut: what is kept stays kept until the fix is on again or the game ends
    if (caller != UK_CALLER_TICK && caller != UK_CALLER_CHECK && caller != UK_CALLER_RELEASE) {
        free_all(current);        // room is needed now: nothing kept stands in the way
        return 0;
    }
    for (unsigned i = 0; i < g_count; i++)   // kept before, used again, queued again: already in the ring
        if (uk_str_eq(g_ring[(g_head + g_uikeep_keep - 1 - i) % g_uikeep_keep].s, name)) { InterlockedIncrement(&g_uk[1]); return 1; }
    if (g_count == g_uikeep_keep) free_kept(g_ring[g_head].s, current);
    else g_count++;
    uk_str_copy(g_ring[g_head].s, name);
    g_head = (g_head + 1) % g_uikeep_keep;
    InterlockedIncrement(&g_uk[0]);
    return 1;
}

// rsp is 16-aligned at the site (entry + 5 pushes), so the call is direct. Live afterwards: r12-r14, rbx (callee-saved).
__attribute__((naked)) void uk64_stub(void) {
    __asm__ volatile(
        "mov %r13, %rdi\n"
        "mov 0x28(%rsp), %rsi\n"
        "mov %r14, %rdx\n"
        "call uk64_decide\n"
        "test %eax, %eax\n jnz 1f\n"
        "mov uk_current_set(%rip), %r15\n mov (%r15), %r15\n"   // the site's instructions
        "add $0x18, %r15\n"
        "mov %r15, %rdi\n"
        "jmp *uk64_ret_delete(%rip)\n"
        "1: jmp *uk64_ret_keep(%rip)\n");
}

#ifndef UIKEEP64_TEST
// 1 installed, 0 the engine's code differs, -1 a thread inside the site, -2 VirtualProtect failed.
int install_uikeep64(void) {
    if (memcmp((void *)(uintptr_t)UK_SITE, UK_SITE_B, 14) ||
        fnv1a((void *)(uintptr_t)UK_STEP, UK_STEP_LEN) != UK_STEP_FNV ||
        fnv1a((void *)(uintptr_t)UK_RELEASE, UK_RELEASE_LEN) != UK_RELEASE_FNV ||
        fnv1a((void *)(uintptr_t)UK_ACQUIRE, UK_ACQUIRE_LEN) != UK_ACQUIRE_FNV) return 0;
    if (thread_inside(UK_SITE, 14)) return -1;
    uint8_t j[14] = {0xFF, 0x25, 0, 0, 0, 0};
    uint64_t a = (uint64_t)(uintptr_t)uk64_stub;
    memcpy(j + 6, &a, 8);
    return patch_code((void *)(uintptr_t)UK_SITE, j, 14) ? 1 : -2;
}
#endif
