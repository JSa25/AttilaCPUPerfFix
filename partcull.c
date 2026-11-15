// Unit detail cull: a soldier's small parts (belts, quivers, gauntlets, sabers, bridles) are not turned into draw items
// once they would be only a few pixels tall. The engine culls per model, never per part, so every piece of a visible
// soldier goes to every view he is in; with a unit mod like 1212 AD, where most parts have no lower detail level, that is
// the main thread's largest single cost in a battle. Measured in the 1212 Abbasid bodyguard battle (4K, camera still):
// 81.3 -> 103.5 fps, +27 %, with the removals confined to the far ranks (notes/part_cull_design.md).
//
// Where: WARSCAPE::RIGID_SCENE_NODE_V2::add_instance (0x17F61A0), which picks a model's detail level and then loops over
// that level's parts, plus a second loop for the previous level while it fades. Skipping there is before the draw item,
// its instance record, the list pushes, the sort, the pass loops and the draw, in the main view and all 4 shadow
// cascades at once. The part object carries its own bounding box at +0x1C (min xyz, max xyz, model space).
//
// Rule: skip a part whose box is under 1 m across and whose size on screen is under the threshold. Head parts (helmets,
// faces, hair, beards: box centred over the body at head height, which catches 4,106 of 4,177 helmet meshes across 1212
// and vanilla and 1 shield in 3,403) get a gentler threshold of their own, because in vanilla a bare head shows when a
// helmet goes. Thresholds are 4K pixels at every resolution, so the look does not change with the screen.
// Only animated models are touched; props, buildings and trees go through the same code untouched.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include "gamerva.h"

#define PC_LOD 0x17F6437       // after the detail-level pick: mov ebx,eax ; mov [esp+2Ch],ebx
#define PC_P1 0x17F657A        // part loop: mov ebx,[eax+ecx*4] ; mov ecx,[esp+44h]
#define PC_P1_NEXT 0x17F6B00   // its increment block (reloads the registers it needs)
#define PC_P2 0x17F6B43        // the fading previous level's loop: mov ebx,[eax+ecx*4] ; mov eax,[esp+74h]
#define PC_P2_NEXT 0x17F701E
static const unsigned char PC_LOD_B[6] = {0x8B, 0xD8, 0x89, 0x5C, 0x24, 0x2C};
static const unsigned char PC_P1_B[7] = {0x8B, 0x1C, 0x88, 0x8B, 0x4C, 0x24, 0x44};
static const unsigned char PC_P2_B[7] = {0x8B, 0x1C, 0x88, 0x8B, 0x44, 0x24, 0x74};
static const unsigned char PC_P1_NEXT_B[4] = {0x8B, 0x4C, 0x24, 0x1C};
static const unsigned char PC_P2_NEXT_B[4] = {0x8B, 0x4C, 0x24, 0x14};

// read by the stubs; set once at install
void *pcull_ret_lod, *pcull_ret_p1, *pcull_ret_p2, *pcull_skip_p1, *pcull_skip_p2;

