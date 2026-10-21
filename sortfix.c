// Faster replacement for the battle draw-list sort.
// FUN_117ad5d0 is std::sort (introsort) over pointers to render items, ordered ascending by the
// unsigned keys at +0x6C, then +0x68. Every comparison reads both keys through the pointers, so in
// big battles it misses the cache constantly, and the main thread waits for it (~15% of a frame).
// This reads each item's keys once into a compact array, radix-sorts that (stable: equal keys keep
// their input order; the original left their order unspecified) and writes the pointers back.
#include <windows.h>
#include <string.h>

#define RVA_DRAWSORT 0x17AD5D0  // VA 0x117AD5D0 at the preferred base 0x10000000
#define DRAWSORT_LEN 630
#define DRAWSORT_FNV 0xD811EF5Au  // FNV-1a of the whole function in the 2026-09-19 build (fnv_sort.py)

struct kp { unsigned long long key; void *p; };

static __thread struct kp *t_a, *t_b;
static __thread unsigned t_cap;
static unsigned char g_tramp[16];  // the overwritten prologue + jmp back: the original sort

typedef void(__cdecl *sort_fn)(void **, void **, int, void *);

static unsigned long long key_of(void *p) {
    const unsigned char *o = p;
    return ((unsigned long long)*(const unsigned *)(o + 0x6C) << 32) | *(const unsigned *)(o + 0x68);
}

static int grow(unsigned n) {
    if (n <= t_cap) return 1;
    unsigned cap = n + n / 2 + 64;
    HANDLE h = GetProcessHeap();
    void *a = t_a ? HeapReAlloc(h, 0, t_a, cap * sizeof *t_a) : HeapAlloc(h, 0, cap * sizeof *t_a);
    if (!a) return 0;
    t_a = a;
    void *b = t_b ? HeapReAlloc(h, 0, t_b, cap * sizeof *t_b) : HeapAlloc(h, 0, cap * sizeof *t_b);
    if (!b) return 0;
    t_b = b;
    t_cap = cap;
    return 1;
}

void __cdecl draw_sort(void **first, void **last, int ideal, void *ctx) {
    unsigned n = (unsigned)(last - first);
    if (n < 2) return;
    if (n <= 32) {  // small: insertion sort (stable)
        for (unsigned i = 1; i < n; i++) {
            void *v = first[i];
            unsigned long long k = key_of(v);
            unsigned j = i;
            while (j && key_of(first[j - 1]) > k) { first[j] = first[j - 1]; j--; }
            first[j] = v;
        }
        return;
    }
    if (!grow(n)) {
        ((sort_fn)(void *)g_tramp)(first, last, ideal, ctx);
        return;
    }
    struct kp *a = t_a, *b = t_b;
    unsigned hist[8][256];
    memset(hist, 0, sizeof hist);
    for (unsigned i = 0; i < n; i++) {
        unsigned long long k = key_of(first[i]);
        a[i].key = k;
        a[i].p = first[i];
        for (int d = 0; d < 8; d++) hist[d][(k >> (8 * d)) & 0xFF]++;
    }
    for (int d = 0; d < 8; d++) {
        unsigned *h = hist[d];
        if (h[(a[0].key >> (8 * d)) & 0xFF] == n) continue;  // every key has the same digit here
        unsigned sum = 0;
        for (int i = 0; i < 256; i++) { unsigned c = h[i]; h[i] = sum; sum += c; }
        for (unsigned i = 0; i < n; i++) b[h[(a[i].key >> (8 * d)) & 0xFF]++] = a[i];
        struct kp *t = a; a = b; b = t;
    }
    for (unsigned i = 0; i < n; i++) first[i] = a[i].p;
}

static DWORD fnv1a(const unsigned char *b, DWORD n) {
    DWORD h = 0x811C9DC5u;
    while (n--) h = (h ^ *b++) * 0x01000193u;
    return h;
}

// Returns 1 if the draw-list sort was redirected, 0 if nothing was changed.
int install_sortfix(HMODULE m, void (*log)(const char *fmt, ...)) {
    unsigned char *site = (unsigned char *)m + RVA_DRAWSORT;
    if (fnv1a(site, DRAWSORT_LEN) != DRAWSORT_FNV) {
        log("draw-list sort at +0x%X does not match this game version - not patched", RVA_DRAWSORT);
        return 0;
    }
    // Trampoline = the first 5 bytes (sub esp,14h; push ebx; push ebp) + jmp to site+5.
    DWORD old;
    if (!VirtualProtect(g_tramp, sizeof g_tramp, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(g_tramp, site, 5);
    g_tramp[5] = 0xE9;
    DWORD rel = (DWORD)((site + 5) - (g_tramp + 10));
    memcpy(g_tramp + 6, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), g_tramp, sizeof g_tramp);
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        log("draw-list sort: VirtualProtect failed (%lu) - not patched", GetLastError());
        return 0;
    }
    site[0] = 0xE9;
    rel = (DWORD)((unsigned char *)draw_sort - (site + 5));
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log("battle draw-list sort replaced (key extraction + radix sort)");
    return 1;
}
