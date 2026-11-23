// pool64: faster "which chunk owns this pointer" for the campaign AI's memory pool, ported from heapfix/pool.c.
// Engine: EMPIRECAMPAIGNAI::MEMORY::MEMORY_IMPL::TREE_NODE<30, POOL_LIST<8,512, ... 3816,62>>::dealloc(void*,
// unsigned long&) at 0x109ed30 (1603 callers pass CAI_MEMORY_POOL::m_mem_pool): walks every chunk of 30 size classes
// until one contains the pointer (research/heap_pools.md). Same answer here, from a per-thread cache (pool + page ->
// class, chunk) checked against the live list and range, and the owning chunk moves to the front of its list. A stale
// cache entry only costs the full search.
#include <string.h>
#include "perffix64.h"

struct node { char *base; struct node *next; };
struct cls { void *freelist; struct node *chunks; uint32_t count, pad; };   // 0x18 bytes

#define NCLS 30
static const uint32_t SIZES[NCLS] = {8, 10, 20, 24, 30, 32, 40, 48, 56, 70, 96, 160, 200, 216, 224, 232, 240, 248, 256,
                                     264, 272, 280, 304, 312, 336, 344, 456, 520, 936, 3816};
#define POOL_SITE 0x109ed30u
#define POOL_TREE_LEN 2224u            // 0x109ed30 .. 0x109f5e0, the root and every sub-node it calls
#define POOL_TREE_FNV 0x0036f034u
static const unsigned char POOL_PROLOGUE[16] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x49, 0x89, 0xd7, 0x48, 0x89, 0xf5, 0x48, 0x89, 0xfb};

#define CACHE_SLOTS 4096
struct centry { struct cls *pool; uint64_t page; struct node *n; unsigned char k; };
static __thread struct centry *t_cache;   // allocated at a thread's first pool free, freed at thread exit
void pool64_thread_exit(void) {
    if (t_cache) HeapFree(GetProcessHeap(), 0, t_cache);
    t_cache = NULL;
}

static inline int owns(const struct cls *c, const struct node *n, uint32_t size, const char *p) {
    return n->base <= p && (uint64_t)(p - n->base) < (uint64_t)(uint32_t)(c->count * size);   // engine: 32-bit multiply
}
static unsigned char take(struct cls *c, struct node **link, struct node *n, uint32_t size, char *p, uint64_t *out) {
    if (link != &c->chunks) { *link = n->next; n->next = c->chunks; c->chunks = n; }   // move to front
    *(void **)p = c->freelist;
    c->freelist = p;
    *out = size;
    return 1;
}

__attribute__((sysv_abi)) unsigned char pool64_dealloc(struct cls *pool, char *p, uint64_t *out) {
    uint64_t page = (uintptr_t)p >> 12;
    if (!t_cache) {
        DWORD err = GetLastError();   // the engine's free must not change it
        t_cache = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, CACHE_SLOTS * sizeof *t_cache);
        SetLastError(err);
    }
    struct centry none = {0};
    struct centry *e = t_cache ? &t_cache[(page ^ ((uintptr_t)pool >> 4) * 0x9E3779B97F4A7C15ull) & (CACHE_SLOTS - 1)] : &none;
    if (e->pool == pool && e->page == page && e->k < NCLS) {
        struct cls *c = &pool[e->k];
        for (struct node **link = &c->chunks, *n; (n = *link) != NULL; link = &n->next)
            if (n == e->n) {
                if (owns(c, n, SIZES[e->k], p)) return take(c, link, n, SIZES[e->k], p, out);
                break;
            }
    }
    for (int k = 0; k < NCLS; k++) {
        struct cls *c = &pool[k];
        for (struct node **link = &c->chunks, *n; (n = *link) != NULL; link = &n->next)
            if (owns(c, n, SIZES[k], p)) {
                e->pool = pool; e->page = page; e->n = n; e->k = (unsigned char)k;
                return take(c, link, n, SIZES[k], p, out);
            }
    }
    return 0;
}

#ifndef POOL64_TEST
int install_pool64(void) {   // 1 replaced, 0 bytes differ, -1 a thread inside the prologue, -2 VirtualProtect failed
    unsigned char *site = (unsigned char *)(uintptr_t)POOL_SITE;
    if (memcmp(site, POOL_PROLOGUE, sizeof POOL_PROLOGUE) || fnv1a(site, POOL_TREE_LEN) != POOL_TREE_FNV) return 0;
    if (thread_inside(POOL_SITE, 14)) return -1;
    unsigned char j[14] = {0xFF, 0x25, 0, 0, 0, 0};   // jmp [rip+0] ; abs64 (the original is never called again)
    uint64_t to = (uint64_t)(uintptr_t)&pool64_dealloc;
    memcpy(j + 6, &to, 8);
    return patch_code(site, j, sizeof j) ? 1 : -2;
}
#endif
