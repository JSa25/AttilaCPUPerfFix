// Faster "which pool chunk owns this pointer" for the game's small-object pools.
// empire.retail.dll frees a pooled object by walking the chunk list of every size class until a
// chunk contains the pointer (FUN_107b6f60 for 18 classes of 4..0xA0 bytes, FUN_107b7070 for 19
// classes of 0xA4..0x774 bytes). Pools gain chunks as a campaign goes on, so frees get slower.
// The replacement gives the same answer but tries a per-thread cache first (pool + page -> class,
// chunk), verified against the live chunk list and range, and moves the owning chunk to the front
// of its list. A stale cache entry only costs a full search, never a wrong answer.
#include <windows.h>
#include <string.h>

struct node { char *base; struct node *next; };
struct cls { void *freelist; struct node *chunks; unsigned count; };

static const unsigned SIZES_A[18] = {4, 6, 8, 12, 0x10, 0x12, 0x14, 0x18, 0x20,
                                     0x2a, 0x30, 0x38, 0x40, 0x6c, 0x70, 0x90, 0x9c, 0xa0};
static const unsigned SIZES_B[19] = {0xa4, 0xa8, 0xac, 0xb0, 0xb4, 0xb8, 0xbc, 0xc0, 0xc8, 0xd0,
                                     0xe0, 0xe4, 0xf0, 0xf4, 0x108, 0x14c, 0x154, 0x1d4, 0x774};

#define CACHE_SLOTS 4096
struct centry { struct cls *pool; unsigned page; struct node *n; unsigned char k; };
static __thread struct centry t_cache[CACHE_SLOTS];

static unsigned char take(struct cls *c, struct node **link, struct node *n, unsigned size, char *p, unsigned *out) {
    if (link != &c->chunks) {  // move to front
        *link = n->next;
        n->next = c->chunks;
        c->chunks = n;
    }
    *(void **)p = c->freelist;
    c->freelist = p;
    *out = size;
    return 1;
}

static unsigned char pool_free(struct cls *pool, const unsigned *sizes, int ncls, char *p, unsigned *out) {
    unsigned page = (unsigned)(UINT_PTR)p >> 12;
    struct centry *e = &t_cache[(page ^ ((unsigned)(UINT_PTR)pool >> 4) * 0x9E3779B1u) & (CACHE_SLOTS - 1)];
    if (e->pool == pool && e->page == page && e->k < ncls) {
        struct cls *c = &pool[e->k];
        for (struct node **link = &c->chunks, *n; (n = *link) != NULL; link = &n->next)
            if (n == e->n) {
                if (n->base <= p && p < n->base + c->count * sizes[e->k])
                    return take(c, link, n, sizes[e->k], p, out);
                break;
            }
    }
    for (int k = 0; k < ncls; k++) {
        struct cls *c = &pool[k];
        for (struct node **link = &c->chunks, *n; (n = *link) != NULL; link = &n->next)
            if (n->base <= p && p < n->base + c->count * sizes[k]) {
                e->pool = pool;
                e->page = page;
                e->n = n;
                e->k = (unsigned char)k;
                return take(c, link, n, sizes[k], p, out);
            }
    }
    return 0;
}

unsigned char __thiscall pool_free_a(struct cls *pool, char *p, unsigned *out) { return pool_free(pool, SIZES_A, 18, p, out); }
unsigned char __thiscall pool_free_b(struct cls *pool, char *p, unsigned *out) { return pool_free(pool, SIZES_B, 19, p, out); }

// Every function the replacement stands in for, hashed whole (FNV-1a): the two entry points and
// the six they chain to. Their bytes are position independent (no absolute addresses).
static const struct { DWORD rva, len, fnv; } POOL_FUNCS[] = {
    {0x7B6F60, 267, 0xFB03A4BCu}, {0x7B6800, 308, 0x54D5B620u}, {0x7B6E40, 279, 0x2690827Au},
    {0x7B6A80, 310, 0x88DAD0F2u}, {0x7B7070, 311, 0xCFCF782Du}, {0x7B6BC0, 311, 0xB6AABF0Fu},
    {0x7B6D00, 309, 0xCD52FD71u}, {0x7B6940, 309, 0xAEA56E55u},
};

static DWORD fnv1a(const unsigned char *b, DWORD n) {
    DWORD h = 0x811C9DC5u;
    while (n--) h = (h ^ *b++) * 0x01000193u;
    return h;
}

static int write_jmp(unsigned char *site, void *to) {
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE9;
    DWORD rel = (DWORD)((unsigned char *)to - (site + 5));
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return 1;
}

// Returns 1 if both pool entry points were redirected, 0 if nothing was changed.
int install_pool(HMODULE m, void (*log)(const char *fmt, ...)) {
    unsigned char *base = (unsigned char *)m;
    for (size_t i = 0; i < sizeof POOL_FUNCS / sizeof POOL_FUNCS[0]; i++)
        if (fnv1a(base + POOL_FUNCS[i].rva, POOL_FUNCS[i].len) != POOL_FUNCS[i].fnv) {
            log("pool function +0x%X does not match this game version - pool lookup not patched", POOL_FUNCS[i].rva);
            return 0;
        }
    if (!write_jmp(base + 0x7B6F60, (void *)pool_free_a)) {
        log("pool lookup: VirtualProtect failed (%lu) - not patched", GetLastError());
        return 0;
    }
    // Either entry works alone (the replacement and the original give the same answers), so a
    // failure here leaves a consistent, partly patched game.
    if (!write_jmp(base + 0x7B7070, (void *)pool_free_b)) {
        log("pool lookup: only the small-size pools were patched (%lu)", GetLastError());
        return 1;
    }
    log("pool free lookup replaced (per-thread cache + move-to-front)");
    return 1;
}
