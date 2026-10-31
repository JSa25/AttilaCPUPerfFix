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
#include "../gamerva.h"

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

    // growth placement: with the address space in pieces (a 224 MB gap at the top, four 32 MB gaps below it, a 16 MB
    // scrap gap at the bottom for other small reservations, everything else taken), filling mimalloc past its 512 MB
    // must put its 32 MB growth steps into the small gaps and leave the big one whole
    {
        enum { MAXB = 8192, NSMALL = 4, BIGN = 14 };  // pieces of 16 MB
        static unsigned char *blk[MAXB];
        static size_t len[MAXB];
        int nb = 0;
        const UINT_PTR A = 32u << 20, G = 16u << 20;
        MEMORY_BASIC_INFORMATION m;
        for (unsigned char *a = (unsigned char *)0x10000; VirtualQuery(a, &m, sizeof m) == sizeof m;) {
            unsigned char *next = (unsigned char *)m.BaseAddress + m.RegionSize;
            if (m.State == MEM_FREE) {  // take it in pieces that end on 16 MB boundaries
                UINT_PTR lo = (UINT_PTR)m.BaseAddress, hi = lo + m.RegionSize;
                while (lo < hi && nb < MAXB) {
                    UINT_PTR e = (lo + G) & ~(G - 1);
                    if (e > hi || e <= lo) e = hi;
                    if (VirtualAlloc((void *)lo, e - lo, MEM_RESERVE, PAGE_NOACCESS) == (void *)lo) {
                        blk[nb] = (unsigned char *)lo;
                        len[nb++] = e - lo;
                    }
                    lo = e;
                }
            }
            if (next <= a) break;
            a = next;
        }
#define WHOLE(i) (len[i] == G && !((UINT_PTR)blk[i] & (G - 1)))
        int big = -1, small[NSMALL], ns = 0, scrap = -1;
        for (int i = nb - BIGN; i >= 0 && big < 0; i--) {  // the highest 224 MB run starting on 32 MB
            int ok = !((UINT_PTR)blk[i] & (A - 1));
            for (int k = 0; k < BIGN && ok; k++) ok = WHOLE(i + k) && blk[i + k] == blk[i] + k * G;
            if (ok) big = i;
        }
        CHECK(big > 0);
        for (int i = 0; i < big - 2 && scrap < 0; i++)  // the lowest 16 MB piece that cannot hold an aligned 32 MB
            if (WHOLE(i) && ((UINT_PTR)blk[i] & (A - 1))) scrap = i;
        CHECK(scrap >= 0);
        for (int i = scrap + 2; i + 2 < big - 1 && ns < NSMALL; i++) {
            if (!WHOLE(i) || !WHOLE(i + 1) || ((UINT_PTR)blk[i] & (A - 1)) || blk[i + 1] != blk[i] + G) continue;
            if (ns && i <= small[ns - 1] + 2) continue;  // keep a taken piece between gaps
            small[ns++] = i;
        }
        CHECK(ns == NSMALL);
#undef WHOLE
        VirtualFree(blk[scrap], 0, MEM_RELEASE);
        for (int k = 0; k < BIGN; k++) VirtualFree(blk[big + k], 0, MEM_RELEASE);
        for (int k = 0; k < NSMALL; k++) {
            VirtualFree(blk[small[k]], 0, MEM_RELEASE);
            VirtualFree(blk[small[k] + 1], 0, MEM_RELEASE);
        }
        unsigned char *big_at = blk[big];

        enum { M = 900000, SZ = 1000 };
        static unsigned char *sm[M];
        int n = 0, taken = 0, big_whole = 1;
        while (n < M) {
            for (int i = 0; i < 10000 && n < M; i++, n++) {
                sm[n] = g_malloc(SZ);
                CHECK(sm[n]);
                sm[n][0] = (unsigned char)n;
            }
            taken = 0;
            for (int k = 0; k < NSMALL; k++)
                if (VirtualQuery(blk[small[k]], &m, sizeof m) == sizeof m && m.State != MEM_FREE) taken++;
            big_whole = VirtualQuery(big_at, &m, sizeof m) == sizeof m && m.State == MEM_FREE && m.RegionSize >= BIGN * G;
            if (taken == NSMALL || !big_whole) break;
        }
        printf("growth placement: %d small blocks, %d of %d 32 MB gaps used, 224 MB gap %s\n", n, taken, NSMALL,
               big_whole ? "still whole" : "CUT");
        CHECK(taken == NSMALL && big_whole);
        for (int i = 0; i < n; i++) {
            CHECK(sm[i][0] == (unsigned char)i);
            g_free(sm[i]);
        }
        for (int i = 0; i < nb; i++) {
            int hole = (i >= big && i < big + BIGN) || i == scrap;
            for (int k = 0; k < NSMALL; k++) hole |= i == small[k] || i == small[k] + 1;
            if (!hole) VirtualFree(blk[i], 0, MEM_RELEASE);
        }
    }

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

