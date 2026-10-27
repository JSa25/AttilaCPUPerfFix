// Test for attrcache.c: every answer (attributes and last error) must equal plain GetFileAttributesW, also while
// files and folders are created, renamed and deleted between queries. A temporary tree with 15 roots stands in for
// the game's search roots. A second run with the watches ignored must find differences (proves the test can
// catch a stale cache). Every other query is a read-only open (CreateFileW, OPEN_EXISTING, the game's flags) instead
// of a check: handle validity and last error must equal plain CreateFileW. Also times cached misses against Windows.
#include "../attrcache.c"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static wchar_t g_base[MAX_PATH];
static unsigned g_rng = 2463534242u;
static unsigned rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static const wchar_t *g_comp[] = {L"vfx", L"Textures", L"fx", L"VariantMeshes", L"rigidmodels", L"data", L"a", L"b",
                                  L"ui", L"x.txt", L"y.dds", L"Readme", L"z.", L"..", L".", L"unit", L"nul", L"Com1.txt"};
#define NCOMP (sizeof g_comp / sizeof *g_comp)

static void rel_path(wchar_t *out) {  // random relative path, sometimes with odd forms
    out[0] = 0;
    int depth = 1 + rnd() % 4;
    for (int i = 0; i < depth; i++) {
        if (i) wcscat(out, (rnd() % 40) ? L"\\" : (rnd() % 2 ? L"/" : L"\\\\"));
        wcscat(out, g_comp[rnd() % NCOMP]);
    }
    if (rnd() % 10 == 0) wcscat(out, L"\\");
}

static void full_path(wchar_t *out, int root, const wchar_t *rel) {
    swprintf(out, MAX_PATH, L"%ls\\r%d\\%ls", g_base, root, rel);
    if (rnd() % 8 == 0) for (wchar_t *c = out; *c; c++) *c = (wchar_t)towupper(*c);
}

static void mutate(void) {  // create / delete / rename something under a random root
    wchar_t rel[MAX_PATH], p[MAX_PATH], p2[MAX_PATH];
    rel_path(rel);
    swprintf(p, MAX_PATH, L"%ls\\r%u\\%ls", g_base, rnd() % 15, rel);
    switch (rnd() % 5) {
    case 0: CreateDirectoryW(p, NULL); break;
    case 1: { HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL); if (h != INVALID_HANDLE_VALUE) CloseHandle(h); break; }
    case 2: RemoveDirectoryW(p); break;
    case 3: DeleteFileW(p); break;
    case 4: swprintf(p2, MAX_PATH, L"%ls\\r%u\\%ls", g_base, rnd() % 15, g_comp[rnd() % NCOMP]); MoveFileW(p, p2); break;
    }
}

static long run(int queries, int mutate_every) {
    long bad = 0;
    for (int i = 0; i < queries; i++) {
        if (mutate_every && i % mutate_every == 0) mutate();
        wchar_t rel[MAX_PATH], p[MAX_PATH];
        rel_path(rel);
        full_path(p, rnd() % 15, rel);
        DWORD a1, e1, a2, e2;
        if (rnd() % 2) {
            SetLastError(1234);
            a1 = ac_GetFileAttributesW(p), e1 = GetLastError();
            SetLastError(1234);
            a2 = GetFileAttributesW(p), e2 = GetLastError();
        } else {  // the game's flag sets: buffered, unbuffered + overlapped + sequential, read-only attribute, and its
                  // stat (attributes access, backup semantics: also opens folders)
            static const DWORD fl[4] = {0x08000001, 0x68000001, 0x80, FILE_FLAG_BACKUP_SEMANTICS};
            int k = rnd() % 4;
            DWORD f = fl[k], sh = rnd() % 2 ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE;
            DWORD acc = k == 3 ? FILE_READ_ATTRIBUTES : GENERIC_READ;
            if (k == 3) sh = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
            SetLastError(1234);
            HANDLE h1 = ac_CreateFileW(p, acc, sh, NULL, OPEN_EXISTING, f, NULL);
            e1 = GetLastError();
            if (h1 != INVALID_HANDLE_VALUE) CloseHandle(h1);
            SetLastError(1234);
            HANDLE h2 = CreateFileW(p, acc, sh, NULL, OPEN_EXISTING, f, NULL);
            e2 = GetLastError();
            if (h2 != INVALID_HANDLE_VALUE) CloseHandle(h2);
            a1 = h1 == INVALID_HANDLE_VALUE, a2 = h2 == INVALID_HANDLE_VALUE;  // 1 = failed
        }
        if (a1 != a2 || e1 != e2) {
            if (bad++ < 5) {
                printf("  differs: %ls -> cache %08lX/%lu, Windows %08lX/%lu\n", p, a1, e1, a2, e2);
                if (getenv("AC_DEBUG")) {
                    DWORD wr = WaitForMultipleObjects((DWORD)g_nwatch, g_watch, FALSE, 0);
                    printf("     watches %ld, wait result %08lX (error %lu)\n", g_nwatch, wr, GetLastError());
                    wchar_t q[MAXPATHC + 2];
                    normalize(p, q);
                    for (LONG w = 0; w < g_nwatch; w++)
                        if (!wcsncmp(q, g_watch_dir[w], wcslen(g_watch_dir[w])))
                            printf("     watch on a parent: %ls signalled %d\n", g_watch_dir[w], WaitForSingleObject(g_watch[w], 0) == WAIT_OBJECT_0);
                }
            }
        }
    }
    return bad;
}

