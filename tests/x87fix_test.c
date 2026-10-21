// Differential test for x87fix.c over EVERY matched site in empire.retail.dll: the original block bytes
// and the replacement run in the same wrapper on edge values (both signs: 0, denormals, integers and
// their neighbours, .5 cases, 2^23/2^24/2^31/2^32 boundaries, huge, inf, NaN) and random inputs. Compared:
// the result slot, EAX, ECX and all 128 bits of XMM0. Then the revert guard via the thread watcher.
// usage: x87fix_test.exe <path to empire.retail.dll>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include "../x87fix.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }

static __attribute__((aligned(16))) unsigned g_pattern[4] = {0x3F800000, 0xDEADBEEF, 0x01234567, 0x89ABCDEF};
static __attribute__((aligned(16))) unsigned g_xmm_out[4];
static unsigned g_eax_out, g_ecx_out;

static unsigned char *put(unsigned char *p, const char *b, int n) { memcpy(p, b, n); return p + n; }
static unsigned char *put32(unsigned char *p, unsigned v) { memcpy(p, &v, 4); return p + 4; }

typedef unsigned(__cdecl *run_fn)(unsigned);
static run_fn build(unsigned char *code, const struct x87site *s, const unsigned char *block) {
    unsigned char *p = code;
    p = put(p, "\x8B\x44\x24\x04", 4);                         // mov eax,[esp+4] (input bits)
    p = put(p, "\x81\xEC\x00\x01\x00\x00", 6);                 // sub esp,100h
    p = put(p, "\x89\x44\x24", 3); *p++ = s->a - 4;            // mov [esp+A-4],eax (the block pushes first)
    p = put(p, "\x0F\x28\x05", 3); p = put32(p, (unsigned)(size_t)g_pattern);  // movaps xmm0,[pattern]
    *p++ = 0xB8; p = put32(p, 0x12345678);                     // mov eax,marker
    *p++ = 0xB9; p = put32(p, 0x87654321);                     // mov ecx,marker
    p = put(p, (const char *)block, s->len);
    p = put(p, "\x0F\x29\x05", 3); p = put32(p, (unsigned)(size_t)g_xmm_out);  // movaps [out],xmm0
    *p++ = 0xA3; p = put32(p, (unsigned)(size_t)&g_eax_out);   // mov [eax_out],eax
    p = put(p, "\x89\x0D", 2); p = put32(p, (unsigned)(size_t)&g_ecx_out);     // mov [ecx_out],ecx
    p = put(p, "\x8B\x44\x24", 3); *p++ = s->r - 4;            // mov eax,[esp+R-4]
    p = put(p, "\x81\xC4\x00\x01\x00\x00\xC3", 7);            // add esp,100h; ret
    return (run_fn)(void *)code;
}

static volatile int g_stop_bad;
static DWORD WINAPI bad_thread(LPVOID p) {
    (void)p;
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    cw |= 0x400;
    __asm__ volatile("fldcw %0" ::"m"(cw));
    while (!g_stop_bad) Sleep(1);
    return 0;
}

static unsigned rng = 777;
static unsigned rnd(void) { rng = rng * 1664525u + 1013904223u; return rng; }

