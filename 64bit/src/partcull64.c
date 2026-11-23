// partcull64: unit detail cull, ported from heapfix/partcull.c (read that file's header for the rule and its history).
// A soldier's small parts are not turned into draw items once they would be only a few pixels tall.
// Engine: WARSCAPE::RIGID_SCENE_NODE_V2::add_instance at 0x33f2690 (SysV; r15 = the RIGID_PARAMS in effect).
//   LOD  0x33f2c96  movss [rsp+0xd0], xmm1      the metric handed to get_lod; the node is at [rsp+0xa8], its distance
//                                                scale at node+0xe94 (x86: node+0xC30)
//   P1   0x33f2eaf  mov r14, [rax+r13*8] ; mov eax, 1      the LOD's part loop; skip -> 0x33f3467 (inc r13 ...)
//   P2   0x33f350b  mov rdx, [rsi+0x30] ; mov rbp, [rdx+rcx*8]   the fading previous LOD's loop; skip -> 0x33f3bea
//                                                (inc r13d ; cmp r13d, eax: eax must hold the count, [rsi+0x2c])
// Part = WARSCAPE::RIGID_MESH; its model-space box (MESH::m_aabb) is at +0x2c (x86: +0x1C). Animated models only:
// RIGID_PARAMS::m_animation_matrix_stack at +0x28 (x86: +0x1C).
// The jumps at the sites are 5-byte rel32 to relays in memory near the engine; each relay jumps on to a stub here.
// Not ported: the partcull_probe.txt log (a 32-bit siege investigation).
#include <string.h>
#include <math.h>
#include "perffix64.h"

#define PC_LOD 0x33f2c96u
#define PC_P1 0x33f2eafu
#define PC_P1_SKIP 0x33f3467u
#define PC_P2 0x33f350bu
#define PC_P2_SKIP 0x33f3beau
static const uint8_t PC_LOD_B[9] = {0xf3, 0x0f, 0x11, 0x8c, 0x24, 0xd0, 0x00, 0x00, 0x00};
static const uint8_t PC_P1_B[9] = {0x4e, 0x8b, 0x34, 0xe8, 0xb8, 0x01, 0x00, 0x00, 0x00};
static const uint8_t PC_P2_B[8] = {0x48, 0x8b, 0x56, 0x30, 0x48, 0x8b, 0x2c, 0xca};
static const uint8_t PC_P1_SKIP_B[3] = {0x49, 0xff, 0xc5}, PC_P2_SKIP_B[3] = {0x41, 0xff, 0xc5};

// read by the stubs; set once at install
void *pcull64_ret_lod, *pcull64_ret_p1, *pcull64_ret_p2, *pcull64_skip_p1, *pcull64_skip_p2;

volatile LONG g_partcull_on = 1;
float g_partcull_px = 22.0f;              // the most that showed no pop-in in game (x86)
float g_partcull_head_px = 9.0f;
static const float PC_MAX_DIAG = 1.0f;    // longer parts (spears, lances) shape the silhouette: never culled
static const float PC_F = 2607.0f;        // 2160 / (2 tan 22.5 deg): the thresholds are 4K pixels everywhere
static const float PC_MAX_DIST = 250.0f;  // m; past it a soldier is an imposter, so a larger distance is a forced one
static DWORD pcull_tls = TLS_OUT_OF_INDEXES;

// the part loops run on several threads at once, so the model's distance and the counters are per thread
struct pcull_th { float dist; unsigned parts, skipped, forced; struct pcull_th *next; };
static struct pcull_th *pcull_threads;
static CRITICAL_SECTION pcull_cs;

static struct pcull_th *pcull_me(void) {
    struct pcull_th *t = TlsGetValue(pcull_tls);
    if (t) return t;
    t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *t);
    if (!t) return NULL;
    t->dist = -1;
    EnterCriticalSection(&pcull_cs);
    t->next = pcull_threads;
    pcull_threads = t;
    LeaveCriticalSection(&pcull_cs);
    TlsSetValue(pcull_tls, t);
    return t;
}

void partcull64_counts(unsigned *parts, unsigned *skipped, unsigned *forced) {
    *parts = *skipped = *forced = 0;
    for (struct pcull_th *t = pcull_threads; t; t = t->next) { *parts += t->parts; *skipped += t->skipped; *forced += t->forced; }
}

// the model's distance, from the metric the engine just built: (scale * distance)^2 + small terms
__attribute__((sysv_abi)) void pcull64_lod(float metric, const uint8_t *node) {
    struct pcull_th *t = pcull_me();
    if (!t) return;
    float scale;
    memcpy(&scale, node + 0xe94, 4);
    t->dist = (scale > 1e-6f && metric > 0) ? sqrtf(metric) / scale : -1.0f;
}

