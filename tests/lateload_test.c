// Test for the late install (heapfix.c, wait_for_game): this DLL is loaded first, as when an overlay or another hook
// DLL pulls dinput8.dll in early, and the game DLL afterwards. Its preferred address is taken first, so Windows has to
// relocate it: the fixes must install from the load notification, with every byte check passing (they compare the
// relocated addresses, so a check made before relocation would fail) and the jumps intact afterwards.
// The game DLL's own startup code runs too, on the patched heap, as in the game.
// usage: lateload_test.exe <path to the built dinput8.dll> <path to empire.retail.dll>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static char g_log[1 << 20];

static int has(const char *s) { return strstr(g_log, s) != NULL; }

// Through the game's patched entry points: blocks over 16 KB go to the game's own dlmalloc (its imports are resolved
// here, unlike in heapfix_test), small ones to mimalloc, and the two must hand blocks over intact.
static void *(__cdecl *g_malloc)(size_t);
static void(__cdecl *g_free)(void *);
static void *(__cdecl *g_realloc)(void *, size_t);
static volatile LONG g_errors;

static size_t free_space(size_t *largest) {
    MEMORY_BASIC_INFORMATION m;
    size_t total = 0;
    *largest = 0;
    for (unsigned char *a = (unsigned char *)0x10000; VirtualQuery(a, &m, sizeof m) == sizeof m && (unsigned char *)m.BaseAddress + m.RegionSize > a;
         a = (unsigned char *)m.BaseAddress + m.RegionSize)
        if (m.State == MEM_FREE) { total += m.RegionSize; if (m.RegionSize > *largest) *largest = m.RegionSize; }
    return total;
}

static DWORD WINAPI mixed(LPVOID arg) {  // sizes on both sides of the split, reallocs across it, from 8 threads
    unsigned seed = (unsigned)(UINT_PTR)arg * 2654435761u;
    enum { SLOTS = 256 };
    unsigned char *slot[SLOTS] = {0};
    size_t len[SLOTS] = {0};
    for (int it = 0; it < 60000; it++) {
        seed = seed * 1103515245u + 12345u;
        int s = (seed >> 8) % SLOTS;
        size_t n = (seed & 0x30000) ? 81 + (seed >> 18) % 8000 : 16000 + (seed >> 18) % 300000;
        if (slot[s]) {
            if (slot[s][0] != (unsigned char)s || slot[s][len[s] - 1] != (unsigned char)s) InterlockedIncrement(&g_errors);
            if (seed & 0x40000) {
                unsigned char *p = g_realloc(slot[s], n);
                if (!p) { InterlockedIncrement(&g_errors); continue; }
                if (p[0] != (unsigned char)s) InterlockedIncrement(&g_errors);
                slot[s] = p;
            } else {
                g_free(slot[s]);
                slot[s] = NULL;
                continue;
            }
        } else if (!(slot[s] = g_malloc(n))) { InterlockedIncrement(&g_errors); continue; }
        len[s] = n;
        slot[s][0] = slot[s][n - 1] = (unsigned char)s;
    }
    for (int s = 0; s < SLOTS; s++) g_free(slot[s]);
    return 0;
}

static void heap_checks(unsigned char *b) {
    g_malloc = (void *)(b + 0x112D80);
    g_free = (void *)(b + 0x112730);
    g_realloc = (void *)(b + 0x113040);
    int(__cdecl * g_trim)(size_t) = (void *)(b + 0x1126E0);  // the game's own dlmalloc_trim, left unpatched

    unsigned char *s = g_malloc(100);
    for (int i = 0; i < 100; i++) s[i] = (unsigned char)i;
    unsigned char *big = g_realloc(s, 50000);  // small -> dlmalloc
    CHECK(big);
    for (int i = 0; i < 100; i++) CHECK(big[i] == (unsigned char)i);
    big[49999] = 7;
    unsigned char *c = g_realloc(big, 64);  // dlmalloc block shrunk: stays a valid block
    CHECK(c);
    for (int i = 0; i < 64; i++) CHECK(c[i] == (unsigned char)i);
    g_free(c);
    unsigned char *e = g_malloc(16384), *f = g_malloc(16385);  // at and just over the split
    CHECK(e && f);
    memset(e, 1, 16384);
    memset(f, 2, 16385);
    e = g_realloc(e, 20384);
    CHECK(e && e[16383] == 1 && f[16384] == 2);
    g_free(e);
    g_free(f);
    void *huge = g_malloc(230608464);  // the ~220 MB block the game asks for on "exit to main menu"
    CHECK(huge);
    memset(huge, 3, 4096);
    g_free(huge);

    // big blocks freed back to dlmalloc are reused for the next ones (it grows in steps, unlike the doubling
    // reservations of a Windows heap), so a second round of the same blocks takes no new address space
    enum { N = 1500 };
    static void *blk[N];
    size_t big0, free0 = free_space(&big0), free1 = 0, free2 = 0;
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < N; i++) { blk[i] = g_malloc(20000 + (i % 7) * 30000); CHECK(blk[i]); ((char *)blk[i])[0] = 1; }
        size_t bg;
        if (round == 0) free1 = free_space(&bg); else free2 = free_space(&bg);
        for (int i = 0; i < N; i++) g_free(blk[i]);
        g_trim(0);
    }
    printf("big blocks: address space free %u MB, after round 1 %u MB, after round 2 %u MB\n",
           (unsigned)(free0 >> 20), (unsigned)(free1 >> 20), (unsigned)(free2 >> 20));
    CHECK(free0 - free1 > (100u << 20));    // the blocks really took address space
    CHECK(free2 + (16u << 20) >= free1);    // the second round reused it

    // more small blocks than mimalloc's fixed reservation holds: the rest must spill to dlmalloc, intact
    {
        enum { M = 950000, SZ = 1000 };  // ~950 MB, over the 768 MB cap
        static unsigned char *sm[M];
        size_t f0 = free_space(&big0);
        for (int i = 0; i < M; i++) {
            sm[i] = g_malloc(SZ);
            CHECK(sm[i]);
            sm[i][0] = sm[i][SZ - 1] = (unsigned char)i;
        }
        size_t f1 = free_space(&big0);
        unsigned char *grown = g_realloc(sm[0], 4000);  // a small block growing while mimalloc is full
        CHECK(grown && grown[0] == 0);
        sm[0] = grown;
        sm[0][SZ - 1] = 0;
        HANDLE nt[4];  // threads started after the cap is full still get memory
        for (int i = 0; i < 4; i++) nt[i] = CreateThread(NULL, 0, mixed, (LPVOID)(UINT_PTR)(100 + i), 0, NULL);
        WaitForMultipleObjects(4, nt, TRUE, INFINITE);
        CHECK(g_errors == 0);
        for (int i = 0; i < M; i++) {
            CHECK(sm[i][0] == (unsigned char)i && sm[i][SZ - 1] == (unsigned char)i);
            g_free(sm[i]);
        }
        printf("small blocks past the cap: %d x %d bytes ok (address space used %u MB), new threads ok\n", M, SZ,
               (unsigned)((f0 - f1) >> 20));
    }

    HANDLE th[8];
    for (int i = 0; i < 8; i++) th[i] = CreateThread(NULL, 0, mixed, (LPVOID)(UINT_PTR)(i + 1), 0, NULL);
    WaitForMultipleObjects(8, th, TRUE, INFINITE);
    printf("mixed sizes on 8 threads through the game's entry points: errors %ld\n", g_errors);
    CHECK(g_errors == 0);
}

