// Prefetch ahead in the battle render pass loops that walk an array of item pointers and stall on the first read
// of a field of each item (AGENT_LOG 47-49). The array is known in advance, so while handling item i the stub asks
// the CPU to start loading the field of item i+4 that the loop is about to read. It only prefetches through item
// pointers, never loads: a loop can stop early, so a slot it never reaches may hold a stale pointer. The array itself
// is only read inside the loop's range (i+4 < end). The loop's own instructions then run unchanged.
// Each site is a loop top that is the target of exactly one jb back-edge, with no jump into the patched bytes.
#include <windows.h>
#include <string.h>

volatile unsigned char g_loopfetch_on = 1;  // dev A/B flips it; the original instructions run either way

static void put32(unsigned char *p, DWORD v) { memcpy(p, &v, 4); }

struct site {
    DWORD rva;
    unsigned char len;
    unsigned char orig[9];
    unsigned char fetch[40];  // complete, including its own pushes and pops
    unsigned char fetch_len;
    unsigned char on;         // installed (off: measured no gain or worse, kept for the test)
    const char *name;
};

// Layout "list in eax, index in ebp, end at [esp+30h]" (FUN_117b1520, FUN_117b0f80): eax must stay the list,
// because the original's first instruction reads through it, so edx and ecx are the scratch registers.
#define FETCH_EAX_LIST_EBP_I(off)                                                                          \
    {0x52, 0x51,                   /* push edx; push ecx */                                              \
     0x8B, 0x50, 0x08,             /* mov edx,[eax+8]        array */                                    \
     0x8D, 0x4D, 0x04,             /* lea ecx,[ebp+4] */                                                 \
     0x3B, 0x4C, 0x24, 0x38,       /* cmp ecx,[esp+38h]      end (+8 for the pushes) */                   \
     0x73, 0x07,                   /* jae skip */                                                        \
     0x8B, 0x14, 0x8A,             /* mov edx,[edx+ecx*4]    item i+4 */                                 \
     0x0F, 0x18, 0x4A, off,        /* prefetcht0 [edx+off]   the field the loop reads first */           \
     0x59, 0x5A}                   /* skip: pop ecx; pop edx */
// Layout "frame in ebp ([ebp+0Ch] list, [ebp+18h] end), index in ecx" (FUN_117aff90, FUN_117afc10, FUN_117af720):
// eax is overwritten by the original next, so it holds the array here.
#define FETCH_EBP_FRAME_ECX_I_HEAD                                                                         \
    0x52,                          /* push edx */                                                        \
    0x8B, 0x45, 0x0C,              /* mov eax,[ebp+0Ch]      list */                                     \
    0x8B, 0x40, 0x08,              /* mov eax,[eax+8]        array */                                    \
    0x8D, 0x51, 0x04,              /* lea edx,[ecx+4] */                                                 \
    0x3B, 0x55, 0x18               /* cmp edx,[ebp+18h]      end */

