// Tests heightcache.c on a stand-in for FUN_118f9150 (same prologue, thiscall, ret 8, float in ST0) with
// a fake terrain object and fake game globals at their RVAs: results unchanged in verify mode, repeats
// counted as hits, a hidden input change caught as a mismatch, a changed state value (water factor) and
// the water branch never served from the cache, cache mode returns the remembered bits, other threads pass.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../heightcache.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }

static float g_hidden;
static int g_orig_calls;
float __cdecl ref_height(unsigned char *terrain, const float *pos, int flag) {
    g_orig_calls++;
    float wf = AT(AT(terrain, 0x794, unsigned char *), 0x7C, float);
    return pos[0] * 0.5f - pos[1] * 0.25f + (float)(flag & 0xFF) + wf + g_hidden;
}

static char g_switch;
char __fastcall game_switch(void *self) { (void)self; return g_switch; }

typedef float(__thiscall *hq_fn)(void *, const float *, int);
static hq_fn g_fn;
static unsigned char *g_terrain;
static DWORD WINAPI other(LPVOID p) {
    static const float pos[2] = {1, 2};
    *(float *)p = g_fn(g_terrain, pos, 0);
    return 0;
}

int main(void) {
    unsigned char *img = VirtualAlloc(NULL, 0x2B00000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK(img);
    // fake game: DAT_12a21d18 -> object with vtable slot +0x2C4 = game_switch
    static void *vtable[0x300 / 4], *game[4];
    vtable[0x2C4 / 4] = (void *)game_switch;
    game[0] = vtable;
    *(void **)(img + 0x2A21D18) = game;
    // fake terrain: +0x794 terrain data (+0x18 mode object, +0x7C water factor), +0x79C patch root
    static unsigned char terrain[0x800], td[0x100], mode_obj[0x100], root[0x40];
    *(void **)(terrain + 0x794) = td;
    *(void **)(td + 0x18) = mode_obj;
    *(float *)(td + 0x7C) = 0.0f;
    *(void **)(terrain + 0x79C) = root;
    g_terrain = terrain;

    // stand-in: prologue; push [ebp+0Ch] (flag); push [ebp+8] (pos); push ecx; call ref_height; add esp,12;
    // mov esp,ebp; pop ebp; ret 8
    unsigned char *f = img + RVA_HEIGHTQ;
    unsigned char code[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0xFF, 0x75, 0x0C, 0xFF, 0x75, 0x08, 0x51,
                            0xE8, 0, 0, 0, 0, 0x83, 0xC4, 0x0C, 0x8B, 0xE5, 0x5D, 0xC2, 0x08, 0x00};
    DWORD rel = (DWORD)((unsigned char *)ref_height - (f + 18));
    memcpy(code + 14, &rel, 4);
    memcpy(f, code, sizeof code);
    g_fn = (hq_fn)(void *)f;
    float pts[50][2];
    for (int i = 0; i < 50; i++) { pts[i][0] = i * 1.5f; pts[i][1] = 100 - i; }
    float plain[50];
    for (int i = 0; i < 50; i++) plain[i] = g_fn(terrain, pts[i], 1);

    CHECK(install_heightcache((HMODULE)img, 1, say) == 1);
    for (int rep = 0; rep < 3; rep++)
        for (int i = 0; i < 50; i++) CHECK(g_fn(terrain, pts[i], 1) == plain[i]);
    printf("verify: %ld calls, %ld hits, %ld mismatches\n", g_hq[0], g_hq[2], g_hq[3]);
    CHECK(g_hq[0] == 150 && g_hq[2] == 100 && g_hq[3] == 0);
    g_hidden = 1;  // an input the cache does not know about changes: caught
    g_fn(terrain, pts[0], 1);
    CHECK(g_hq[3] == 1);
    g_hidden = 0;
    g_fn(terrain, pts[0], 1);

    g_hq_mode = 2;
    int before = g_orig_calls;
    for (int i = 0; i < 50; i++) CHECK(g_fn(terrain, pts[i], 1) == plain[i]);
    CHECK(g_orig_calls == before);  // all served from the cache
    CHECK(g_fn(terrain, pts[0], 2) != plain[0] && g_orig_calls == before + 1);  // another flag is another key

    *(float *)(td + 0x7C) = 3.0f;  // a state value changes: recomputed, the new value returned
    CHECK(g_fn(terrain, pts[1], 1) == plain[1] + 3.0f && g_orig_calls == before + 2);
    *(float *)(td + 0x7C) = 0.0f;
    CHECK(g_fn(terrain, pts[1], 1) == plain[1]);

    g_switch = 1;  // water branch active (switch on and this+0x128 set): never cached
    *(void **)(terrain + 0x128) = td;
    int b2 = g_orig_calls;
    g_fn(terrain, pts[2], 1); g_fn(terrain, pts[2], 1);
    CHECK(g_orig_calls == b2 + 2);
    g_switch = 0;
    *(void **)(terrain + 0x128) = NULL;

    float r = 0;
    int b3 = g_orig_calls;
    HANDLE t = CreateThread(NULL, 0, other, &r, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    CHECK(g_orig_calls == b3 + 1);
    printf("all heightcache tests passed\n");
    return 0;
}
