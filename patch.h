// The jump a fix writes into the game's code, and the trampoline that still reaches the code it replaced: one copy for
// every fix (each used to write its own; 14 of them, some with the NOP fill, some without, one ignoring a failed
// VirtualProtect). What the bytes at a site must be before it may be patched stays each fix's own check.
#ifndef PATCH_H
#define PATCH_H
#include <windows.h>
#include <string.h>

// jmp rel32 at `at` to `to` (5 bytes). `at` must be writable already.
static inline void patch_rel(unsigned char *at, const void *to) {
    DWORD rel = (DWORD)((const unsigned char *)to - (at + 5));
    at[0] = 0xE9;
    memcpy(at + 1, &rel, 4);
}

// The trampoline: the first `copy` bytes of the code at `site` (whole instructions, none with a relative operand), then a
// jump to the rest. `t`: copy + 5 bytes of writable, executable memory.
static inline void patch_tramp(unsigned char *t, const unsigned char *site, unsigned copy) {
    memcpy(t, site, copy);
    patch_rel(t + copy, site + copy);
    FlushInstructionCache(GetCurrentProcess(), t, copy + 5);
}

// The game's code at `site` now jumps to `to`: the jump, then NOPs up to `len` (the whole instructions it replaces;
// never run, they keep a disassembly readable). 0: the page could not be made writable and nothing was written.
static inline int patch_jump(unsigned char *site, unsigned len, const void *to) {
    DWORD old;
    if (len < 5 || !VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    patch_rel(site, to);
    memset(site + 5, 0x90, len - 5);
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return 1;
}
#endif
