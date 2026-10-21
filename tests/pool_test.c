// Differential test: the game's original pool free functions vs pool.c on the same random pools.
// usage: pool_test.exe <path to empire.retail.dll>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../pool.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef unsigned char(__thiscall *pool_fn)(struct cls *, char *, unsigned *);

static void quiet(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); printf("\n"); }

static unsigned rng = 12345;
static unsigned rnd(unsigned n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

// Builds a pool set with random chunks; returns all chunk nodes for picking pointers.
static int build(struct cls *pool, int ncls, const unsigned *sizes, struct node **all, int *klass) {
    int total = 0;
    for (int k = 0; k < ncls; k++) {
        pool[k].freelist = (void *)(UINT_PTR)(0x1000 + k);
        pool[k].chunks = NULL;
        pool[k].count = 1 + rnd(40);
        int nchunks = rnd(8);
        for (int j = 0; j < nchunks; j++) {
            struct node *n = malloc(sizeof *n);
            n->base = malloc((pool[k].count + 3) * sizes[k] + 16);  // room for "just past the end" pointers
            n->next = pool[k].chunks;
            pool[k].chunks = n;
            all[total] = n;
            klass[total++] = k;
        }
    }
    return total;
}

static int in_list(struct cls *c, struct node *x) {
    for (struct node *n = c->chunks; n; n = n->next) if (n == x) return 1;
    return 0;
}

static void run(pool_fn orig, unsigned char(__thiscall *mine)(struct cls *, char *, unsigned *), int ncls, const unsigned *sizes, const char *label) {
    struct cls pool[19], snap[19];
    struct node *all[19 * 8];
    int klass[19 * 8];
    int total = build(pool, ncls, sizes, all, klass);
    int found = 0, missed = 0, unlinked = 0;
    for (int it = 0; it < 200000; it++) {
        char *p;
        int pick = total ? (int)rnd(total) : -1;
        unsigned r = rnd(100);
        if (pick < 0 || r < 5) {
            p = (char *)malloc(64);  // not in any pool
        } else {
            int k = klass[pick];
            unsigned idx = rnd(pool[k].count + (r < 10 ? 2 : 0));  // sometimes just past the end
            p = all[pick]->base + idx * sizes[k] + (r < 15 ? rnd(sizes[k]) : 0);
        }
        if (r == 99 && rnd(200) == 0 && pick >= 0 && in_list(&pool[klass[pick]], all[pick])) {
            // Unlink a chunk from its list (as if the game released it): both must stop finding it.
            struct cls *c = &pool[klass[pick]];
            for (struct node **l = &c->chunks; *l; l = &(*l)->next)
                if (*l == all[pick]) { *l = all[pick]->next; break; }
            unlinked++;
        }
        void *saved_word = *(void **)p;
        memcpy(snap, pool, sizeof(struct cls) * ncls);
        // Original runs on a copy of the list order; restore pool fields afterwards.
        struct node *order[19][8]; int len[19];
        for (int k = 0; k < ncls; k++) { len[k] = 0; for (struct node *n = pool[k].chunks; n; n = n->next) order[k][len[k]++] = n; }
        unsigned out_o = 0xDEAD, out_m = 0xDEAD;
        unsigned char ro = orig(pool, p, &out_o);
        void *word_o = *(void **)p;
        void *fl_o[19]; for (int k = 0; k < ncls; k++) fl_o[k] = pool[k].freelist;
        memcpy(pool, snap, sizeof(struct cls) * ncls);
        for (int k = 0; k < ncls; k++) { for (int j = 0; j < len[k]; j++) order[k][j]->next = j + 1 < len[k] ? order[k][j + 1] : NULL; }
        *(void **)p = saved_word;
        unsigned char rm = mine(pool, p, &out_m);
        CHECK((ro != 0) == (rm != 0));
        CHECK(out_o == out_m);
        CHECK(*(void **)p == word_o);
        for (int k = 0; k < ncls; k++) {
            CHECK(pool[k].freelist == fl_o[k]);
            int n2 = 0; for (struct node *n = pool[k].chunks; n; n = n->next) { CHECK(n2 < len[k]); n2++; }
            CHECK(n2 == len[k]);  // same chunks, possibly reordered
            for (int j = 0; j < len[k]; j++) CHECK(in_list(&pool[k], order[k][j]));
        }
        if (rm) found++; else missed++;
        // Undo the free so the pool stays "full" and pointers stay valid to pick.
        if (rm) for (int k = 0; k < ncls; k++) pool[k].freelist = snap[k].freelist;
        *(void **)p = saved_word;
    }
    printf("%s: 200000 frees identical to the game's function (%d found, %d not in a pool, %d chunks unlinked)\n", label, found, missed, unlinked);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, w, MAX_PATH);
    HMODULE m = LoadLibraryExW(w, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(m != NULL);
    run((pool_fn)((char *)m + 0x7B6F60), pool_free_a, 18, SIZES_A, "pool set A (4..0xA0 bytes)");
    run((pool_fn)((char *)m + 0x7B7070), pool_free_b, 19, SIZES_B, "pool set B (0xA4..0x774 bytes)");

    // Speed: worst case for the original (pointer in the last class, many chunks everywhere).
    struct cls big[18];
    for (int k = 0; k < 18; k++) {
        big[k].freelist = NULL; big[k].chunks = NULL; big[k].count = 64;
        for (int j = 0; j < 200; j++) { struct node *n = malloc(sizeof *n); n->base = malloc(64 * SIZES_A[k]); n->next = big[k].chunks; big[k].chunks = n; }
    }
    struct node *target = big[17].chunks;
    for (int j = 0; j < 150; j++) target = target->next;  // deep in the last list
    unsigned out;
    pool_fn orig = (pool_fn)((char *)m + 0x7B6F60);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 20000; i++) { big[17].freelist = NULL; orig(big, target->base + (i % 64) * 0xa0, &out); }
    QueryPerformanceCounter(&t1);
    double us_o = (t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart / 20000;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 20000; i++) { big[17].freelist = NULL; pool_free_a(big, target->base + (i % 64) * 0xa0, &out); }
    QueryPerformanceCounter(&t1);
    double us_m = (t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart / 20000;
    printf("3600 chunks, pointer in the last class: original %.2f us per free, new %.3f us (%.0fx)\n", us_o, us_m, us_o / us_m);

    // install_pool: patches a fresh mapping, refuses a second time (bytes changed).
    CHECK(install_pool(m, quiet) == 1);
    CHECK(((unsigned char *)m)[0x7B6F60] == 0xE9 && ((unsigned char *)m)[0x7B7070] == 0xE9);
    CHECK(install_pool(m, quiet) == 0);
    printf("all pool tests passed\n");
    return 0;
}
