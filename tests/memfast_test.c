// Tests memfast.c: the hooked stand-in (the game routine's prologue, then the C library memmove) against
// a reference memmove, for every size 0..200, source and destination alignments 0..15 and every
// overlap distance -80..80; return value = dst, bytes around the destination untouched, callee-saved
// registers (EBX, ESI, EDI, EBP) preserved.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../memfast.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }

static void *g_memmove = (void *)memmove;
typedef void *(__cdecl *copy_fn)(void *, const void *, unsigned);
copy_fn g_copy;
unsigned g_saved[4], g_after[4];

// call_checked(d, s, n): calls g_copy with EBX/ESI/EDI/EBP set to markers and stores them afterwards.
void *call_checked(void *d, const void *s, unsigned n);
__asm__(
    ".globl _call_checked\n"
    "_call_checked:\n\t"
    "pushl %ebx\n\tpushl %esi\n\tpushl %edi\n\tpushl %ebp\n\t"
    "movl 28(%esp), %eax\n\tpushl %eax\n\t"   // n   (args at +20/+24/+28 after the 4 pushes)
    "movl 28(%esp), %eax\n\tpushl %eax\n\t"   // s
    "movl 28(%esp), %eax\n\tpushl %eax\n\t"   // d
    "movl $0x11111111, %ebx\n\tmovl $0x22222222, %esi\n\tmovl $0x33333333, %edi\n\tmovl $0x44444444, %ebp\n\t"
    "call *_g_copy\n\t"
    "addl $12, %esp\n\t"
    "movl %ebx, _g_after\n\tmovl %esi, _g_after+4\n\tmovl %edi, _g_after+8\n\tmovl %ebp, _g_after+12\n\t"
    "popl %ebp\n\tpopl %edi\n\tpopl %esi\n\tpopl %ebx\n\t"
    "ret\n");

int main(void) {
    unsigned char *img = VirtualAlloc(NULL, 0x1B00000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK(img);
    // stand-in: push edi; push esi; mov esi,[esp+10h]; pop esi; pop edi; jmp [g_memmove]
    unsigned char *f = img + RVA_MEMCPY_FN;
    unsigned char code[] = {0x57, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x5E, 0x5F, 0xFF, 0x25, 0, 0, 0, 0};
    void **pp = &g_memmove;
    memcpy(code + 10, &pp, 4);
    memcpy(f, code, sizeof code);
    CHECK(install_memfast((HMODULE)img, say) == 1);
    g_copy = (copy_fn)(void *)f;

    static unsigned char buf[512], ref[512];
    long cases = 0;
    for (unsigned n = 0; n <= 200; n++)
        for (int sa = 0; sa < 16; sa++)
            for (int delta = -80; delta <= 80; delta++) {
                int so = 160 + sa, dofs = so + delta;
                for (int i = 0; i < 512; i++) buf[i] = (unsigned char)(i * 7 + n + sa);
                memcpy(ref, buf, 512);
                memmove(ref + dofs, ref + so, n);
                void *r = call_checked(buf + dofs, buf + so, n);
                CHECK(r == buf + dofs);
                CHECK(memcmp(buf, ref, 512) == 0);
                CHECK(g_after[0] == 0x11111111 && g_after[1] == 0x22222222 && g_after[2] == 0x33333333 && g_after[3] == 0x44444444);
                cases++;
            }
    printf("%ld copies (sizes 0..200, 16 alignments, overlaps -80..80) identical to memmove\n", cases);
    (void)g_saved;
    printf("all memfast tests passed\n");
    return 0;
}
