// Every compiler-generated x87 float->int / float->integral-float block that switches the FPU rounding
// mode for one instruction, replaced in place without touching the control word. The block (36 or 38
// bytes, found by exact pattern in empire.retail.dll's .text):
//   push eax ; fld dword [esp+A] ; fstcw [esp+C] ; mov eax,[esp+C] ; or eax,MODE ; mov [esp+T],eax ;
//   fldcw [esp+T] ; (fistp dword [esp+R] | frndint ; fstp dword [esp+R]) ; fldcw [esp+C] ; pop eax
// MODE 0x400 = down, 0x800 = up (both assume the thread's rounding control is the default 00, as in
// campfix.c), 0xC00 = truncate (any rounding control).
// Replacements:
//   truncate + fistp: fistp -> fisttp (always truncates), both fldcw -> NOP; all other bytes unchanged.
//   otherwise: push ecx/eax ; movd ecx,xmm0 ; roundss xmm0,[input],mode ; (cvttss2si eax,xmm0 ;
//   mov [result],eax | movss [result],xmm0) ; pinsrd xmm0,ecx,0 ; pop. Same integer / same float bits
//   (sign of zero, NaN, overflow -> 0x80000000 included) while MXCSR has DAZ off. The CW temporaries
//   [esp+C] and [esp+T] are compiler scratch for this block (written only by it), so they are not written.
// x87fix_frame() (per frame, main thread; other game threads suspended meanwhile) puts the original bytes back for the down/up/frndint sites if
// the main thread's rounding control or DAZ ever changes, or if the watcher thread (every 10 s it briefly
// suspends each game thread and reads its x87 control word and MXCSR) saw another thread with them. x87fix_test.c runs every matched site's
// original bytes and replacement on edge values and random inputs.
#include <windows.h>
#include <tlhelp32.h>
#include "gamerva.h"
#include <string.h>

#define X87_MAX_SITES 2048
struct x87site {
    unsigned rva;
    unsigned char len, mode, frndint, a, c, t, r;  // mode: 1 down, 2 up, 3 trunc
    unsigned char orig[38];
    int patched;
};
static struct x87site g_x87[X87_MAX_SITES];
static int g_x87_n;
static unsigned char *g_x87_base;
static void (*g_x87_log)(const char *fmt, ...);
static int g_x87_reverted;

// Matches one block at p. Returns its length (36 or 38) or 0.
static int x87_match(const unsigned char *p, struct x87site *s) {
    if (p[0] != 0x50 || p[1] != 0xD9 || p[2] != 0x44 || p[3] != 0x24) return 0;                      // push eax; fld [esp+A]
    if (p[5] != 0x9B || p[6] != 0xD9 || p[7] != 0x7C || p[8] != 0x24) return 0;                      // fstcw [esp+C]
    if (p[10] != 0x8B || p[11] != 0x44 || p[12] != 0x24 || p[13] != p[9]) return 0;                 // mov eax,[esp+C]
    if (p[14] != 0x0D || p[15] != 0x00 || (p[16] != 0x04 && p[16] != 0x08 && p[16] != 0x0C) || p[17] || p[18]) return 0;
    if (p[19] != 0x89 || p[20] != 0x44 || p[21] != 0x24) return 0;                                   // mov [esp+T],eax
    if (p[23] != 0xD9 || p[24] != 0x6C || p[25] != 0x24 || p[26] != p[22]) return 0;                 // fldcw [esp+T]
    int k, frndint = 0;
    unsigned char r;
    if (p[27] == 0xDB && p[28] == 0x5C && p[29] == 0x24) { r = p[30]; k = 31; }                      // fistp dword [esp+R]
    else if (p[27] == 0xD9 && p[28] == 0xFC && p[29] == 0xD9 && p[30] == 0x5C && p[31] == 0x24) {   // frndint; fstp dword [esp+R]
        r = p[32]; k = 33; frndint = 1;
    } else return 0;
    if (p[k] != 0xD9 || p[k + 1] != 0x6C || p[k + 2] != 0x24 || p[k + 3] != p[9] || p[k + 4] != 0x58) return 0;  // fldcw [esp+C]; pop eax
    unsigned char a = p[4], c = p[9], t = p[22];
    if (a >= 0x7C || r >= 0x7C || a < 4 || r < 4) return 0;  // disp8 room for the +4 below; input/result above the pushed eax
    if (r == c || r == t || a == c || a == t) return 0;        // result/input must not be the CW scratch slots
    s->len = (unsigned char)(k + 5);
    s->mode = p[16] == 0x04 ? 1 : p[16] == 0x08 ? 2 : 3;
    s->frndint = (unsigned char)frndint;
    s->a = a; s->c = c; s->t = t; s->r = r;
    return s->len;
}

