// Runs the game's own code for each of the four batch-fix sites (copied from empire.retail.dll, from
// the "same material" test to the reset and the AL test after it), unpatched and patched, for every
// combination of state+0x14 and item+0xEC on the two paths that reach the reset without a draw call.
// usage: batchfix_test.exe <path to empire.retail.dll>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../batchfix.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }

static unsigned char *load_text(const char *path, unsigned char *img) {  // maps .text at its RVA
    FILE *f = fopen(path, "rb");
    CHECK(f);
    static unsigned char d[0x3000000];
    size_t n = fread(d, 1, sizeof d, f);
    fclose(f);
    CHECK(n > 0x1000);
    unsigned pe = *(unsigned *)(d + 0x3C), nsec = *(unsigned short *)(d + pe + 6), opt = *(unsigned short *)(d + pe + 20);
    for (unsigned i = 0; i < nsec; i++) {
        const unsigned *s = (const unsigned *)(d + pe + 24 + opt + 40 * i + 8);  // vsize, va, rawsize, rawptr
        if (s[1] <= SITES[0].cmp_rva && SITES[0].cmp_rva < s[1] + s[2]) memcpy(img + s[1], d + s[3], s[2]);
    }
    return img;
}

struct regs { unsigned eax, ebp, esi, ecx, ebx; unsigned al_out, edx_out; };

// call_site(regs, code): loads EAX/EBP/ESI/ECX/EBX (EDX = ECX, as at the site), calls the code on a
// scratch frame (the game stores AL at [esp+0Ch] or [esp+24h]), stores AL and EDX back.
struct regs *g_r;
void call_site(struct regs *r, unsigned char *code);
__asm__(
    ".globl _call_site\n"
    "_call_site:\n\t"
    "pushl %ebp\n\tpushl %ebx\n\tpushl %esi\n\tpushl %edi\n\t"
    "movl 20(%esp), %eax\n\t"
    "movl %eax, _g_r\n\t"
    "movl 24(%esp), %edi\n\t"
    "subl $0x60, %esp\n\t"
    "movl 4(%eax), %ebp\n\t"
    "movl 8(%eax), %esi\n\t"
    "movl 12(%eax), %ecx\n\t"
    "movl 16(%eax), %ebx\n\t"
    "movl 0(%eax), %eax\n\t"
    "movl %ecx, %edx\n\t"
    "call *%edi\n\t"
    "addl $0x60, %esp\n\t"
    "movl _g_r, %ecx\n\t"
    "movzbl %al, %eax\n\t"
    "movl %eax, 20(%ecx)\n\t"
    "movl %edx, 24(%ecx)\n\t"
    "popl %edi\n\tpopl %esi\n\tpopl %ebx\n\tpopl %ebp\n\t"
    "ret\n");

static void run_site(unsigned char *c, struct regs *r) { call_site(r, c); }

int main(int argc, char **argv) {
    CHECK(argc == 2);
    unsigned char *orig = VirtualAlloc(NULL, 0x2000000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char *img = VirtualAlloc(NULL, 0x2000000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK(orig && img);
    load_text(argv[1], orig);
    load_text(argv[1], img);
    for (int i = 0; i < 4; i++) {  // end each copy right after the AL test with a ret
        orig[SITES[i].reset_rva + 9] = 0xC3;
        img[SITES[i].reset_rva + 9] = 0xC3;
    }
    CHECK(install_batchfix((HMODULE)img, say) == 4);
    CHECK(img[SITES[0].cmp_rva] == 0xE9 && img[SITES[0].reset_rva] == 0xE9);

    unsigned ecs[] = {0, 5, 7};
    int changed = 0, cases = 0;
    for (int i = 0; i < 4; i++)
        for (int cur = 0; cur < 3; cur++)
            for (int it = 0; it < 3; it++)
                for (int count_case = 0; count_case < 2; count_case++) {
                    // count 0 with count != limit: same path goes straight to the reset; diff path skips
                    // the draw because the count is 0. (Paths that draw are not run here.)
                    static unsigned char state[0x100], item[0x100];
                    struct regs a = {0}, b;
                    unsigned count = count_case ? 0 : 0;
                    unsigned limit = 99;
                    memset(item, 0, sizeof item);
                    *(unsigned *)(item + SITES[i].field) = ecs[it];
                    if (SITES[i].item_modrm == 0x95) a.ebp = (unsigned)(size_t)item; else a.eax = (unsigned)(size_t)item;
                    a.esi = (unsigned)(size_t)state;
                    a.ecx = count;
                    a.ebx = 0x1234;
                    b = a;
                    unsigned char st_a[0x100], st_b[0x100];
                    memset(state, 0, sizeof state); *(unsigned *)(state + 0x14) = ecs[cur]; *(unsigned *)(state + 0x64) = limit;
                    run_site(orig + SITES[i].cmp_rva, &a);
                    memcpy(st_a, state, sizeof state);
                    memset(state, 0, sizeof state); *(unsigned *)(state + 0x14) = ecs[cur]; *(unsigned *)(state + 0x64) = limit;
                    run_site(img + SITES[i].cmp_rva, &b);
                    memcpy(st_b, state, sizeof state);
                    cases++;
                    int same_nonzero = ecs[cur] != 0 && ecs[cur] == ecs[it];
                    if (!same_nonzero) {  // everything else: exactly the original
                        CHECK(a.al_out == b.al_out && a.edx_out == b.edx_out && !memcmp(st_a, st_b, sizeof st_a));
                    } else {              // the new case: same path (AL 0), state+0x14 kept
                        changed++;
                        CHECK(a.al_out == 1 && b.al_out == 0);
                        CHECK(*(unsigned *)(st_a + 0x14) == 0 && *(unsigned *)(st_b + 0x14) == ecs[cur]);
                        CHECK(b.edx_out == b.ecx);
                    }
                    CHECK(*(unsigned *)(st_b + 0x70) == 0x1234);  // the game's store before the reset still runs
                }
    printf("%d cases: %d identical to the game's code, %d now batch (equal non-zero +0xEC)\n", cases, cases - changed, changed);
    printf("all batch fix tests passed\n");
    return 0;
}