// 1 = do not make a draw item for this part
__attribute__((sysv_abi)) int pcull64_part(const uint8_t *part, const uint8_t *params) {
    struct pcull_th *t = pcull_me();
    if (!t) return 0;
    t->parts++;
    void *pose = NULL;
    if (params) memcpy(&pose, params + 0x28, 8);
    if (!g_partcull_on || !pose || t->dist <= 0) return 0;   // only animated models
    if (t->dist > PC_MAX_DIST) { t->forced++; return 0; }    // not a camera distance (sieges force 500)
    float b[6];
    memcpy(b, part + 0x2c, sizeof b);
    float dx = b[3] - b[0], dy = b[4] - b[1], dz = b[5] - b[2];
    if (!(dx >= 0 && dy >= 0 && dz >= 0)) return 0;   // not a box, or NaN: keep
    float diag = sqrtf(dx * dx + dy * dy + dz * dz);
    if (diag >= PC_MAX_DIAG) return 0;
    float cx = (b[0] + b[3]) * 0.5f, cy = (b[1] + b[4]) * 0.5f, cz = (b[2] + b[5]) * 0.5f;
    int head = fabsf(cx) < 0.25f && fabsf(cz) < 0.3f && cy > 1.4f && cy < 2.1f;
    float px = head ? g_partcull_head_px : g_partcull_px;
    if (diag * PC_F >= px * t->dist) return 0;
    t->skipped++;
    return 1;
}

// Every caller-saved register, the flags and all 16 xmm registers are kept around the call: the loops are mid-function.
// 10 pushes = 80 bytes keeps rsp 16-aligned (it is aligned in the function's body).
#define PC_SAVE \
    "pushfq\n push %rax\n push %rcx\n push %rdx\n push %rsi\n push %rdi\n push %r8\n push %r9\n push %r10\n push %r11\n" \
    "sub $0x100, %rsp\n" \
    "movups %xmm0, 0x00(%rsp)\n movups %xmm1, 0x10(%rsp)\n movups %xmm2, 0x20(%rsp)\n movups %xmm3, 0x30(%rsp)\n" \
    "movups %xmm4, 0x40(%rsp)\n movups %xmm5, 0x50(%rsp)\n movups %xmm6, 0x60(%rsp)\n movups %xmm7, 0x70(%rsp)\n" \
    "movups %xmm8, 0x80(%rsp)\n movups %xmm9, 0x90(%rsp)\n movups %xmm10, 0xa0(%rsp)\n movups %xmm11, 0xb0(%rsp)\n" \
    "movups %xmm12, 0xc0(%rsp)\n movups %xmm13, 0xd0(%rsp)\n movups %xmm14, 0xe0(%rsp)\n movups %xmm15, 0xf0(%rsp)\n"
#define PC_RESTORE \
    "movups 0x00(%rsp), %xmm0\n movups 0x10(%rsp), %xmm1\n movups 0x20(%rsp), %xmm2\n movups 0x30(%rsp), %xmm3\n" \
    "movups 0x40(%rsp), %xmm4\n movups 0x50(%rsp), %xmm5\n movups 0x60(%rsp), %xmm6\n movups 0x70(%rsp), %xmm7\n" \
    "movups 0x80(%rsp), %xmm8\n movups 0x90(%rsp), %xmm9\n movups 0xa0(%rsp), %xmm10\n movups 0xb0(%rsp), %xmm11\n" \
    "movups 0xc0(%rsp), %xmm12\n movups 0xd0(%rsp), %xmm13\n movups 0xe0(%rsp), %xmm14\n movups 0xf0(%rsp), %xmm15\n" \
    "add $0x100, %rsp\n" \
    "pop %r11\n pop %r10\n pop %r9\n pop %r8\n pop %rdi\n pop %rsi\n pop %rdx\n pop %rcx\n pop %rax\n popfq\n"

