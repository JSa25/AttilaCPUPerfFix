// Test for loopfetch.c: every stub runs with the register and stack state its loop has, for every index of arrays
// of 1..40 items placed at the very end of a page followed by a no-access page, with every slot past the current
// index holding a pointer into that no-access page (a stale pointer: the stub may only prefetch through it), with
// prefetching on and off. Afterwards the registers must equal what the original instructions produce and edx must
// be unchanged. Then install_loopfetch must accept the real game code (and refuse a second time).
// usage: loopfetch_test.exe <path to empire.retail.dll>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../loopfetch.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d (site %u, count %u, i %u): %s\n", __LINE__, k, count, i, #c); exit(1); } } while (0)
#define CHECK0(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

unsigned g_r[8];  // eax ecx edx ebx esp ebp esi edi as seen by the pad
__attribute__((naked)) void pad(void) {
    __asm__ volatile("pushal\n\t"
                     "movl 28(%esp), %eax\n\tmovl %eax, _g_r\n\t"
                     "movl 24(%esp), %eax\n\tmovl %eax, _g_r+4\n\t"
                     "movl 20(%esp), %eax\n\tmovl %eax, _g_r+8\n\t"
                     "movl 16(%esp), %eax\n\tmovl %eax, _g_r+12\n\t"
                     "movl 8(%esp), %eax\n\tmovl %eax, _g_r+20\n\t"
                     "movl 4(%esp), %eax\n\tmovl %eax, _g_r+24\n\t"
                     "movl 0(%esp), %eax\n\tmovl %eax, _g_r+28\n\t"
                     "popal\n\tret\n\t");
}
enum { EAX, ECX, EDX, EBX, ESP_, EBP, ESI, EDI };
// The emitter site's original instructions push edi and eax: this pad records those two values and drops them.
unsigned g_pushed[2];
__attribute__((naked)) void pad_push2(void) {
    __asm__ volatile("pushal\n\t"
                     "movl 28(%esp), %eax\n\tmovl %eax, _g_r\n\t"
                     "movl 24(%esp), %eax\n\tmovl %eax, _g_r+4\n\t"
                     "movl 20(%esp), %eax\n\tmovl %eax, _g_r+8\n\t"
                     "movl 16(%esp), %eax\n\tmovl %eax, _g_r+12\n\t"
                     "movl 8(%esp), %eax\n\tmovl %eax, _g_r+20\n\t"
                     "movl 4(%esp), %eax\n\tmovl %eax, _g_r+24\n\t"
                     "movl 0(%esp), %eax\n\tmovl %eax, _g_r+28\n\t"
                     "movl 32(%esp), %eax\n\tmovl %eax, _g_pushed\n\t"
                     "movl 36(%esp), %eax\n\tmovl %eax, _g_pushed+4\n\t"
                     "popal\n\taddl $8, %esp\n\tret\n\t");
}

static unsigned char *g_stub[NSITES];
static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }

unsigned g_in[7], g_fill;
unsigned char *g_cur_stub;
// Calls a stub with eax, ecx, ebx, ebp, esi, edi set; `fill` dwords sit between the return address and `end`,
// so that [esp + 4*(fill+1)] at stub entry is the loop's end value.
static void run(unsigned k, unsigned eax, unsigned ecx, unsigned ebx, unsigned ebp, unsigned esi, unsigned edi,
                unsigned end, int fill) {
    g_in[0] = eax; g_in[1] = ecx; g_in[2] = ebx; g_in[3] = ebp; g_in[4] = esi; g_in[5] = edi; g_in[6] = end;
    g_fill = (unsigned)fill;
    g_cur_stub = g_stub[k];
    __asm__ volatile(
        "pushal\n\t"
        "movl _g_fill, %%ecx\n\t"
        "pushl _g_in+24\n\t"
        "1: testl %%ecx, %%ecx\n\tjz 2f\n\tpushl $0x11111111\n\tdecl %%ecx\n\tjmp 1b\n\t"
        "2: movl _g_in, %%eax\n\tmovl _g_in+4, %%ecx\n\tmovl _g_in+8, %%ebx\n\tmovl _g_in+12, %%ebp\n\t"
        "movl _g_in+16, %%esi\n\tmovl _g_in+20, %%edi\n\tmovl $0x5EED5EED, %%edx\n\t"
        "calll *_g_cur_stub\n\t"
        "movl _g_fill, %%ecx\n\t"
        "leal 4(%%esp,%%ecx,4), %%esp\n\t"
        "popal\n\t"
        ::: "memory", "cc");
}

