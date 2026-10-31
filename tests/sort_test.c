// Differential test: the game's two render-list sorts (FUN_117ad5d0 by +0x6C/+0x68, FUN_117acb50 by +0x4C/+0x48)
// vs sortfix.c's replacements on the same random lists, plus a speed comparison.
// usage: sort_test.exe <path to empire.retail.dll>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../sortfix.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }
static unsigned rng = 99;
static unsigned rnd(unsigned n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

static void **make(unsigned n, unsigned range6c, unsigned range68) {
    void **v = malloc(n * sizeof *v);
    for (unsigned i = 0; i < n; i++) {
        unsigned char *o = malloc(0x80 + rnd(256));  // scattered like real objects
        *(unsigned *)(o + 0x6C) = range6c ? rnd(range6c) : 0xFFFFFFFFu - rnd(3);
        *(unsigned *)(o + 0x68) = range68 ? rnd(range68) : rnd(0xFFFFFFFFu);
        *(unsigned *)(o + 0x4C) = range6c ? rnd(range6c) : 0xFFFFFFFFu - rnd(3);  // the second sort's keys
        *(unsigned *)(o + 0x48) = range68 ? rnd(range68) : rnd(0xFFFFFFFFu);
        v[i] = o;
    }
    return v;
}

static int cmp_ptr(const void *a, const void *b) {
    UINT_PTR x = *(UINT_PTR *)a, y = *(UINT_PTR *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, w, MAX_PATH);
    HMODULE m = LoadLibraryExW(w, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(m != NULL);
    sort_fn origs[2] = {(sort_fn)((char *)m + game_rva(m, g_sorts[0].rva)), (sort_fn)((char *)m + game_rva(m, g_sorts[1].rva))};
    sort_fn mine[2] = {draw_sort, list_sort};
    sort_fn orig = origs[0];

    unsigned sizes[] = {0, 1, 2, 3, 31, 32, 33, 100, 1000, 5000, 30000};
    unsigned ranges[][2] = {{4, 4}, {1, 1000}, {1000, 0}, {0, 0}, {100000, 100000}};
    int cases = 0;
    for (int w2 = 0; w2 < 2; w2++)
    for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++)
        for (unsigned r = 0; r < sizeof ranges / sizeof *ranges; r++) {
            unsigned n = sizes[s];
            void **a = make(n ? n : 1, ranges[r][0], ranges[r][1]);
            void **b = malloc((n ? n : 1) * sizeof *b), **ca = malloc((n ? n : 1) * sizeof *ca), **cb = malloc((n ? n : 1) * sizeof *cb);
            memcpy(b, a, n * sizeof *a);
            origs[w2](a, a + n, (int)n, NULL);
            mine[w2](b, b + n, (int)n, NULL);
            const struct sort_site *ss = &g_sorts[w2];
            for (unsigned i = 0; i < n; i++) CHECK(key_at(a[i], ss) == key_at(b[i], ss));  // same key order
            for (unsigned i = 1; i < n; i++) CHECK(key_at(b[i - 1], ss) <= key_at(b[i], ss));
            memcpy(ca, a, n * sizeof *a); memcpy(cb, b, n * sizeof *b);
            qsort(ca, n, sizeof *ca, cmp_ptr); qsort(cb, n, sizeof *cb, cmp_ptr);
            CHECK(memcmp(ca, cb, n * sizeof *ca) == 0);  // same items
            cases++;
        }
    printf("%d lists over both sorts (0..30000 items, few/many/huge/equal keys): same key order as the game's sorts\n", cases);

    // Speed: 20,000 scattered items, realistic key spread, average of 20 runs.
    unsigned n = 20000;
    void **base = make(n, 64, 5000), **work = malloc(n * sizeof *work);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    double us[2] = {0, 0};
    for (int k = 0; k < 2; k++)
        for (int r = 0; r < 20; r++) {
            memcpy(work, base, n * sizeof *work);
            QueryPerformanceCounter(&t0);
            if (k == 0) orig(work, work + n, (int)n, NULL); else draw_sort(work, work + n, (int)n, NULL);
            QueryPerformanceCounter(&t1);
            us[k] += (t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart / 20;
        }
    printf("20,000 items: game's sort %.0f us, new %.0f us (%.1fx)\n", us[0], us[1], us[0] / us[1]);
    double u2[2] = {0, 0};
    for (int k = 0; k < 2; k++)
        for (int r = 0; r < 20; r++) {
            memcpy(work, base, n * sizeof *work);
            QueryPerformanceCounter(&t0);
            if (k == 0) origs[1](work, work + n, (int)n, NULL); else list_sort(work, work + n, (int)n, NULL);
            QueryPerformanceCounter(&t1);
            u2[k] += (t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart / 20;
        }
    printf("20,000 items, second sort: game's %.0f us, new %.0f us (%.1fx)\n", u2[0], u2[1], u2[0] / u2[1]);

    CHECK(install_sortfix(m, say) == 2);
    CHECK(((unsigned char *)m)[game_rva(m, RVA_DRAWSORT)] == 0xE9 && ((unsigned char *)m)[game_rva(m, g_sorts[1].rva)] == 0xE9);
    void **c = make(500, 10, 10);
    orig(c, c + 500, 500, NULL);  // now jumps to draw_sort through the patched entry
    for (unsigned i = 1; i < 500; i++) CHECK(key_of(c[i - 1]) <= key_of(c[i]));
    origs[1](c, c + 500, 500, NULL);  // the second entry jumps to list_sort
    for (unsigned i = 1; i < 500; i++) CHECK(key_at(c[i - 1], &g_sorts[1]) <= key_at(c[i], &g_sorts[1]));
    CHECK(install_sortfix(m, say) == 0);  // already patched: hashes differ
    printf("all sort tests passed\n");
    return 0;
}
