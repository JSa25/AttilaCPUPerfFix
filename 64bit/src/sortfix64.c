// sortfix64: the render-list sorts that render jobs run while the main thread waits, ported from heapfix/sortfix.c.
// Engine: five std::__introsort_loop<T**, INSTANCE_LIST_SORTER<T>> (research/render_sorts_batch_prefetch.md), ascending
// by an unsigned 64-bit key read through each pointer (+0x78 for WEIGHTED_INSTANCE, +0x50 for the rigid types), so
// every comparison chases two pointers. This reads each key once into a compact array, radix-sorts it (stable) and
// writes the pointers back; up to 32 items: insertion sort. The callers' std::__final_insertion_sort then finds the
// range sorted. A failed scratch allocation falls back to the engine's own sort through a trampoline.
#include <string.h>
#include "perffix64.h"

struct kp { uint64_t key; void *p; };
static __thread struct kp *t_a, *t_b;
static __thread size_t t_cap;

void sortfix64_thread_exit(void) {
    if (t_a) HeapFree(GetProcessHeap(), 0, t_a);
    if (t_b) HeapFree(GetProcessHeap(), 0, t_b);
    t_a = t_b = NULL; t_cap = 0;
}

static int grow(size_t n) {
    if (n <= t_cap) return 1;
    size_t cap = n + n / 2 + 64;
    HANDLE h = GetProcessHeap();
    DWORD err = GetLastError();
    void *a = t_a ? HeapReAlloc(h, 0, t_a, cap * sizeof *t_a) : HeapAlloc(h, 0, cap * sizeof *t_a);
    if (a) t_a = a;
    void *b = a ? (t_b ? HeapReAlloc(h, 0, t_b, cap * sizeof *t_b) : HeapAlloc(h, 0, cap * sizeof *t_b)) : NULL;
    if (b) { t_b = b; t_cap = cap; }
    SetLastError(err);
    return b != NULL;
}

typedef __attribute__((sysv_abi)) void (*introsort_fn)(void **first, void **last, long depth);

#define NSORT 5
// FNV-1a of each whole function (self-contained: its only call is to itself, no RIP-relative operands)
static const struct { uint32_t site, len, key, fnv; const char *name; } SORTS[NSORT] = {
    {0x353e120, 0x34b, 0x78, 0xee8e06efu, "WEIGHTED_INSTANCE"},
    {0x3392d90, 0x40b, 0x50, 0x0b813591u, "RIGID_INSTANCE_V2"},
    {0x3527230, 0x34b, 0x50, 0xc014abdfu, "RIGID_DECAL_DIRT_INSTANCE"},
    {0x3529270, 0x34b, 0x50, 0xc014abdfu, "RIGID_VERTEX_CLOTH_INSTANCE_V2"},
    {0x3520a70, 0x34b, 0x50, 0xc014abdfu, "RIGID_CAMPAIGN_VEGETATION_INSTANCE"},
};
const char *const g_sort_label[NSORT] = {"soldiers", "rigid parts", "decal dirt", "cloth", "campaign vegetation"};
int g_sort_state[NSORT];
static const unsigned char SORT_PROLOGUE[15] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x49, 0x89, 0xd6, 0x49, 0x89, 0xff};
static unsigned char *g_tramp;   // NSORT x 32 bytes: the 15 original bytes + jmp [rip+0] back to site+15

static inline uint64_t key_at(const void *p, unsigned k) { uint64_t v; memcpy(&v, (const char *)p + k, 8); return v; }