volatile LONG g_partcull_on = 1;          // heapfix.ini / the add-on's panel
float g_partcull_px = 22.0f;              // the most that showed no pop-in in game
float g_partcull_head_px = 9.0f;
static const float PC_MAX_DIAG = 1.0f;    // longer parts (spears, lances) shape the silhouette: never culled
// A caller can hand the engine a distance instead of letting it measure from the camera (instance parameters +0xA4,
// against a marker meaning "none"), and sieges use that to force a model to its lowest detail level - with 500. Taking
// that as a real distance made soldiers in front of the camera lose their parts, and riders came off their horses (the
// probe's log was 200 lines of "caller gave the distance: 500.0"). Ignoring every supplied distance is no good either:
// open battles supply them too, and there they are real, so that turned the whole fix off (89.1 vs 87.3 fps, nothing).
// What is safe is the size of the number. The engine draws a soldier past ~200 m as a flat imposter, with no parts at
// all, so a part arriving with a distance far beyond that is not a soldier out there - it is a forced value like the
// siege's. Distances over PC_MAX_DIST are not believed and nothing is culled on them.
// Tried and dropped first: keeping any part with a box volume over 0.02 m3. It did protect vanilla's 133 body meshes,
// but body and armour bulk overlap, so it also spared most of 1212's armour - the share of a rider culled at 100 m fell
// 61 % -> 46 % and the three panel settings collapsed into each other (18 % / 22 % in game against 51 % / 65 %).
static const float PC_F = 2607.0f;        // 2160 / (2 tan 22.5 deg): the thresholds are 4K pixels everywhere
static const float PC_MAX_DIST = 250.0f;  // m; past it a soldier is an imposter, so a larger distance is a forced one
static DWORD pcull_tls = TLS_OUT_OF_INDEXES;

// the part loops run on several threads at once, so the model's distance and the counters are per thread
struct pcull_th { float dist, scale, metric; unsigned parts, skipped, forced; struct pcull_th *next; };
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

void partcull_counts(unsigned *parts, unsigned *skipped) {
    *parts = *skipped = 0;
    for (struct pcull_th *t = pcull_threads; t; t = t->next) { *parts += t->parts; *skipped += t->skipped; }
}
// parts the engine placed at a distance of its own choosing, which this fix leaves alone
unsigned partcull_forced(void) {
    unsigned n = 0;
    for (struct pcull_th *t = pcull_threads; t; t = t->next) n += t->forced;
    return n;
}

// the model's distance, from the metric the engine just built: (node[0xC30] * distance)^2 + small terms
void __cdecl pcull_lod(unsigned metric_bits, const unsigned char *node) {
    struct pcull_th *t = pcull_me();
    if (!t) return;
    float m, scale;
    memcpy(&m, &metric_bits, 4);
    memcpy(&scale, node + 0xC30, 4);
    t->metric = m;
    t->scale = scale;
    t->dist = (scale > 1e-6f && m > 0) ? sqrtf(m) / scale : -1.0f;
}

// ---- probe (partcull_probe.txt in the game folder): why a big part gets culled ----
// Riders vanish in sieges but not in open battles, so something the cull reads differs there. This writes the first
// PC_PROBE_N culled parts over 0.45 m to partcull_probe.log with everything the decision used: the box, the distance
// and what it was built from, and whether the caller handed the engine a distance instead of the camera working it out
// (instance parameters +0xA4 against the engine's "none" value). Off unless the file is there; one flag read per part.
#define PC_PROBE_N 200
// Only parts close to the camera. A siege culls 200 legitimate parts over 100 m away before the camera is anywhere
// near the rider whose armour goes, so an unbounded probe fills with correct decisions and never sees the bug.
#define PC_PROBE_MAX_DIST 60.0f
static volatile LONG pcull_probe_on, pcull_probe_left = PC_PROBE_N;
static const float *pcull_no_dist;   // the engine's "no distance supplied" marker (set at install, always)
static wchar_t pcull_probe_path[MAX_PATH];