// Writes the replacement for s into out (s->len bytes).
static void x87_replacement(const struct x87site *s, unsigned char *out) {
    unsigned char *p = out;
    if (s->mode == 3 && !s->frndint) {  // fisttp in place
        memcpy(out, s->orig, s->len);
        memcpy(out + 23, "\x0F\x1F\x40\x00", 4);  // fldcw [esp+T] -> nop
        out[28] = 0x4C;                           // DB 5C (fistp m32) -> DB 4C (fisttp m32)
        memcpy(out + 31, "\x0F\x1F\x40\x00", 4);  // fldcw [esp+C] -> nop
        return;
    }
    unsigned char imm = s->mode == 1 ? 0x09 : s->mode == 2 ? 0x0A : 0x0B;  // floor / ceil / trunc, no precision exception
    if (s->frndint) {
        // push ecx (offsets as in the original, which pushed eax) ; movd ecx,xmm0 ; roundss xmm0,[esp+A],imm ;
        // movss [esp+R],xmm0 ; pinsrd xmm0,ecx,0 ; pop ecx
        *p++ = 0x51;
        memcpy(p, "\x66\x0F\x7E\xC1", 4); p += 4;
        memcpy(p, "\x66\x0F\x3A\x0A\x44\x24", 6); p += 6; *p++ = s->a; *p++ = imm;
        memcpy(p, "\xF3\x0F\x11\x44\x24", 5); p += 5; *p++ = s->r;
        memcpy(p, "\x66\x0F\x3A\x22\xC1\x00", 6); p += 6;
        *p++ = 0x59;
    } else {
        // push ecx ; push eax (offsets +4) ; movd ecx,xmm0 ; roundss xmm0,[esp+A+4],imm ; cvttss2si eax,xmm0 ;
        // mov [esp+R+4],eax ; pinsrd xmm0,ecx,0 ; pop eax ; pop ecx
        *p++ = 0x51; *p++ = 0x50;
        memcpy(p, "\x66\x0F\x7E\xC1", 4); p += 4;
        memcpy(p, "\x66\x0F\x3A\x0A\x44\x24", 6); p += 6; *p++ = (unsigned char)(s->a + 4); *p++ = imm;
        memcpy(p, "\xF3\x0F\x2C\xC0", 4); p += 4;
        memcpy(p, "\x89\x44\x24", 3); p += 3; *p++ = (unsigned char)(s->r + 4);
        memcpy(p, "\x66\x0F\x3A\x22\xC1\x00", 6); p += 6;
        *p++ = 0x58; *p++ = 0x59;
    }
    while (p < out + s->len) *p++ = 0x90;
}

static void x87_write(unsigned char *at, const unsigned char *bytes, unsigned n) {
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
}

static unsigned short x87_cw(void) {
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    return cw;
}
static unsigned x87_mxcsr(void) {
    unsigned m;
    __asm__ volatile("stmxcsr %0" : "=m"(m));
    return m;
}

// Finds the blocks in [text, text+size) (RVA rva0). Fills g_x87; returns the count.
static int x87_scan(const unsigned char *text, unsigned size, unsigned rva0) {
    g_x87_n = 0;
    for (unsigned i = 0; i + 38 <= size && g_x87_n < X87_MAX_SITES;) {
        struct x87site *s = &g_x87[g_x87_n];
        int len = text[i] == 0x50 ? x87_match(text + i, s) : 0;
        if (len) {
            s->rva = rva0 + i;
            memcpy(s->orig, text + i, len);
            s->patched = 0;
            g_x87_n++;
            i += len;
        } else i++;
    }
    return g_x87_n;
}

// Set by the watcher thread when some game thread runs with non-default rounding control or DAZ.
static volatile LONG g_x87_other_thread_bad;

