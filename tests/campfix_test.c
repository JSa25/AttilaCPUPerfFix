// Differential test for campfix.c: each site's original instruction block (read from empire.retail.dll,
// with the store just before it that puts the input on the stack) and its replacement run inside the
// same wrapper on millions of inputs - random bit patterns plus edge values (0, denormals, integers and
// their neighbours, 2^23/2^24/2^31 boundaries, huge, inf, NaN). The produced integer must be identical
// on each site's input domain. Also checks the rounding-mode guard of the terrain height patch.
// usage: campfix_test.exe <path to empire.retail.dll>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include "../campfix.c"

volatile LONG g_campfix_state = -1;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }

struct spec { int site; unsigned prefix_rva; unsigned char load_xmm1, slot; int positive_only; };
static const struct spec SPECS[] = {
    {0, 0xD79AC2, 0, 0x0C, 1},   // fog 1: movss [esp+4],xmm0 ; block ; result [esp+0Ch]; only reached for x > 0
    {1, 0xD79B0C, 0, 0x08, 1},   // fog 2: movss [esp+18h],xmm0 ; result [esp+8]
    {2, 0x189B35D, 1, 0x1C, 0},  // height: movss [esp+14h],xmm1 ; result [esp+1Ch]; any value
    {3, 0x174A8B7, 0, 0x48, 1},  // curve: movss [esp+5Ch],xmm0 ; result [esp+48h]; x >= 0.5
};

static unsigned char *g_text;  // .text of the DLL file mapped at its RVAs

static void load_text(const char *path) {
    FILE *f = fopen(path, "rb");
    CHECK(f);
    static unsigned char d[0x3000000];
    size_t n = fread(d, 1, sizeof d, f);
    fclose(f);
    CHECK(n > 0x1000);
    g_text = calloc(1, 0x2000000);
    unsigned pe = *(unsigned *)(d + 0x3C), nsec = *(unsigned short *)(d + pe + 6), opt = *(unsigned short *)(d + pe + 20);
    for (unsigned i = 0; i < nsec; i++) {
        const unsigned *s = (const unsigned *)(d + pe + 24 + opt + 40 * i + 8);
        if (s[1] <= 0xD79AC8 && 0xD79AC8 < s[1] + s[2]) memcpy(g_text + s[1], d + s[3], s[2]);
    }
}

typedef int(__cdecl *conv_fn)(float);

static conv_fn build(unsigned char *code, const struct spec *sp, int replaced) {
    const struct site *st = &SITES[sp->site];
    unsigned char *p = code;
    memcpy(p, sp->load_xmm1 ? "\xF3\x0F\x10\x4C\x24\x04" : "\xF3\x0F\x10\x44\x24\x04", 6); p += 6;  // movss xmmN,[esp+4]
    memcpy(p, "\x81\xEC\x80\x00\x00\x00", 6); p += 6;                                               // sub esp,80h
    memcpy(p, g_text + sp->prefix_rva, 6); p += 6;                                                  // the game's store
    if (replaced) { memcpy(p, st->repl, st->repl_len); memset(p + st->repl_len, 0x90, st->len - st->repl_len); }
    else memcpy(p, g_text + st->rva, st->len);
    p += st->len;
    *p++ = 0x8B; *p++ = 0x44; *p++ = 0x24; *p++ = sp->slot;                                         // mov eax,[esp+slot]
    memcpy(p, "\x81\xC4\x80\x00\x00\x00\xC3", 7);                                                   // add esp,80h; ret
    return (conv_fn)(void *)code;
}

static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1664525u + 1013904223u; return rng; }

int main(int argc, char **argv) {
    CHECK(argc == 2);
    load_text(argv[1]);
    for (int i = 0; i < NSITES; i++) CHECK(memcmp(g_text + SITES[i].rva, SITES[i].orig, SITES[i].len) == 0);
    CHECK((fpu_cw() & 0xC00) == 0);  // the default rounding mode, as in the game
    unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK(code);

    // edge values
    static float edges[4096];
    int ne = 0;
    float base[] = {0.0f, 1e-45f, 1e-38f, 0.25f, 0.5f, 0.75f, 1.0f, 2.0f, 3.0f, 7.0f, 7.999f, 8.0f, 8.5f, 100.0f,
                    8388607.0f, 8388608.0f, 16777216.0f, 2147483520.0f, 2147483648.0f, 4294967296.0f, 1e30f,
                    INFINITY, NAN};
    for (unsigned k = 0; k < sizeof base / sizeof base[0]; k++) {
        float v = base[k];
        edges[ne++] = v; edges[ne++] = -v;
        edges[ne++] = nextafterf(v, INFINITY); edges[ne++] = nextafterf(v, -INFINITY);
        edges[ne++] = -nextafterf(v, INFINITY); edges[ne++] = -nextafterf(v, -INFINITY);
    }
    for (int k = -300; k <= 300; k++) {  // integers and their neighbours
        edges[ne++] = (float)k; edges[ne++] = nextafterf((float)k, 1e9f); edges[ne++] = nextafterf((float)k, -1e9f);
        edges[ne++] = k + 0.5f;
    }

    for (unsigned s = 0; s < sizeof SPECS / sizeof SPECS[0]; s++) {
        const struct spec *sp = &SPECS[s];
        conv_fn orig = build(code, sp, 0), repl = build(code + 1024, sp, 1);
        long tested = 0;
        for (long k = 0; k < 4000000 + ne; k++) {
            float v;
            if (k < ne) v = edges[k];
            else {
                unsigned bits = rnd();
                if (k & 1) { float u = (float)(rnd() & 0xFFFFFF) / 16777216.0f; v = u * (float)(1u << (rnd() % 31)); }  // ordinary magnitudes
                else memcpy(&v, &bits, 4);                                                                      // any bit pattern
            }
            if (sp->positive_only && !(v > 0.0f)) continue;  // the game never reaches this block otherwise
            if (s == 3 && !(v >= 0.5f && v < 16777216.0f)) continue;  // curve: x = clamp(..)*(n-1) + 0.5
            int a = orig(v), b = repl(v);
            if (a != b) { printf("FAIL %s: input %.9g (0x%08X) game %d (0x%X), ours %d (0x%X)\n", SITES[sp->site].name, v, *(unsigned *)&v, a, a, b, b); return 1; }
            tested++;
        }
        printf("%-15s %ld inputs identical\n", SITES[sp->site].name, tested);
    }

    // the install on a module copy patches all four; a changed rounding mode removes the height patch
    unsigned char *img = VirtualAlloc(NULL, 0x2000000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK(img);
    for (int i = 0; i < NSITES; i++) memcpy(img + SITES[i].rva, SITES[i].orig, SITES[i].len);
    CHECK(install_campfix((HMODULE)img, 0, say) == 4);
    CHECK(memcmp(img + 0xD79AC8, FOG1_REPL, sizeof FOG1_REPL) == 0 && img[0xD79AC8 + 35] == 0x90);
    campfix_frame();
    CHECK(SITES[2].installed);
    unsigned short cw = fpu_cw(), up = cw | 0xC00;
    __asm__ volatile("fldcw %0" ::"m"(up));
    campfix_frame();
    __asm__ volatile("fldcw %0" ::"m"(cw));
    CHECK(!SITES[2].installed && memcmp(img + 0x189B363, HEIGHT_ORIG, 36) == 0);
    printf("all campfix tests passed\n");
    return 0;
}
