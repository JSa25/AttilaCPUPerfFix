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

    // the heap jumps (dltrim's overlaps a relocated address) must still point into this DLL
    unsigned char *b = (unsigned char *)game;
    const DWORD rvas[] = {0x112D80, 0x112730, 0x113040, 0x1126E0};
    unsigned char *lo = (unsigned char *)me, *hi = lo + ((IMAGE_NT_HEADERS *)(lo + ((IMAGE_DOS_HEADER *)lo)->e_lfanew))->OptionalHeader.SizeOfImage;
    for (int i = 0; i < 4; i++) {
        unsigned char *site = b + rvas[i];
        DWORD rel;
        memcpy(&rel, site + 1, 4);
        unsigned char *to = site + 5 + rel;
        CHECK(site[0] == 0xE9 && to >= lo && to < hi);
    }
    printf("late install: game DLL relocated to %p, all fixes installed from the load notification\n", (void *)game);
    printf("all late-load tests passed\n");
    return 0;
}
