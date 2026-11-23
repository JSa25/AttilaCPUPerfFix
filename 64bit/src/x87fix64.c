// x87fix64: the engine converts float->int32 with x87 blocks that switch the FPU rounding mode (fnstcw/fldcw x2)
// around one fistpl; every fldcw serialises the CPU. Each block is replaced in place by one that never touches the
// control word (fisttp + a +-1 fix-up after an x87 compare). Same int32 as the original as long as the thread's
// rounding control is the default (round to nearest), checked on the engine thread. Sites: tools/x87_gen.py.
// shortcut: rounding control checked once at install, not every frame as the x86 x87fix does; add a frame guard if
// anything in the process is found to change it.
#include <intrin.h>
#include <string.h>
#include "perffix64.h"
#include "x87_sites.h"

void install_x87fix(struct x87_result *r) {
    memset(r, 0, sizeof *r);
    r->sites = X87_SITE_COUNT;
    const unsigned char *elf = (const unsigned char *)0x400000;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(elf, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || memcmp(elf, "\x7f" "ELF\x02\x01", 6) || *(const uint16_t *)(elf + 16) != 2) { r->why = 1; return; }
    int cpu[4]; __cpuid(cpu, 1);
    if (!(cpu[2] & 1)) { r->why = 2; return; }
    unsigned short cw; __asm__ volatile("fnstcw %0" : "=m"(cw));
    if (cw & 0xC00) { r->why = 3; return; }
    int match = 0;
    for (int i = 0; i < X87_SITE_COUNT; ++i)
        match += !memcmp((const void *)(uintptr_t)X87_SITES[i].addr, X87_ORIG + X87_SITES[i].off, X87_SITES[i].len);
    if (match < X87_SITE_COUNT - X87_SITE_COUNT / 100) { r->why = 4; r->kept = X87_SITE_COUNT - match; return; }   // other build
    for (int i = 0; i < X87_SITE_COUNT; ++i) {
        const struct x87_site *s = &X87_SITES[i];
        void *at = (void *)(uintptr_t)s->addr;
        if (memcmp(at, X87_ORIG + s->off, s->len) || thread_inside(s->addr, s->len) || !patch_code(at, X87_REP + s->off, s->len)) ++r->kept;
        else ++r->patched;
    }
}