int main(int argc, char **argv) {
    CHECK(argc == 3);
    char logp[MAX_PATH];
    strcpy(logp, argv[1]);
    char *sl = strrchr(logp, '\\');
    CHECK(sl);
    strcpy(sl + 1, "heapfix.log");
    DeleteFileA(logp);

    void *taken = VirtualAlloc((void *)0x10000000, 0x100000, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(taken == (void *)0x10000000);
    HMODULE me = LoadLibraryA(argv[1]);
    CHECK(me);
    CHECK(GetModuleHandleA("empire.retail.dll") == NULL);
    // a normal load (the notification is not sent for DONT_RESOLVE_DLL_REFERENCES), with the game folder on the
    // search path for its other imports; its DINPUT8.dll import binds to the copy already loaded
    char gdir[MAX_PATH];
    strcpy(gdir, argv[2]);
    char *gs = strrchr(gdir, '\\');
    CHECK(gs);
    *gs = 0;
    SetDllDirectoryA(gdir);
    HMODULE game = LoadLibraryA(argv[2]);
    CHECK(game && game != (HMODULE)0x10000000);

    FILE *f = fopen(logp, "rb");
    CHECK(f);
    g_log[fread(g_log, 1, sizeof g_log - 1, f)] = 0;
    fclose(f);
    fputs(g_log, stdout);
    CHECK(has("was loaded before empire.retail.dll"));
    CHECK(has("empire.retail.dll is loading now - installing the fixes"));
    CHECK(has("game heap replaced"));
    CHECK(has("battle draw-list sort replaced") && has("render-list sort (+0x4C/+0x48) replaced"));
    CHECK(has("batch fix installed") && has("campaign fix: 4 of 4") && has("x87 fix: 1277 of 1277"));
    CHECK(has("height cache installed") && has("memcpy fast path installed"));
    CHECK(has("loop prefetch: 6 of 6") && has("frame hook ready"));
    CHECK(!has("does not match") && !has("differs") && !has("skipped") && !has("not the game"));

    // the three heap jumps must point into this DLL; dlmalloc's trim stays the game's own, relocated address intact
    unsigned char *b = (unsigned char *)game;
    const DWORD rvas[] = {0x112D80, 0x112730, 0x113040};
    unsigned char *lo = (unsigned char *)me, *hi = lo + ((IMAGE_NT_HEADERS *)(lo + ((IMAGE_DOS_HEADER *)lo)->e_lfanew))->OptionalHeader.SizeOfImage;
    for (int i = 0; i < 3; i++) {
        unsigned char *site = b + rvas[i];
        DWORD rel;
        memcpy(&rel, site + 1, 4);
        unsigned char *to = site + 5 + rel;
        CHECK(site[0] == 0xE9 && to >= lo && to < hi);
    }
    DWORD trim_abs;
    memcpy(&trim_abs, b + 0x1126E0 + 2, 4);
    CHECK(b[0x1126E0] == 0x83 && trim_abs == 0x11F20BBC - 0x10000000 + (DWORD)(UINT_PTR)b);
    printf("late install: game DLL relocated to %p, all fixes installed from the load notification\n", (void *)game);
    heap_checks(b);
    printf("all late-load tests passed\n");
    return 0;
}