static void sort_by_key(void **first, void **last, long depth, int s) {
    size_t n = (size_t)(last - first);
    unsigned K = SORTS[s].key;
    if (n < 2) return;
    if (n <= 32) {
        for (size_t i = 1; i < n; i++) {
            void *v = first[i];
            uint64_t k = key_at(v, K);
            size_t j = i;
            while (j && key_at(first[j - 1], K) > k) { first[j] = first[j - 1]; j--; }
            first[j] = v;
        }
        return;
    }
    if (!grow(n)) { ((introsort_fn)(void *)(g_tramp + 32 * s))(first, last, depth); return; }
    struct kp *a = t_a, *b = t_b;
    size_t hist[8][256];
    memset(hist, 0, sizeof hist);
    for (size_t i = 0; i < n; i++) {
        uint64_t k = key_at(first[i], K);
        a[i].key = k; a[i].p = first[i];
        for (int d = 0; d < 8; d++) hist[d][(k >> (8 * d)) & 0xFF]++;
    }
    for (int d = 0; d < 8; d++) {
        size_t *h = hist[d];
        if (h[(a[0].key >> (8 * d)) & 0xFF] == n) continue;   // every key has the same digit here
        size_t sum = 0;
        for (int i = 0; i < 256; i++) { size_t c = h[i]; h[i] = sum; sum += c; }
        for (size_t i = 0; i < n; i++) b[h[(a[i].key >> (8 * d)) & 0xFF]++] = a[i];
        struct kp *t = a; a = b; b = t;
    }
    for (size_t i = 0; i < n; i++) first[i] = a[i].p;
}

__attribute__((sysv_abi)) static void sort0(void **f, void **l, long d) { sort_by_key(f, l, d, 0); }
__attribute__((sysv_abi)) static void sort1(void **f, void **l, long d) { sort_by_key(f, l, d, 1); }
__attribute__((sysv_abi)) static void sort2(void **f, void **l, long d) { sort_by_key(f, l, d, 2); }
__attribute__((sysv_abi)) static void sort3(void **f, void **l, long d) { sort_by_key(f, l, d, 3); }
__attribute__((sysv_abi)) static void sort4(void **f, void **l, long d) { sort_by_key(f, l, d, 4); }
static introsort_fn const REPL[NSORT] = {sort0, sort1, sort2, sort3, sort4};

// The site as found: SITE_OURS = the game's own function, whole (ours to replace); SITE_LOADER = the loader's hook
// on it (Sol Invictus's fast sort: a jmp rel32 over the first 5 bytes, the rest of the prologue as shipped; his
// parallel list preparation needs his sort in place, so the site is his); SITE_DIFFERS = anything else.
int sortfix64_classify(const unsigned char *site, uint32_t len, uint32_t fnv) {
    if (!memcmp(site, SORT_PROLOGUE, sizeof SORT_PROLOGUE)) return fnv1a(site, len) == fnv ? SITE_OURS : SITE_DIFFERS;
    if (site[0] == 0xE9 && !memcmp(site + 5, SORT_PROLOGUE + 5, sizeof SORT_PROLOGUE - 5)) return SITE_LOADER;
    return SITE_DIFFERS;
}

#ifdef SORTFIX64_TEST
void sortfix64_sort(int s, void **f, void **l) { REPL[s](f, l, 64); }
#else
// Returns how many of the five sorts were replaced (each alone is correct). Trampoline memory comes from the caller
// (allocated before the other threads were paused).
int install_sortfix64(unsigned char *tramp) {
    g_tramp = tramp;
    int n = 0;
    for (int s = 0; s < NSORT; s++) {
        unsigned char *site = (unsigned char *)(uintptr_t)SORTS[s].site;
        g_sort_state[s] = sortfix64_classify(site, SORTS[s].len, SORTS[s].fnv);
        if (g_sort_state[s] != SITE_OURS) continue;   // SITE_OURS here: the game's own bytes, ours to take
        if (thread_inside(SORTS[s].site, sizeof SORT_PROLOGUE)) { g_sort_state[s] = SITE_BUSY; continue; }
        unsigned char *t = tramp + 32 * s;
        uint64_t back = SORTS[s].site + sizeof SORT_PROLOGUE, to = (uint64_t)(uintptr_t)REPL[s];
        memcpy(t, SORT_PROLOGUE, 15);
        t[15] = 0xFF; t[16] = 0x25; memset(t + 17, 0, 4); memcpy(t + 21, &back, 8);
        unsigned char j[15] = {0xFF, 0x25, 0, 0, 0, 0};
        memcpy(j + 6, &to, 8);
        j[14] = 0x90;
        if (patch_code(site, j, sizeof j)) n++; else g_sort_state[s] = SITE_FAILED;
    }
    return n;
}
#endif