// heapfix.ini given by the test (argv[3] "ini"): a switch, a clamped limit, a count; the DLL must obey it and keep it
static const char INI_TEST[] = "[fixes]\r\nsmall_object_pools=0\r\n[mod]\r\nself_update=0\r\n[tuning]\r\nsmall_heap_limit_mb=800\r\nmenu_textures_kept=7\r\n";

int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4);
    int own_ini = argc == 4 && !strcmp(argv[3], "ini");
    char logp[MAX_PATH], inip[MAX_PATH];
    strcpy(logp, argv[1]);
    char *sl = strrchr(logp, '\\');
    CHECK(sl);
    strcpy(sl + 1, "heapfix.log");
    DeleteFileA(logp);
    strcpy(inip, logp);
    strcpy(inip + (sl + 1 - logp), "heapfix.ini");
    DeleteFileA(inip);
    if (own_ini) {
        FILE *fi = fopen(inip, "wb");
        CHECK(fi && fwrite(INI_TEST, 1, sizeof INI_TEST - 1, fi) == sizeof INI_TEST - 1);
        fclose(fi);
    }
    // the DLSS add-on present but switched off, its settings.txt past 1 KB as the add-on writes it since dlss_preset /
    // sharpness (AGENT_LOG 141: a 1024-byte read missed addon=off at the end; the add-on came back on)
    char fb[MAX_PATH], fbf[MAX_PATH];
    strcpy(fb, logp);
    strcpy(fb + (sl + 1 - logp), "FrameBridge");
    if (own_ini) {   // the release run (build.sh): the dev DLL has no add-on switch
        CreateDirectoryA(fb, NULL);
        snprintf(fbf, sizeof fbf, "%s\\helper64.exe", fb);
        FILE *fh = fopen(fbf, "wb");
        CHECK(fh);
        fclose(fh);
        snprintf(fbf, sizeof fbf, "%s\\settings.txt", fb);
        fh = fopen(fbf, "wb");
        CHECK(fh);
        for (int i = 0; i < 24; i++) fputs("# a comment line as long as the add-on's own ones, to push the switch past 1 KB\r\n", fh);
        fputs("super_resolution=quality\r\naddon=off\r\nversion=3\r\n", fh);
        CHECK(ftell(fh) > 1200);
        fclose(fh);
    }

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
    if (own_ini) {
        snprintf(fbf, sizeof fbf, "%s\\settings.txt", fb);
        DeleteFileA(fbf);
        snprintf(fbf, sizeof fbf, "%s\\helper64.exe", fb);
        DeleteFileA(fbf);
        RemoveDirectoryA(fb);
        CHECK(has("DLSS add-on found but switched off (addon=off in FrameBridge\\settings.txt)"));
    }
    CHECK(has("was loaded before empire.retail.dll"));
    CHECK(has("empire.retail.dll is loading now - installing the fixes"));
    CHECK(has("game heap replaced"));
    {   // heapfix.ini: obeyed and left as it was, or created with the defaults when missing
        static char ini[4096];
        FILE *fi = fopen(inip, "rb");
        CHECK(fi);
        ini[fread(ini, 1, sizeof ini - 1, fi)] = 0;
        fclose(fi);
        if (own_ini) {
            CHECK(!strcmp(ini, INI_TEST) && has("self-update off (self_update=0 in heapfix.ini)"));
            CHECK(has("pool free lookup switched off (heapfix.ini or pool_off.txt) - not replaced") && !has("pool free lookup replaced"));
            CHECK(has("grows to 768 MB") && has("menu textures: the last 7 closed ones stay loaded"));
            CHECK(has("OFF  FPS: small-object pools (switched off in heapfix.ini, or by its _off.txt file)"));
        } else {
            CHECK(strstr(ini, "[fixes]") && strstr(ini, "small_object_pools=1") && strstr(ini, "self_update=1") && strstr(ini, "crash_logging=1"));
            CHECK(!has("self-update off") && has("crash logging on"));
            CHECK(has("pool free lookup replaced") && has("menu textures: the last 1024 closed ones stay loaded"));
        }
    }
    CHECK(has("battle draw-list sort replaced") && has("render-list sort (+0x4C/+0x48) replaced"));
    CHECK(has("batch fix installed") && has("campaign fix: 4 of 4") && has("x87 fix: 1277 of 1277"));
    CHECK(has("height cache installed") && has("memcpy fast path installed"));
    CHECK((has("loop prefetch: 6 of 6") || has("loop prefetch: 7 of 7")) &&  // dev builds: + effects emitter site
          (has("frame hook ready") || has("FPS logger ready")));                // dev builds: FPS logger
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
    // the loading fixes hook the game's imports: the loader must not have overwritten them after the notification
    CHECK(has("file I/O fix installed") && has("attribute cache installed") && has("texture compression replaced"));
    IMAGE_NT_HEADERS *gnt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + gnt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    int hooked = 0;
    for (; d->Name; d++) {
        if (_stricmp((char *)(b + d->Name), "KERNEL32.dll") != 0) continue;
        IMAGE_THUNK_DATA *nm = (IMAGE_THUNK_DATA *)(b + d->OriginalFirstThunk), *iat = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; nm->u1.AddressOfData; nm++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(nm->u1.Ordinal)) continue;
            const char *n = (char *)((IMAGE_IMPORT_BY_NAME *)(b + nm->u1.AddressOfData))->Name;
            if (strcmp(n, "GetFileAttributesW") && strcmp(n, "CreateFileW") && strcmp(n, "ReadFile")) continue;
            unsigned char *to = (unsigned char *)iat->u1.Function;
            if (!(to >= lo && to < hi)) printf("import %s points at %p, outside this DLL\n", n, (void *)to);
            CHECK(to >= lo && to < hi);
            hooked++;
        }
    }
    CHECK(hooked == 3);
    // the texture compressor through the game's patched entry (dev builds with texfix_verify.txt: through the
    // trampoline to the game's own code as well): a 64x64 image, every block written, same result twice
    {
        typedef void(__attribute__((thiscall)) * comp_fn)(const DWORD *, const DWORD *, unsigned char **);
        static unsigned char img[64 * 64 * 4 + 64], o1[256 * 8 + 16], o2[256 * 8 + 16];
        for (unsigned i = 0; i < sizeof img; i++) img[i] = (unsigned char)(i * 2654435761u >> 13);
        DWORD src[8] = {(DWORD)(UINT_PTR)img, 0, 64, 0, 256, 0, 4, 0}, rows[2] = {0, 64};
        memset(o1, 0xCD, sizeof o1);
        memset(o2, 0xCD, sizeof o2);
        unsigned char *p1 = o1, *p2 = o2;
        ((comp_fn)(void *)(b + game_rva(b, 0x18E8390)))(src, rows, &p1);
        ((comp_fn)(void *)(b + game_rva(b, 0x18E8390)))(src, rows, &p2);
        CHECK(p1 == o1 + 256 * 8 && p2 == o2 + 256 * 8 && !memcmp(o1, o2, sizeof o1) && o1[256 * 8] == 0xCD);
    }
    printf("late install: game DLL relocated to %p, all fixes installed from the load notification\n", (void *)game);
    heap_checks(b);
    printf("all late-load tests passed\n");
    return 0;
}