int main(int argc, char **argv) {
    CHECK0(argc == 2);
    unsigned char *page = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK0(page);
    for (unsigned k = 0; k < NSITES; k++) {  // fake site k: the stub jumps back to site+len, where a jmp to the pad sits
        unsigned char *site = page + 256 * k;
        site[SITES[k].len] = 0xE9;
        void *to = SITES[k].rva == 0x18933C0 ? (void *)pad_push2 : (void *)pad;
        put32(site + SITES[k].len + 1, (DWORD)((unsigned char *)to - (site + SITES[k].len + 5)));
        g_stub[k] = page + 4096 + 256 * k;
        build(g_stub[k], &SITES[k], site);
    }
    FlushInstructionCache(GetCurrentProcess(), page, 8192);

    static unsigned items[128][64];  // item[j][0] = 1000+j; item[j]+0x60 -> its own row (for site 2)
    for (int j = 0; j < 128; j++) { items[j][0] = 1000 + j; items[j][0x60 / 4] = (unsigned)(UINT_PTR)items[j]; }
    unsigned char *arr_pages = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old;
    CHECK0(VirtualProtect(arr_pages + 4096, 4096, PAGE_NOACCESS, &old));
    unsigned stale = (unsigned)(UINT_PTR)(arr_pages + 4096 + 64);
    static unsigned obj[64];  // the esi object: [esi+0x70]
    obj[0x70 / 4] = 0xABCD1234;
    static unsigned list[4], frame[16], emitters[8];  // emitters+8 = entry array {emitter, ref} (emitter site)
    long runs = 0;
    for (int on = 0; on < 2; on++) {
        g_loopfetch_on = (unsigned char)on;
        for (unsigned count = 1; count <= 40; count++) {
            unsigned *arr = (unsigned *)(arr_pages + 4096 - 4 * count);  // ends exactly at the no-access page
            list[2] = (unsigned)(UINT_PTR)arr;                            // list+8 = array
            for (unsigned i = 0; i < count; i++) {
                for (unsigned j = 0; j < count; j++) arr[j] = j <= i ? (unsigned)(UINT_PTR)items[(j * 7) % 128] : stale;
                unsigned *it = (unsigned *)(UINT_PTR)arr[i];
                unsigned A = (unsigned)(UINT_PTR)arr, L = (unsigned)(UINT_PTR)list, F = (unsigned)(UINT_PTR)frame;
                unsigned O = (unsigned)(UINT_PTR)obj;
                frame[0x0C / 4] = L;
                frame[0x18 / 4] = count;
                for (unsigned k = 0; k < NSITES; k++) {
                    if (SITES[k].rva == 0x18933C0) {  // ebx = struct ([+8] entries), esi = i, count [esp+34h]:
                        // entries of 8 bytes ending at the no-access page (they share it with arr: set here, arr put
                        // back after); entries past i hold a stale emitter pointer
                        unsigned *ent = (unsigned *)(arr_pages + 4096 - 8 * count);
                        for (unsigned j = 0; j < count; j++) {
                            ent[2 * j] = j <= i ? (unsigned)(UINT_PTR)items[(j * 5) % 128] : stale;
                            ent[2 * j + 1] = 0x7EF00000 + j;
                        }
                        emitters[2] = (unsigned)(UINT_PTR)ent;
                        unsigned E = (unsigned)(UINT_PTR)emitters;
                        run(k, 0xA5A5, 0xCCCC, E, 0xBEEF, i, 0xD1D1, count, 12);  // ecx = entries, edi and eax pushed
                        CHECK(g_r[ECX] == (unsigned)(UINT_PTR)ent && g_r[EBX] == E && g_r[ESI] == i && g_r[EAX] == 0xA5A5 &&
                              g_r[EDI] == 0xD1D1 && g_r[EBP] == 0xBEEF && g_pushed[0] == 0xA5A5 && g_pushed[1] == 0xD1D1);
                        for (unsigned j = 0; j < count; j++) arr[j] = j <= i ? (unsigned)(UINT_PTR)items[(j * 7) % 128] : stale;
                        CHECK(g_r[EDX] == 0x5EED5EED);
                        runs++;
                        continue;
                    }
                    switch (k) {
                    case 0:  // ebp = list, ebx = i, end [esp+28h]: eax = array, ebp = item, edi = [item]
                        run(k, 0xAAAA, 0xCCCC, i, L, 0xE51E51, 0xD1D1, count, 9);
                        CHECK(g_r[EAX] == A && g_r[EBP] == arr[i] && g_r[EDI] == it[0] && g_r[EBX] == i && g_r[ECX] == 0xCCCC);
                        break;
                    case 1:  // edi = list, ebx = i, esi = obj, ebp = frame: eax = array, edi = [esi+70h]
                        run(k, 0xAAAA, 0xCCCC, i, F, O, L, 0, 0);
                        CHECK(g_r[EAX] == A && g_r[EDI] == 0xABCD1234 && g_r[EBX] == i && g_r[EBP] == F && g_r[ECX] == 0xCCCC);
                        break;
                    case 2:  // eax = list, ecx = i, end [esp+58h]: eax = array, ecx = item, esi = [item+60h]
                        run(k, L, i, 0xBBBB, 0xBEEF, 0xE51E51, 0xD1D1, count, 21);
                        CHECK(g_r[EAX] == A && g_r[ECX] == arr[i] && g_r[ESI] == it[0x60 / 4] && g_r[EBX] == 0xBBBB && g_r[EDI] == 0xD1D1);
                        break;
                    case 3: case 4:  // eax = list, ebp = i, esi = obj, end [esp+30h]: eax = array, edi = [esi+70h]
                        run(k, L, 0xCCCC, 0xBBBB, i, O, 0xD1D1, count, 11);
                        CHECK(g_r[EAX] == A && g_r[EDI] == 0xABCD1234 && g_r[EBP] == i && g_r[ECX] == 0xCCCC && g_r[EBX] == 0xBBBB);
                        break;
                    default:  // ebp = frame ([+0Ch] list, [+18h] end), ecx = i: eax = array
                        run(k, 0xAAAA, i, 0xBBBB, F, 0xE51E51, 0xD1D1, 0, 0);
                        CHECK(g_r[EAX] == A && g_r[ECX] == i && g_r[EBP] == F && g_r[EBX] == 0xBBBB && g_r[ESI] == 0xE51E51 && g_r[EDI] == 0xD1D1);
                    }
                    CHECK(g_r[EDX] == 0x5EED5EED);
                    runs++;
                }
            }
        }
    }
    printf("%ld stub runs over %u sites (arrays of 1..40 items ending at a no-access page, unreached slots pointing into it, prefetch on/off): registers match the original instructions\n",
           runs, (unsigned)NSITES);

    HMODULE m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK0(m);
    CHECK0(install_loopfetch(m, say) == 6);
    CHECK0(install_loopfetch(m, say) == 0);
    printf("all loop prefetch tests passed\n");
    return 0;
}