int main(int argc, char **argv) {
    CHECK(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    CHECK(f);
    static unsigned char d[0x3000000];
    size_t n = fread(d, 1, sizeof d, f);
    fclose(f);
    unsigned pe = *(unsigned *)(d + 0x3C), nsec = *(unsigned short *)(d + pe + 6), opt = *(unsigned short *)(d + pe + 20);
    const unsigned char *text = NULL;
    unsigned tsize = 0, tva = 0;
    for (unsigned i = 0; i < nsec; i++) {
        const unsigned char *h = d + pe + 24 + opt + 40 * i;
        if (!memcmp(h, ".text", 6)) { tva = *(unsigned *)(h + 12); tsize = *(unsigned *)(h + 16); text = d + *(unsigned *)(h + 20); }
    }
    CHECK(text && n > 0x1000 && tsize);
    CHECK((x87_cw() & 0xC00) == 0 && !(x87_mxcsr() & 0x40) && cpu_has_sse41());
    int found = x87_scan(text, tsize, tva);
    int cnt[2][4] = {{0}};
    for (int i = 0; i < found; i++) cnt[g_x87[i].frndint][g_x87[i].mode]++;
    printf("%d blocks: fistp down %d up %d trunc %d | frndint down %d up %d trunc %d\n", found, cnt[0][1], cnt[0][2], cnt[0][3],
           cnt[1][1], cnt[1][2], cnt[1][3]);
    CHECK(found > 1000);

    static float edges[8192];
    int ne = 0;
    float base[] = {0.0f, 1e-45f, 1e-40f, 1.1754942e-38f, 1e-10f, 0.25f, 0.4999999f, 0.5f, 0.5000001f, 0.75f, 1.0f, 1.5f, 2.5f,
                    7.999f, 8.0f, 100.5f, 8388607.0f, 8388607.5f, 8388608.0f, 16777215.0f, 16777216.0f, 2147483520.0f,
                    2147483648.0f, 2147483904.0f, 4294967296.0f, 1e20f, 3.4028235e38f, INFINITY, NAN};
    for (unsigned k = 0; k < sizeof base / sizeof base[0]; k++)
        for (int sgn = 0; sgn < 2; sgn++) {
            float v = sgn ? -base[k] : base[k];
            edges[ne++] = v; edges[ne++] = nextafterf(v, INFINITY); edges[ne++] = nextafterf(v, -INFINITY);
        }
    for (int k = -70; k <= 70; k++) { edges[ne++] = (float)k; edges[ne++] = k + 0.5f; edges[ne++] = nextafterf((float)k, 1e9f); edges[ne++] = nextafterf((float)k, -1e9f); }
    unsigned snan = 0x7F800001, qneg = 0xFFC12345;
    memcpy(&edges[ne++], &snan, 4); memcpy(&edges[ne++], &qneg, 4);

    unsigned char *code = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK(code);
    long runs = 0;
    for (int i = 0; i < found; i++) {
        struct x87site *s = &g_x87[i];
        unsigned char repl[38];
        x87_replacement(s, repl);
        run_fn o = build(code, s, s->orig), r = build(code + 4096, s, repl);
        for (int k = 0; k < ne + 2000; k++) {
            unsigned in;
            if (k < ne) memcpy(&in, &edges[k], 4);
            else if (k & 1) in = rnd();
            else { float v = ((float)(rnd() & 0xFFFFFF) / 16777216.0f - 0.5f) * (float)(1u << (rnd() % 32)); memcpy(&in, &v, 4); }
            unsigned ro = o(in), eo = g_eax_out, co = g_ecx_out, xo[4];
            memcpy(xo, g_xmm_out, 16);
            unsigned rr = r(in);
            if (ro != rr || eo != g_eax_out || co != g_ecx_out || memcmp(xo, g_xmm_out, 16)) {
                float fv; memcpy(&fv, &in, 4);
                printf("FAIL site +0x%X (mode %d frndint %d): input 0x%08X (%g): game %08X eax %08X ecx %08X | ours %08X eax %08X ecx %08X%s\n",
                       s->rva, s->mode, s->frndint, in, fv, ro, eo, co, rr, g_eax_out, g_ecx_out, memcmp(xo, g_xmm_out, 16) ? " XMM0 differs" : "");
                return 1;
            }
            runs++;
        }
    }
    printf("%d sites x %d inputs = %ld runs: results, EAX, ECX and XMM0 identical\n", found, ne + 2000, runs);

    // install on a module-like copy, then the guard restores the rounding sites when the rounding control changes
    unsigned char *img = VirtualAlloc(NULL, 0x2000000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK(img);
    memcpy(img, d, 0x1000);  // headers
    memcpy(img + tva, text, tsize);
    int patched = install_x87fix((HMODULE)img, say);
    CHECK(patched == found);
    x87fix_frame();
    CHECK(!g_x87_reverted);
    // another thread switches its rounding control: the watcher (every 10 s) flags it, the next frame restores
    HANDLE bad = CreateThread(NULL, 0, bad_thread, NULL, 0, NULL);
    CHECK(bad);
    Sleep(11000);
    x87fix_frame();
    CHECK(!g_x87_reverted || g_x87_other_thread_bad);
    CHECK(g_x87_reverted);
    g_stop_bad = 1;
    for (int i = 0; i < found; i++) {
        struct x87site *s = &g_x87[i];
        if (s->mode == 3 && !s->frndint) CHECK(s->patched && img[s->rva + 28] == 0x4C);
        else CHECK(!s->patched && !memcmp(img + s->rva, s->orig, s->len));
    }
    printf("all x87fix tests passed\n");
    return 0;
}
