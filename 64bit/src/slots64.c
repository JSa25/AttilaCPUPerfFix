// slots64: the engine's C library calls that are worth replacing, through its own function-pointer slots.
// The game file reaches every library function through a pointer slot (.got.plt) that the loader fills in before any
// game code runs. Under Sol's loader:
//   malloc / free / realloc  -> a shim that calls the Windows C runtime heap. CA::custom_malloc (0x525f10), which
//                               nearly all of the engine allocates through, is a plain call of malloc: the Linux
//                               build has no pools of its own, so every allocation of every thread is one heap call;
//   operator new / new[]     -> libstdc++, which calls malloc the same way;
//   strlen, memcmp           -> loops that handle one byte at a time (loader RVA 0x510a0, 0x510f0).
// Two independent parts, each behind its own switch:
//   heap:    the x86 mod's allocator fix. malloc, free, realloc, operator new, new[], new(nothrow) and delete[] go to
//            mimalloc. Blocks made before this is installed, or by code that does not go through these slots (the
//            loader, libstdc++ itself), are told apart by mi_is_in_heap_region and go back to where they came from.
//   strings: strlen and memcmp go to the Windows C runtime's (vectorised) ones.
// memcpy, memmove and memset are left alone: the loader's own are SSE routines with small-size fast paths, which is
// what the x86 mod's small-copy fix (memfast.c) adds there.
// Slot addresses: R_X86_64_JUMP_SLOT entries of the game file (slots64_test checks each against the file).
#include <string.h>
#include <mimalloc.h>
#include "perffix64.h"

#define SV __attribute__((sysv_abi))
enum { S_MALLOC, S_FREE, S_REALLOC, S_NEW, S_NEWA, S_NEWNT, S_DELA, S_STRLEN, S_MEMCMP, S_COUNT };
const struct slot64 { uint32_t addr; const char *sym; } SLOTS64[S_COUNT] = {
    {0x4bf0548, "malloc"}, {0x4bf0a00, "free"}, {0x4bf0020, "realloc"}, {0x4bf09f8, "_Znwm"}, {0x4bf0aa8, "_Znam"},
    {0x4bf08f0, "_ZnwmRKSt9nothrow_t"}, {0x4bf06d8, "_ZdaPv"}, {0x4bf0700, "strlen"}, {0x4bf02d8, "memcmp"},
};
void *slots64_orig[S_COUNT];   // what each slot held: the loader's shim, or libstdc++'s function
volatile LONG64 g_heap64[3];   // allocations served, frees of blocks that were not ours, operator new fallbacks

typedef SV void *(*malloc_fn)(size_t);
typedef SV void (*free_fn)(void *);
typedef SV void *(*realloc_fn)(void *, size_t);
typedef SV void *(*newnt_fn)(size_t, const void *);

SV void *h64_malloc(size_t n) {
    void *p = mi_malloc(n);
    if (p) { InterlockedIncrement64(&g_heap64[0]); return p; }
    return ((malloc_fn)slots64_orig[S_MALLOC])(n);   // out of memory in mimalloc: the heap the game had before
}
SV void h64_free(void *p) {
    if (!p) return;
    if (mi_is_in_heap_region(p)) { mi_free(p); return; }
    InterlockedIncrement64(&g_heap64[1]);
    ((free_fn)slots64_orig[S_FREE])(p);
}
SV void *h64_realloc(void *p, size_t n) {
    if (!p) return h64_malloc(n);
    if (mi_is_in_heap_region(p)) return mi_realloc(p, n);
    return ((realloc_fn)slots64_orig[S_REALLOC])(p, n);   // a block of the other heap stays there
}
// operator new must throw std::bad_alloc when there is no memory: only libstdc++'s own can, so that case goes to it
SV void *h64_new(size_t n) {
    void *p = mi_malloc(n ? n : 1);
    if (p) { InterlockedIncrement64(&g_heap64[0]); return p; }
    InterlockedIncrement64(&g_heap64[2]);
    return ((malloc_fn)slots64_orig[S_NEW])(n);
}
SV void *h64_newa(size_t n) {
    void *p = mi_malloc(n ? n : 1);
    if (p) { InterlockedIncrement64(&g_heap64[0]); return p; }
    InterlockedIncrement64(&g_heap64[2]);
    return ((malloc_fn)slots64_orig[S_NEWA])(n);
}
SV void *h64_newnt(size_t n, const void *tag) {
    void *p = mi_malloc(n ? n : 1);
    if (p) { InterlockedIncrement64(&g_heap64[0]); return p; }
    return ((newnt_fn)slots64_orig[S_NEWNT])(n, tag);
}
SV void h64_dela(void *p) {
    if (!p) return;
    if (mi_is_in_heap_region(p)) { mi_free(p); return; }
    InterlockedIncrement64(&g_heap64[1]);
    ((free_fn)slots64_orig[S_DELA])(p);
}
SV size_t s64_strlen(const char *s) { return strlen(s); }
SV int s64_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }

static void *const REPL[S_COUNT] = {(void *)h64_malloc, (void *)h64_free, (void *)h64_realloc, (void *)h64_new, (void *)h64_newa,
                                    (void *)h64_newnt, (void *)h64_dela, (void *)s64_strlen, (void *)s64_memcmp};

// The loader itself (and libstdc++ through the loader's shim) also frees and resizes memory, some of it handed over by
// the game: attila64.exe's own imports of free and realloc get the same test, so a block of ours that ends up there is
// still freed by mimalloc. Windows calling convention: these are the exe's C runtime imports.
static void (__cdecl *exe_free)(void *);
static void *(__cdecl *exe_realloc)(void *, size_t);
void __cdecl x64_free(void *p) {
    if (p && mi_is_in_heap_region(p)) { mi_free(p); return; }
    exe_free(p);
}
static size_t (__cdecl *exe_msize)(void *);
size_t __cdecl x64_msize(void *p) { return p && mi_is_in_heap_region(p) ? mi_usable_size(p) : exe_msize(p); }
void *__cdecl x64_realloc(void *p, size_t n) {
    if (p && mi_is_in_heap_region(p)) return mi_realloc(p, n);
    return exe_realloc(p, n);
}

#ifndef SLOTS64_TEST
void **import_slot64(HMODULE m, const char *dll, const char *name);   // d3d64.c
static int exe_hooks(void) {
    HMODULE exe = GetModuleHandleW(NULL);
    void **f = import_slot64(exe, "api-ms-win-crt-heap-l1-1-0.dll", "free"), **r = import_slot64(exe, "api-ms-win-crt-heap-l1-1-0.dll", "realloc");
    DWORD o1, o2;
    if (!f || !r || !VirtualProtect(f, 8, PAGE_READWRITE, &o1)) return 0;
    if (!VirtualProtect(r, 8, PAGE_READWRITE, &o2)) { VirtualProtect(f, 8, o1, &o1); return 0; }
    exe_free = (void (__cdecl *)(void *))*f; exe_realloc = (void *(__cdecl *)(void *, size_t))*r;
    *f = (void *)x64_free; *r = (void *)x64_realloc;
    VirtualProtect(r, 8, o2, &o2); VirtualProtect(f, 8, o1, &o1);
    void **ms = import_slot64(exe, "api-ms-win-crt-heap-l1-1-0.dll", "_msize");   // the loader's big-block statistics ask a block's size
    if (ms && VirtualProtect(ms, 8, PAGE_READWRITE, &o1)) { exe_msize = (size_t (__cdecl *)(void *))*ms; *ms = (void *)x64_msize; VirtualProtect(ms, 8, o1, &o1); }
    return 1;
}
// A slot is taken only if the loader has filled it in: it then points outside the game image.
static int resolved(int i) {
    uintptr_t v = *(uintptr_t *)(uintptr_t)SLOTS64[i].addr;
    return v && (v < 0x400000 || v >= 0x5300000);
}
// heap / strings: which part to install. Returns bit 0 = heap in, bit 1 = strings in. All seven heap slots go
// together or not at all (a block must never be freed by a heap that did not make it).
int install_slots64(int heap, int strings) {
    int got = 0;
    if (heap) {
        int ok = 1;
        for (int i = S_MALLOC; i <= S_DELA; i++) ok &= resolved(i);
        if (ok) mi_process_init();
        if (ok && !exe_hooks()) ok = 0;   // without them a block of ours freed inside the loader would corrupt its heap
        if (ok) {
            for (int i = S_MALLOC; i <= S_DELA; i++) slots64_orig[i] = *(void **)(uintptr_t)SLOTS64[i].addr;
            for (int i = S_DELA; i >= S_MALLOC; i--) *(void **)(uintptr_t)SLOTS64[i].addr = REPL[i];   // malloc last: free and realloc know both heaps first
            got |= 1;
        }
    }
    if (strings)
        for (int i = S_STRLEN; i <= S_MEMCMP; i++)
            if (resolved(i)) { slots64_orig[i] = *(void **)(uintptr_t)SLOTS64[i].addr; *(void **)(uintptr_t)SLOTS64[i].addr = REPL[i]; got |= 2; }
    return got;
}
#endif