static const struct site SITES[8] = {
    {0x17B1840, 9, {0x8B, 0x45, 0x08, 0x8B, 0x2C, 0x98, 0x8B, 0x7D, 0x00},
     {0x52, 0x8B, 0x45, 0x08, 0x8D, 0x53, 0x04, 0x3B, 0x54, 0x24, 0x2C, 0x73, 0x06, 0x8B, 0x14, 0x90, 0x0F, 0x18, 0x0A, 0x5A},
     20, 0, "render pass FUN_117b17f0"},  // measured slower (AGENT_LOG 47)
    {0x17B0450, 6, {0x8B, 0x47, 0x08, 0x8B, 0x7E, 0x70},
     {0x52,                        // push edx
      0x8B, 0x47, 0x08,            // mov eax,[edi+8]        array (edi = list; the original sets eax the same way)
      0x8D, 0x53, 0x04,            // lea edx,[ebx+4]
      0x3B, 0x55, 0x18,            // cmp edx,[ebp+18h]      end
      0x73, 0x06,                  // jae skip
      0x8B, 0x14, 0x90,            // mov edx,[eax+edx*4]    item i+4
      0x0F, 0x18, 0x0A,            // prefetcht0 [edx]       the loop reads [item] first
      0x5A},                       // skip: pop edx
     19, 1, "render pass FUN_117b0400"},
    {0x182E0DA, 9, {0x8B, 0x40, 0x08, 0x8B, 0x0C, 0x88, 0x8B, 0x71, 0x60},
     {0x52, 0x8B, 0x70, 0x08, 0x8D, 0x51, 0x04, 0x3B, 0x54, 0x24, 0x5C, 0x73, 0x0A, 0x8B, 0x14, 0x96, 0x0F, 0x18, 0x0A, 0x0F, 0x18, 0x4A, 0x60, 0x5A},
     24, 0, "animation batching FUN_1182e080"},  // no gain once prefetch-only (AGENT_LOG 48)
    {0x17B1560, 6, {0x8B, 0x40, 0x08, 0x8B, 0x7E, 0x70}, FETCH_EAX_LIST_EBP_I(0x3C), 23, 1, "render pass FUN_117b1520"},
    {0x17B0FC0, 6, {0x8B, 0x40, 0x08, 0x8B, 0x7E, 0x70}, FETCH_EAX_LIST_EBP_I(0x3C), 23, 1, "render pass FUN_117b0f80"},
    {0x17B0065, 6, {0x8B, 0x45, 0x0C, 0x8B, 0x40, 0x08},
     {FETCH_EBP_FRAME_ECX_I_HEAD, 0x73, 0x07, 0x8B, 0x14, 0x90, 0x0F, 0x18, 0x4A, 0x3C, 0x5A},
     23, 1, "render pass FUN_117aff90"},
    {0x17AFCE2, 6, {0x8B, 0x45, 0x0C, 0x8B, 0x40, 0x08},
     {FETCH_EBP_FRAME_ECX_I_HEAD, 0x73, 0x07, 0x8B, 0x14, 0x90, 0x0F, 0x18, 0x4A, 0x40, 0x5A},
     23, 1, "render pass FUN_117afc10"},
    {0x17AF7F5, 6, {0x8B, 0x45, 0x0C, 0x8B, 0x40, 0x08},
     {FETCH_EBP_FRAME_ECX_I_HEAD, 0x73, 0x0E, 0x8B, 0x14, 0x90, 0x0F, 0x18, 0x4A, 0x3C,
      0x0F, 0x18, 0x8A, 0xC0, 0x00, 0x00, 0x00, 0x5A},  // also prefetcht0 [edx+0C0h], read later in the loop
     30, 1, "render pass FUN_117af720"},
};
#define NSITES (sizeof SITES / sizeof *SITES)

// [cmp byte [flag],0; je orig] <fetch>; orig: <original bytes>; jmp site+len
static unsigned build(unsigned char *s, const struct site *t, unsigned char *site) {
    unsigned n = 0;
    s[n++] = 0x80; s[n++] = 0x3D; put32(s + n, (DWORD)(UINT_PTR)&g_loopfetch_on); n += 4; s[n++] = 0x00;
    s[n++] = 0x74; s[n++] = t->fetch_len;
    memcpy(s + n, t->fetch, t->fetch_len); n += t->fetch_len;
    memcpy(s + n, t->orig, t->len); n += t->len;
    s[n++] = 0xE9; put32(s + n, (DWORD)((site + t->len) - (s + n + 4))); n += 4;
    return n;
}

static int patch(unsigned char *site, unsigned len, unsigned char *to) {
    DWORD old;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE9;
    put32(site + 1, (DWORD)(to - (site + 5)));
    for (unsigned i = 5; i < len; i++) site[i] = 0x90;
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return 1;
}

int install_loopfetch(HMODULE m, void (*log)(const char *fmt, ...)) {
    unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return 0;
    int n = 0, want = 0;
    for (unsigned i = 0; i < NSITES; i++) {
        const struct site *t = &SITES[i];
        if (!t->on) continue;
        want++;
        unsigned char *site = (unsigned char *)m + t->rva, *s = code + 128 * i;
        if (memcmp(site, t->orig, t->len) != 0) {
            log("loop prefetch: %s differs from this game version - skipped", t->name);
            continue;
        }
        build(s, t, site);
        FlushInstructionCache(GetCurrentProcess(), s, 128);
        n += patch(site, t->len, s);
    }
    log("loop prefetch: %d of %d render loops prefetch ahead", n, want);
    return n;
}