__attribute__((naked)) void pcull64_stub_lod(void) {
    __asm__ volatile(
        "movss %xmm1, 0xd0(%rsp)\n"              // the site's instruction
        PC_SAVE
        "movaps %xmm1, %xmm0\n"
        "mov 0x1f8(%rsp), %rdi\n"                // the node: [rsp+0xa8] before the 0x150 bytes saved above
        "call pcull64_lod\n"
        PC_RESTORE
        "jmp *pcull64_ret_lod(%rip)\n");
}
__attribute__((naked)) void pcull64_stub_p1(void) {
    __asm__ volatile(
        "mov (%rax,%r13,8), %r14\n"              // the site's first instruction: the part
        PC_SAVE
        "mov %r14, %rdi\n mov %r15, %rsi\n"
        "call pcull64_part\n"
        "test %eax, %eax\n jnz 1f\n"
        PC_RESTORE
        "mov $1, %eax\n"                         // the site's second instruction
        "jmp *pcull64_ret_p1(%rip)\n"
        "1:\n"
        PC_RESTORE
        "jmp *pcull64_skip_p1(%rip)\n");
}
__attribute__((naked)) void pcull64_stub_p2(void) {
    __asm__ volatile(
        "mov 0x30(%rsi), %rdx\n mov (%rdx,%rcx,8), %rbp\n"   // the site's two instructions: the part
        PC_SAVE
        "mov %rbp, %rdi\n mov %r15, %rsi\n"
        "call pcull64_part\n"
        "test %eax, %eax\n jnz 1f\n"
        PC_RESTORE
        "jmp *pcull64_ret_p2(%rip)\n"
        "1:\n"
        PC_RESTORE
        "mov 0x2c(%rsi), %eax\n"                 // the increment block compares against the count in eax
        "jmp *pcull64_skip_p2(%rip)\n");
}

// relay at r (16 bytes): jmp [rip+0] ; abs64
static __attribute__((unused)) void pc_relay(uint8_t *r, void *to) {
    uint64_t a = (uint64_t)(uintptr_t)to;
    r[0] = 0xFF; r[1] = 0x25; memset(r + 2, 0, 4); memcpy(r + 6, &a, 8); r[14] = r[15] = 0xCC;
}
// the bytes for `site`: jmp rel32 to `relay`, nop-padded to len
void partcull64_jmp(uint8_t *out, int len, uint64_t site, uint64_t relay) {
    int32_t rel = (int32_t)(int64_t)(relay - (site + 5));
    out[0] = 0xE9; memcpy(out + 1, &rel, 4); memset(out + 5, 0x90, len - 5);
}

void partcull64_setup(void) {   // the per-thread state; the test calls this without the install
    if (pcull_tls == TLS_OUT_OF_INDEXES) { pcull_tls = TlsAlloc(); InitializeCriticalSection(&pcull_cs); }
}

#ifndef PARTCULL64_TEST
// code: 48 bytes of executable memory within +-2 GB of the engine. 1 installed, 0 bytes differ, -1 a thread inside a
// site or no near memory, -2 VirtualProtect failed. Runs while every other thread is paused.
int install_partcull64(uint8_t *code) {
    if (memcmp((void *)(uintptr_t)PC_LOD, PC_LOD_B, 9) || memcmp((void *)(uintptr_t)PC_P1, PC_P1_B, 9) ||
        memcmp((void *)(uintptr_t)PC_P2, PC_P2_B, 8) || memcmp((void *)(uintptr_t)PC_P1_SKIP, PC_P1_SKIP_B, 3) ||
        memcmp((void *)(uintptr_t)PC_P2_SKIP, PC_P2_SKIP_B, 3)) return 0;
    int64_t d = (int64_t)((uintptr_t)code - PC_LOD);
    if (!code || d < -0x7FFF0000ll || d > 0x7FFF0000ll) return -1;
    if (thread_inside(PC_LOD, 9) || thread_inside(PC_P1, 9) || thread_inside(PC_P2, 8)) return -1;
    partcull64_setup();
    if (pcull_tls == TLS_OUT_OF_INDEXES) return -2;
    pcull64_ret_lod = (void *)(uintptr_t)(PC_LOD + 9);
    pcull64_ret_p1 = (void *)(uintptr_t)(PC_P1 + 9);
    pcull64_ret_p2 = (void *)(uintptr_t)(PC_P2 + 8);
    pcull64_skip_p1 = (void *)(uintptr_t)PC_P1_SKIP;
    pcull64_skip_p2 = (void *)(uintptr_t)PC_P2_SKIP;
    pc_relay(code, (void *)pcull64_stub_lod);
    pc_relay(code + 16, (void *)pcull64_stub_p1);
    pc_relay(code + 32, (void *)pcull64_stub_p2);
    uint8_t j[9];
    // the distance hook first: the part hooks do nothing until a distance is known
    partcull64_jmp(j, 9, PC_LOD, (uintptr_t)code);
    if (!patch_code((void *)(uintptr_t)PC_LOD, j, 9)) return -2;
    partcull64_jmp(j, 9, PC_P1, (uintptr_t)code + 16);
    if (!patch_code((void *)(uintptr_t)PC_P1, j, 9)) return -2;
    partcull64_jmp(j, 8, PC_P2, (uintptr_t)code + 32);
    return patch_code((void *)(uintptr_t)PC_P2, j, 8) ? 1 : -2;
}
#endif