static void pcull_probe(const unsigned char *part, const unsigned *inst, const float *b, float diag, float px,
                        const struct pcull_th *t) {
    if (diag < 0.45f || t->dist > PC_PROBE_MAX_DIST || InterlockedDecrement(&pcull_probe_left) < 0) return;
    float given = 0;
    int supplied = 0;
    if (inst) {
        memcpy(&given, (const unsigned char *)inst + 0xA4, 4);
        supplied = !pcull_no_dist || memcmp(&given, pcull_no_dist, 4) != 0;
    }
    unsigned verts = 0, idx = 0;
    memcpy(&verts, part + 0xBC, 4);
    memcpy(&idx, part + 0xC0, 4);
    // one buffer, one append: the part loops run on several threads and interleaved fprintf tore 17 of 131 lines
    char line[320];
    int n = snprintf(line, sizeof line,
                     "culled %.2f m box %.2f %.2f %.2f  dist %.1f (metric %.1f / scale %.3f)  on screen %.1f px vs"
                     " %.0f  %s%.1f  verts %u tris %u\n",
                     diag, b[3] - b[0], b[4] - b[1], b[5] - b[2], t->dist, t->metric, t->scale,
                     diag * PC_F / t->dist, px, supplied ? "caller gave the distance: " : "camera distance, marker ",
                     given, verts, idx / 3);
    if (n <= 0) return;
    if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;
    HANDLE h = CreateFileW(pcull_probe_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote;
    WriteFile(h, line, (DWORD)n, &wrote, NULL);   // FILE_APPEND_DATA: Windows appends each write whole
    CloseHandle(h);
}

// 1 = do not make a draw item for this part
int __cdecl pcull_part(const unsigned char *part, const unsigned *inst) {
    struct pcull_th *t = pcull_me();
    if (!t) return 0;
    t->parts++;
    if (!g_partcull_on || !inst || !inst[7] || t->dist <= 0) return 0;   // inst[7] = the pose: only animated models
    if (t->dist > PC_MAX_DIST) { t->forced++; return 0; }   // not a camera distance: see above
    float b[6];
    memcpy(b, part + 0x1C, sizeof b);
    float dx = b[3] - b[0], dy = b[4] - b[1], dz = b[5] - b[2];
    if (!(dx >= 0 && dy >= 0 && dz >= 0)) return 0;   // not a box, or NaN: keep
    float diag = sqrtf(dx * dx + dy * dy + dz * dz);
    if (diag >= PC_MAX_DIAG) return 0;
    float cx = (b[0] + b[3]) * 0.5f, cy = (b[1] + b[4]) * 0.5f, cz = (b[2] + b[5]) * 0.5f;
    int head = fabsf(cx) < 0.25f && fabsf(cz) < 0.3f && cy > 1.4f && cy < 2.1f;
    float px = head ? g_partcull_head_px : g_partcull_px;
    if (diag * PC_F >= px * t->dist) return 0;
    t->skipped++;
    if (pcull_probe_on) pcull_probe(part, inst, b, diag, px, t);
    return 1;
}

// Each stub replays the instructions its site held, then either returns to the game or jumps to that loop's increment
// block. Every register and flag the game uses is preserved; the increment blocks reload what they need themselves.
__attribute__((naked)) static void pcull_stub_lod(void) {
    __asm__ volatile(
        "pushal\n pushfl\n"
        "pushl 20(%esp)\n pushl 0x44(%esp)\n"    // the node (saved ebx), the metric at [esp+1Ch]
        "call _pcull_lod\n addl $8, %esp\n"
        "popfl\n popal\n"
        "movl %eax, %ebx\n movl %ebx, 0x2C(%esp)\n"
        "jmp *_pcull_ret_lod\n");
}
__attribute__((naked)) static void pcull_stub_p1(void) {
    __asm__ volatile(
        "movl (%eax,%ecx,4), %ebx\n movl 0x44(%esp), %ecx\n"
        "pushal\n pushfl\n"
        "pushl 0x64(%esp)\n pushl %ebx\n"        // the instance parameters in effect ([esp+40h]), the part
        "call _pcull_part\n addl $8, %esp\n"
        "testl %eax, %eax\n"
        "jnz 1f\n"
        "popfl\n popal\n jmp *_pcull_ret_p1\n"
        "1: popfl\n popal\n addl $4, %esp\n jmp *_pcull_skip_p1\n");   // one argument was pushed before the site
}
__attribute__((naked)) static void pcull_stub_p2(void) {
    __asm__ volatile(
        "movl (%eax,%ecx,4), %ebx\n movl 0x74(%esp), %eax\n"
        "pushal\n pushfl\n"
        "pushl 0x60(%esp)\n pushl %ebx\n"        // here the parameters are at [esp+3Ch]: one push less than loop 1
        "call _pcull_part\n addl $8, %esp\n"
        "testl %eax, %eax\n"
        "jnz 1f\n"
        "popfl\n popal\n jmp *_pcull_ret_p2\n"
        "1: popfl\n popal\n jmp *_pcull_skip_p2\n");
}

static void pcull_jmp(unsigned char *at, int len, void *to) {
    DWORD old;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return;
    at[0] = 0xE9;
    DWORD rel = (DWORD)((unsigned char *)to - (at + 5));
    memcpy(at + 1, &rel, 4);
    memset(at + 5, 0x90, len - 5);
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
}

// 1 installed, 0 not (another game build, or already installed)
int install_partcull(HMODULE m, void (*log)(const char *fmt, ...)) {
    static int done;
    if (done) return 0;
    unsigned char *b = (unsigned char *)m;
    unsigned r_lod = game_rva(b, PC_LOD), r_p1 = game_rva(b, PC_P1), r_p2 = game_rva(b, PC_P2);
    unsigned r_n1 = game_rva(b, PC_P1_NEXT), r_n2 = game_rva(b, PC_P2_NEXT);
    if (!r_lod || !r_p1 || !r_p2 || !r_n1 || !r_n2) {
        log("unit detail cull: this game build's addresses are not known - not installed");
        return 0;
    }
    unsigned char *l = b + r_lod, *p1 = b + r_p1, *p2 = b + r_p2;
    if (memcmp(l, PC_LOD_B, 6) || memcmp(p1, PC_P1_B, 7) || memcmp(p2, PC_P2_B, 7) ||
        memcmp(b + r_n1, PC_P1_NEXT_B, 4) || memcmp(b + r_n2, PC_P2_NEXT_B, 4)) {
        log("unit detail cull: the game's code is not what this build expects - not installed");
        return 0;
    }
    if (pcull_tls == TLS_OUT_OF_INDEXES && (pcull_tls = TlsAlloc()) == TLS_OUT_OF_INDEXES) return 0;
    {   // only the probe reads it; a build without it just logs no marker
        unsigned r_marker = game_rva(b, 0x1B0F08C);
        pcull_no_dist = r_marker ? (const float *)(b + r_marker) : NULL;
    }
    InitializeCriticalSection(&pcull_cs);
    pcull_ret_lod = l + 6;
    pcull_ret_p1 = p1 + 7;
    pcull_ret_p2 = p2 + 7;
    pcull_skip_p1 = b + r_n1;
    pcull_skip_p2 = b + r_n2;
    pcull_jmp(l, 6, (void *)pcull_stub_lod);
    pcull_jmp(p1, 7, (void *)pcull_stub_p1);
    pcull_jmp(p2, 7, (void *)pcull_stub_p2);
    done = 1;
    {   // the probe's switch, beside the game's own exe
        if (GetModuleFileNameW(NULL, pcull_probe_path, MAX_PATH)) {
            wchar_t *sl = wcsrchr(pcull_probe_path, (wchar_t)92);
            if (sl) {
                wcscpy(sl + 1, L"partcull_probe.txt");
                if (GetFileAttributesW(pcull_probe_path) != INVALID_FILE_ATTRIBUTES) {
                    wcscpy(sl + 1, L"partcull_probe.log");
                    DeleteFileW(pcull_probe_path);
                    InterlockedExchange(&pcull_probe_on, 1);
                    log("unit detail cull: probe on, the first %d culled parts over 0.45 m and within %.0f m go to"
                        " partcull_probe.log", PC_PROBE_N, PC_PROBE_MAX_DIST);
                }
            }
        }
    }
    log("unit detail cull on: soldier parts under %.0f px tall (helmets and heads %.0f px) are not drawn",
        g_partcull_px, g_partcull_head_px);
    return 1;
}
