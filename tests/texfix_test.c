// Differential test for texfix.c: the game's own DXT1 compressor (FUN_118e8390, run from empire.retail.dll mapped
// without running it; it calls only FUN_1192eee0, no imports) against tf_compress on random images: random and
// structured pixels (transparent, constant, two-colour and narrow-range blocks, extremes), random widths, pitches,
// bytes per pixel and row ranges. Output bytes and the final output pointer must be identical. tf_compress is also
// called with the stack misaligned by 4, 8 and 12 bytes, as the game's code may call it. Then a speed comparison.
// usage: texfix_test.exe <path to empire.retail.dll>
#define TEXFIX_TEST
#include "../texfix.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned g_rng = 88172645u;
static unsigned rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

// Calls f(src, rows, out) as thiscall with esp lowered by `shift` bytes first.
static void call_shift(compress_fn f, const DWORD *src, const DWORD *rows, unsigned char **out, int shift) {
    struct { compress_fn f; const DWORD *src, *rows; unsigned char **out; int shift; } a = {f, src, rows, out, shift};
    __asm__ volatile("mov %%esp, %%esi\n\t"
                     "sub 16(%%edi), %%esp\n\t"
                     "push 12(%%edi)\n\t"
                     "push 8(%%edi)\n\t"
                     "mov 4(%%edi), %%ecx\n\t"
                     "call *(%%edi)\n\t"
                     "mov %%esi, %%esp"
                     :
                     : "D"(&a)
                     : "esi", "eax", "ecx", "edx", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
}

static void fill(unsigned char *img, size_t n, int style) {
    unsigned char pal[2][4];
    for (int i = 0; i < 8; i++) pal[i / 4][i % 4] = (unsigned char)rnd();
    unsigned base = rnd() & 0xFF;
    for (size_t i = 0; i < n; i++) {
        unsigned r = rnd();
        unsigned char v;
        switch (style) {
        case 0: v = (unsigned char)r; break;                                   // noise
        case 1: v = pal[r & 1][i & 3]; break;                                  // two colours (ties)
        case 2: v = (unsigned char)(base + (r & 7)); break;                   // narrow range
        case 3: v = (r & 1) ? 0xFF : 0; break;                                 // extremes
        case 4: v = (unsigned char)((i >> 2) * 3 + (i & 3) * 40); break;       // gradient
        default: v = pal[0][i & 3]; break;                                     // constant
        }
        if ((i & 3) == 3) {  // alpha: mix of 0, 255 and anything
            unsigned a = rnd() % 8;
            v = a < 2 ? 0 : a < 5 ? 0xFF : (unsigned char)rnd();
            if (style == 5 && (rnd() & 1)) v = 0;
        }
        img[i] = v;
    }
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, w, MAX_PATH);
    HMODULE m = LoadLibraryExW(w, NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(m != NULL);
    unsigned char *b = (unsigned char *)m;
    CHECK(fnv1a(b + RVA_COMPRESS, LEN_COMPRESS) == FNV_COMPRESS && fnv1a(b + RVA_INDICES, LEN_INDICES) == FNV_INDICES);
    compress_fn orig = (compress_fn)(void *)(b + RVA_COMPRESS);

    long cases = 0, blocks = 0;
    for (int iter = 0; iter < 20000; iter++) {
        DWORD width = 1 + rnd() % 96, bpp = (rnd() % 4) ? 4 : 1 + rnd() % 8, height = 1 + rnd() % 40;
        DWORD pitch = width * bpp + (rnd() % 3 ? 0 : rnd() % 64);
        if (pitch < 16) pitch = 16;
        DWORD y0 = rnd() % (height + 1), y1 = rnd() % (height + 8);
        if (rnd() % 4 == 0) y0 &= ~3u;
        DWORD last_row = (y1 > y0 ? y1 : y0) + 8;
        size_t n = (size_t)pitch * last_row + (size_t)width * bpp + 64;  // every byte any block can read
        unsigned char *img = malloc(n);
        fill(img, n, iter % 6);
        if (iter % 50 == 0) for (size_t i = 3; i < n; i += 4) img[i] = 0;  // whole image transparent
        DWORD src[8] = {(DWORD)(UINT_PTR)img, 0, width, 0, pitch, 0, bpp, 0};
        DWORD rows[2] = {y0, y1};
        DWORD nb = (y1 > y0 ? (y1 - y0 + 3) / 4 : 0) * ((width + 3) / 4);
        unsigned char *o1 = malloc(nb * 8 + 16), *o2 = malloc(nb * 8 + 16);
        memset(o1, 0xCD, nb * 8 + 16);
        memset(o2, 0xCD, nb * 8 + 16);
        unsigned char *p1 = o1, *p2 = o2;
        orig(src, rows, &p1);
        call_shift((compress_fn)(void *)tf_compress, src, rows, &p2, (iter % 4) * 4);
        if (p1 - o1 != p2 - o2 || memcmp(o1, o2, nb * 8 + 16) != 0) {
            for (DWORD i = 0; i < nb * 8; i++)
                if (o1[i] != o2[i]) {
                    printf("differs: case %d, width %lu bpp %lu pitch %lu rows %lu-%lu, block %lu byte %lu: game %02X ours %02X\n",
                           iter, width, bpp, pitch, y0, y1, i / 8, i % 8, o1[i], o2[i]);
                    break;
                }
            printf("texfix test FAILED\n");
            return 1;
        }
        cases++;
        blocks += nb;
        free(img);
        free(o1);
        free(o2);
    }
    printf("%ld random images, %ld blocks: identical output and output pointer (stack shifted by 0/4/8/12)\n", cases, blocks);

    // speed: a 1024 x 1024 noisy BGRA image with some transparency
    DWORD W = 1024, H = 1024;
    unsigned char *img = malloc((size_t)W * 4 * (H + 4) + 64), *o = malloc((size_t)W * H / 2 + 64);
    fill(img, (size_t)W * 4 * (H + 4) + 64, 0);
    DWORD src[8] = {(DWORD)(UINT_PTR)img, 0, W, 0, W * 4, 0, 4, 0}, rows[2] = {0, H};
    LARGE_INTEGER f, t0, t1, t2;
    QueryPerformanceFrequency(&f);
    unsigned char *p;
    QueryPerformanceCounter(&t0);
    for (int r = 0; r < 10; r++) { p = o; orig(src, rows, &p); }
    QueryPerformanceCounter(&t1);
    for (int r = 0; r < 10; r++) { p = o; tf_compress(src, rows, &p); }
    QueryPerformanceCounter(&t2);
    double ms_g = 100.0 * (t1.QuadPart - t0.QuadPart) / f.QuadPart, ms_o = 100.0 * (t2.QuadPart - t1.QuadPart) / f.QuadPart;
    printf("1024x1024 image: game %.2f ms, ours %.2f ms (%.1fx)\n", ms_g, ms_o, ms_g / ms_o);
    puts("all texfix tests passed");
    return 0;
}
