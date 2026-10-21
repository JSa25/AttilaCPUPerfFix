// Small-copy fast path for the game's memcpy/memmove (FUN_11a27ca0, the MSVC CRT routine; cdecl
// (dst, src, n), returns dst). Counted in game (captures/full_verify): ~48,000 calls per frame, 86% of
// them 64 bytes or less, avg 175 B: the routine's size and overlap checks cost more than the copying
// (~4.5% of the main thread on the map and in battle).
// For n <= 64 the copy is done here: every byte of the source is loaded into registers before any byte
// is stored, so overlapping source and destination give the same result as memmove. Larger copies go
// to the game's routine unchanged. Uses EAX/ECX/EDX and XMM0-3 (all caller-saved in the 32-bit ABI) and
// changes flags, as the original does.
#include <windows.h>
#include <string.h>

#define RVA_MEMCPY_FN 0x1A27CA0
static const unsigned char MEMFAST_PROLOGUE[6] = {0x57, 0x56, 0x8B, 0x74, 0x24, 0x10};  // push edi; push esi; mov esi,[esp+10h]
static unsigned char g_memfast_tramp[16];
void *g_memfast_tramp_ptr = g_memfast_tramp;

__attribute__((naked)) void memfast_stub(void) {
    __asm__ volatile(
        "movl 12(%esp), %ecx\n\t"       // n
        "cmpl $64, %ecx\n\t"
        "ja 90f\n\t"
        "movl 4(%esp), %eax\n\t"        // dst (also the return value)
        "movl 8(%esp), %edx\n\t"        // src
        "cmpl $16, %ecx\n\t"
        "jbe 20f\n\t"
        "cmpl $32, %ecx\n\t"
        "jbe 10f\n\t"
        // 33..64: four 16-byte pieces (first, second, second-to-last, last), loaded before storing
        "movups (%edx), %xmm0\n\t"
        "movups 16(%edx), %xmm1\n\t"
        "movups -32(%edx,%ecx), %xmm2\n\t"
        "movups -16(%edx,%ecx), %xmm3\n\t"
        "movups %xmm0, (%eax)\n\t"
        "movups %xmm1, 16(%eax)\n\t"
        "movups %xmm2, -32(%eax,%ecx)\n\t"
        "movups %xmm3, -16(%eax,%ecx)\n\t"
        "ret\n"
        "10:\n\t"                       // 17..32: first and last 16 bytes
        "movups (%edx), %xmm0\n\t"
        "movups -16(%edx,%ecx), %xmm1\n\t"
        "movups %xmm0, (%eax)\n\t"
        "movups %xmm1, -16(%eax,%ecx)\n\t"
        "ret\n"
        "20:\n\t"
        "cmpl $8, %ecx\n\t"
        "jb 30f\n\t"                    // 8..16: first and last 8 bytes
        "movq (%edx), %xmm0\n\t"
        "movq -8(%edx,%ecx), %xmm1\n\t"
        "movq %xmm0, (%eax)\n\t"
        "movq %xmm1, -8(%eax,%ecx)\n\t"
        "ret\n"
        "30:\n\t"
        "cmpl $4, %ecx\n\t"
        "jb 40f\n\t"                    // 4..7: first and last 4 bytes
        "movd (%edx), %xmm0\n\t"
        "movd -4(%edx,%ecx), %xmm1\n\t"
        "movd %xmm0, (%eax)\n\t"
        "movd %xmm1, -4(%eax,%ecx)\n\t"
        "ret\n"
        "40:\n\t"
        "testl %ecx, %ecx\n\t"
        "jz 50f\n\t"                    // 1..3: first byte, middle byte (n/2), last byte
        "pushl %ebx\n\t"
        "pushl %esi\n\t"
        "movzbl (%edx), %ebx\n\t"
        "movl %ecx, %esi\n\t"
        "shrl $1, %esi\n\t"
        "movzbl (%edx,%esi), %esi\n\t"
        "movzbl -1(%edx,%ecx), %edx\n\t"
        "movb %bl, (%eax)\n\t"
        "pushl %ecx\n\t"
        "shrl $1, %ecx\n\t"
        "movl %esi, %ebx\n\t"
        "movb %bl, (%eax,%ecx)\n\t"
        "popl %ecx\n\t"
        "movb %dl, -1(%eax,%ecx)\n\t"
        "popl %esi\n\t"
        "popl %ebx\n"
        "50:\n\t"
        "ret\n"
        "90:\n\t"
        "jmp *_g_memfast_tramp_ptr\n\t");
}

// Returns 1 if installed.
int install_memfast(HMODULE m, void (*log)(const char *fmt, ...)) {
    unsigned char *site = (unsigned char *)m + RVA_MEMCPY_FN;
    if (memcmp(site, MEMFAST_PROLOGUE, 6) != 0) { log("memcpy fast path: code differs from this game version - not installed"); return 0; }
    DWORD old;
    if (!VirtualProtect(g_memfast_tramp, sizeof g_memfast_tramp, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(g_memfast_tramp, site, 6);
    g_memfast_tramp[6] = 0xE9;
    DWORD rel = (DWORD)((site + 6) - (g_memfast_tramp + 11));
    memcpy(g_memfast_tramp + 7, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), g_memfast_tramp, sizeof g_memfast_tramp);
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE9;
    rel = (DWORD)((unsigned char *)memfast_stub - (site + 5));
    memcpy(site + 1, &rel, 4);
    site[5] = 0x90;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    log("memcpy fast path installed (copies of 64 bytes or less)");
    return 1;
}