static void del_tree(const wchar_t *dir) {
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t p[MAX_PATH];
            swprintf(p, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) del_tree(p); else DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void make_tree(void) {
    del_tree(g_base);
    CreateDirectoryW(g_base, NULL);
    for (int r = 0; r < 15; r++) {
        wchar_t p[MAX_PATH];
        swprintf(p, MAX_PATH, L"%ls\\r%d", g_base, r);
        CreateDirectoryW(p, NULL);
        if (r % 4 == 0) { wcscat(p, L"\\vfx"); CreateDirectoryW(p, NULL); wcscat(p, L"\\textures"); CreateDirectoryW(p, NULL); }
        if (r % 5 == 1) {
            swprintf(p, MAX_PATH, L"%ls\\r%d\\x.txt", g_base, r);
            HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    }
}

static void reset_cache(void) {
    AcquireSRWLockExclusive(&g_lock);
    clear_table();
    for (LONG i = 0; i < g_nwatch; i++) FindCloseChangeNotification(g_watch[i]);
    g_nwatch = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

int main(void) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(g_base, MAX_PATH, L"%lsattrcache_test_%lu", tmp, GetCurrentProcessId());
    if (!attrcache_init(0, NULL)) { puts("init failed"); return 1; }
    int fail = 0;

    make_tree();
    long b = run(200000, 0);
    printf("200,000 random queries, tree unchanged: %ld differences, %ld check + %ld open cache answers, %ld names learned\n",
           b, g_hits, g_open_hits, g_inserts);
    fail |= b != 0 || g_hits < 25000 || g_open_hits < 25000;

    long h0 = g_hits + g_open_hits;
    b = run(200000, 50);
    printf("200,000 random queries, a change every 50 queries: %ld differences, %ld cache answers, %ld resets\n", b, g_hits + g_open_hits - h0, g_resets);
    fail |= b != 0;

    b = run(50000, 1);
    printf("50,000 random queries, a change before every query: %ld differences\n", b);
    fail |= b != 0;

    // the same with the watches ignored must go wrong, or the test proves nothing
    make_tree();
    reset_cache();
    g_nowatch = 1;
    b = run(100000, 50);
    g_nowatch = 0;
    printf("watches ignored (must differ): %ld differences\n", b);
    fail |= b == 0;

    // speed: misses under a missing folder, cache vs Windows
    make_tree();
    reset_cache();
    wchar_t p[MAX_PATH];
    LARGE_INTEGER f, t0, t1, t2;
    QueryPerformanceFrequency(&f);
    const int N = 200000;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < N; i++) { swprintf(p, MAX_PATH, L"%ls\\r%d\\variantmeshes\\unit\\m%d.variantmeshdefinition", g_base, i % 15, i % 1000); ac_GetFileAttributesW(p); }
    QueryPerformanceCounter(&t1);
    for (int i = 0; i < N; i++) { swprintf(p, MAX_PATH, L"%ls\\r%d\\variantmeshes\\unit\\m%d.variantmeshdefinition", g_base, i % 15, i % 1000); GetFileAttributesW(p); }
    QueryPerformanceCounter(&t2);
    double us_c = 1e6 * (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart / N, us_w = 1e6 * (double)(t2.QuadPart - t1.QuadPart) / f.QuadPart / N;
    printf("missing-file checks: cache %.2f us, Windows %.2f us per call (%.1fx)\n", us_c, us_w, us_w / us_c);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < N; i++) { swprintf(p, MAX_PATH, L"%ls\\r%d\\variantmeshes\\unit\\m%d.variantmeshdefinition", g_base, i % 15, i % 1000); ac_CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0x68000001, NULL); }
    QueryPerformanceCounter(&t1);
    for (int i = 0; i < N; i++) { swprintf(p, MAX_PATH, L"%ls\\r%d\\variantmeshes\\unit\\m%d.variantmeshdefinition", g_base, i % 15, i % 1000); CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0x68000001, NULL); }
    QueryPerformanceCounter(&t2);
    us_c = 1e6 * (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart / N, us_w = 1e6 * (double)(t2.QuadPart - t1.QuadPart) / f.QuadPart / N;
    printf("missing-file opens: cache %.2f us, Windows %.2f us per call (%.1fx)\n", us_c, us_w, us_w / us_c);

    reset_cache();
    del_tree(g_base);
    puts(fail ? "attrcache test FAILED" : "attrcache test passed");
    return fail;
}
