// Shared by the fixes of attila64_perffix (proxy SDL2.dll for the Linux x64 engine, SHA-256 217e1435...8784).
#pragma once
#include <windows.h>
#include <stdint.h>

// The stubs the game's 5-byte jumps reach: one block of LOW_SIZE bytes below 2 GB (perffix64.c allocates it), by part.
// Each part's room is fixed here so that adding sites to one part cannot run into the next.
enum { LOW_BATCH = 0x0000,       // 32 x 64 bytes
       LOW_PARTCULL = 0x0800,    // 48 bytes
       LOW_SOUNDGUARD = 0x0900,  // 48 bytes
       LOW_SCRIPTEXT = 0x0a00,   // 96 bytes
       LOW_BRIDGE = 0x0b00,      // 128 bytes: the upscaling add-on's two entry hooks and its record-copy relay
       LOW_LOOPFETCH = 0x1000,   // 64 x 64 bytes
       LOW_SIZE = 0x2000 };
enum { BATCH_MAX = (LOW_PARTCULL - LOW_BATCH) / 64, LOOPFETCH_MAX = (LOW_SIZE - LOW_LOOPFETCH) / 64 };

// Every install_* runs while all other threads are paused: no allocation, no CRT, no logging there.
int thread_inside(uint64_t start, uint64_t len);   // 1 if a paused thread's rip is in [start, start+len)
int patch_code(void *at, const void *bytes, size_t n);   // VirtualProtect + copy + restore; 1 on success
uint32_t fnv1a(const void *p, size_t n);

struct x87_result { int sites, patched, kept, why; };    // why: 0 ok, 1 no ELF, 2 no SSE3, 3 rounding mode, 4 other build
void install_x87fix(struct x87_result *r);
int install_texfix64(void);   // 1 replaced, 0 bytes differ, -1 a thread inside the prologue, -2 VirtualProtect failed
extern volatile LONG64 g_tf_calls, g_tf_blocks;   // texfix64.c
int install_pool64(void);      // same codes as install_texfix64
void pool64_thread_exit(void);
// What holds a site the loader may also patch, after the install: its bytes are neither the game's nor a known patch
// of the loader's (DIFFERS), our patch (OURS), the loader's own patch of the same thing (LOADER), a paused thread was
// inside it (BUSY), or the write failed (FAILED).
enum { SITE_DIFFERS, SITE_OURS, SITE_LOADER, SITE_BUSY, SITE_FAILED };
int install_sortfix64(unsigned char *tramp);   // tramp: 5 x 32 bytes of executable memory; returns sorts replaced
extern int g_sort_state[5];                    // SITE_* per sort
extern const char *const g_sort_label[5];
void sortfix64_thread_exit(void);
int install_batchfix64(unsigned char *code);   // code: 4 KB executable below 2 GB; returns loops patched, -1 no memory
extern int g_batch_state[];                    // SITE_* per loop (batchfix64_count of them)
extern const char *const g_batch_label[];
extern const int batchfix64_count;
int install_heightcache64(unsigned char *tramp, int mode);   // tramp 96 bytes; mode 1 verify, 2 cache
extern volatile LONG64 g_hq[4];   // calls, engine-thread calls, hits, mismatches
extern volatile LONG g_hq_clears;
int install_partcull64(unsigned char *code);     // code: 48 bytes near the engine; 1 on, 0 bytes differ, -1 thread inside / no near memory, -2 VirtualProtect
void partcull64_counts(unsigned *parts, unsigned *skipped, unsigned *forced);
extern volatile LONG g_partcull_on;
extern float g_partcull_px, g_partcull_head_px;
int install_uikeep64(void);                      // same codes
extern volatile LONG g_uk[4];                    // kept, kept again, deleted as oldest, already gone
extern volatile unsigned char g_uikeep_on;
extern unsigned g_uikeep_keep;
int install_loopfetch64(unsigned char *code);    // code: loopfetch64_count x 64 bytes near the engine; returns loops patched, -1 no near memory
extern const int loopfetch64_count;
int install_soundguard64(unsigned char *code);   // code: 48 bytes near the engine; same codes as partcull
extern volatile LONG g_soundguard_skips;
int flagwave64_pack_switch(const wchar_t *home, char *who, int wn);   // flagwave64.c: 1 a loaded pack says on, 0 off, -1 none has the file
int flagwave64_setup(const wchar_t *home);   // flagwave64.c (generated): 1 = shaders compiled
void flagwave64_device(void *dev, void *ctx);   // ctx: the context the game was given (the add-on's wrapper, or the real one)
void flagwave64_frame(void *swapchain);
// loader64.c
#define LOADER64_CHECKED 1, 3, 13, 0             // the newest loader the fix was checked with (binary 1.3.13; its published source: 1.3.7)
#define LOADER64_CHECKED_STR "1.3.13"
int loader64_version(HMODULE exe, unsigned v[4]);
int loader64_compare(const unsigned v[4]);
void loader64_site_notes(char *out, size_t cap, const int *state, const char *const *label, int count, const char *loader_what);
int install_d3d64(const wchar_t *dir, void (*log)(const char *fmt, ...));   // d3d64.c: 1 = attila64.exe's D3D11CreateDevice import hooked
int scriptext64_prepare(const wchar_t *dir);     // before the pause: 1 = the lua_ext folder exists in dir
int install_scriptext64(unsigned char *code);    // code: 96 bytes near the engine; 1 on, 0 not wanted, -1 not installed
void scriptext64_frame(void);
void panel64_frame(void *swapchain);                 // panel64.c: the F8 panel over the back buffer
void panel64_setup(void (*log)(const char *fmt, ...));
void fgp_set_cull(int choice), fgp_set_cull_available(int ok);   // heapfix/fgpanel.c
// bridge64 (the upscaling add-on's game side, out/gen/bridge64.c + src/bridge64_tail.inc)
int bridge64_prepare(const wchar_t *home, void (*log)(const char *fmt, ...));   // before the pause: 1 = installed and not switched off
int install_bridge64(unsigned char *code);           // paused: bit 0 stage hook, 1 camera hook, 2 record copies, 3 view classes as expected
void bridge64_installed(int r);                      // after the pause: logs; the add-on stays off without the hooks
int bridge64_owns_panel(void);                       // 1: the add-on's F8 panel replaces panel64's
int install_uicache64(void);                         // uicache64.c: 1, 0 code differs, -1 thread inside, -2 VirtualProtect
void uicache64_setup(void (*log)(const char *fmt, ...));
void uicache64_probe(const wchar_t *home);          // uicache64_probe.inc: on with <home>\uicache_probe.txt
void uicache64_frame(void *swapchain);              // uicache64_probe.inc: before each Present (the probe's frame sequence)
void uicache64_counts(unsigned *reuse, unsigned *redraw);
int install_slots64(int heap, int strings);          // slots64.c: bit 0 = allocator in, bit 1 = string functions in
extern volatile LONG64 g_heap64[3];                  // allocations served, frees of blocks that were not ours, operator new fallbacks