// Puts the original bytes of the down/up/frndint sites back while every other game thread is suspended, so none runs
// half-rewritten code (paused_write, gamerva.h). 0 (nothing written) if a suspended thread stood inside a site: tried
// again next frame.
static int g_x87_restored;
static int x87_busy(UINT_PTR eip) {
    DWORD rva = (DWORD)eip - (DWORD)(UINT_PTR)g_x87_base;
    for (int i = 0; i < g_x87_n; i++)
        if (g_x87[i].patched && rva - g_x87[i].rva < g_x87[i].len) return 1;
    return 0;
}
static void x87_restore(void) {
    for (int i = 0; i < g_x87_n; i++) {
        struct x87site *s = &g_x87[i];
        if (s->patched && !(s->mode == 3 && !s->frndint)) {
            x87_write(g_x87_base + s->rva, s->orig, s->len);
            s->patched = 0;
            g_x87_restored++;
        }
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
}
static int x87_restore_paused(int *restored) {
    g_x87_restored = 0;
    int ok = paused_write((HMODULE)g_x87_base, x87_busy, x87_restore);
    *restored = g_x87_restored;
    return ok;
}

// Per frame on the main thread: the down/up/frndint replacements assume default rounding control and DAZ off.
void x87fix_frame(void) {
    if (g_x87_reverted || ((x87_cw() & 0xC00) == 0 && !(x87_mxcsr() & 0x40) && !g_x87_other_thread_bad)) return;
    int n = 0;
    if (!x87_restore_paused(&n)) return;
    g_x87_reverted = 1;
    g_x87_log("x87 fix: non-default rounding control or DAZ seen on a game thread - %d rounding sites restored", n);
}

// Every 10 s: briefly suspend each game thread and read its x87 control word and MXCSR.
static DWORD WINAPI x87_watch(LPVOID unused) {
    (void)unused;
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    for (;;) {
        Sleep(10000);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 te = {.dwSize = sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            if (!game_thread(h, (HMODULE)g_x87_base)) { CloseHandle(h); continue; }
            CONTEXT ctx = {.ContextFlags = CONTEXT_FLOATING_POINT | CONTEXT_EXTENDED_REGISTERS};
            if (SuspendThread(h) != (DWORD)-1) {
                BOOL got = GetThreadContext(h, &ctx);
                ResumeThread(h);
                unsigned mxcsr;
                memcpy(&mxcsr, ctx.ExtendedRegisters + 24, 4);  // FXSAVE layout: MXCSR at byte 24
                if (got && ((ctx.FloatSave.ControlWord & 0xC00) || (mxcsr & 0x40)) && !g_x87_other_thread_bad) {
                    g_x87_log("x87 fix: thread %lu runs with x87 control word %04lX, MXCSR %08X - rounding sites will be restored",
                              te.th32ThreadID, ctx.FloatSave.ControlWord & 0xFFFF, mxcsr);
                    InterlockedExchange(&g_x87_other_thread_bad, 1);
                }
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
        if (g_x87_reverted) return 0;
    }
}

static int cpu_ecx1(void) {   // CPUID leaf 1 ECX: bit 0 SSE3 (fisttp), bit 19 SSE4.1 (roundss)
    int eax = 1, ecx;
    __asm__ volatile("cpuid" : "+a"(eax), "=c"(ecx) : : "ebx", "edx");   // cpuid also overwrites EAX
    return ecx;
}
static int cpu_has_sse41(void) { return (cpu_ecx1() >> 19) & 1; }

// Must run on the game's main thread. Returns the number of blocks replaced.
int install_x87fix(HMODULE m, void (*log)(const char *fmt, ...)) {
    unsigned char *b = (unsigned char *)m;
    g_x87_base = b;
    g_x87_log = log;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (!memcmp(sec->Name, ".text", 6)) break;
    if ((unsigned char *)sec >= (unsigned char *)IMAGE_FIRST_SECTION(nt) + nt->FileHeader.NumberOfSections * sizeof *sec) return 0;
    int sse3 = cpu_ecx1() & 1, sse41 = cpu_has_sse41(), defaults = (x87_cw() & 0xC00) == 0 && !(x87_mxcsr() & 0x40);
    int found = x87_scan(b + sec->VirtualAddress, sec->Misc.VirtualSize, sec->VirtualAddress), n = 0, fast = 0;
    for (int i = 0; i < found; i++) {
        struct x87site *s = &g_x87[i];
        int fisttp = s->mode == 3 && !s->frndint;
        if (fisttp ? !sse3 : !(sse41 && defaults)) continue;
        unsigned char repl[38];
        x87_replacement(s, repl);
        x87_write(b + s->rva, repl, s->len);
        s->patched = 1;
        n++;
        fast += fisttp;
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    if (n > fast) {
        HANDLE w = CreateThread(NULL, 0, x87_watch, NULL, 0, NULL);
        if (w) CloseHandle(w);
    }
    log("x87 fix: %d of %d rounding-mode blocks replaced (%d truncating via fisttp, %d via SSE4.1 roundss)%s", n, found,
        fast, n - fast, !sse3 ? " - SSE3 missing: none of the truncating ones" : sse41 && defaults ? "" : " - SSE4.1 missing or non-default FPU state: only the truncating ones");
    return n;
}
