// Small-block heap that uses address space like the game's own (6.8.0, AGENT_LOG 143). The game's heap is dlmalloc with
// one lock (6.7.0 replaced it with mimalloc for speed): offline, 600 MB of game-sized blocks took 608 MB of address
// space on the game's heap, grown in 64 KB pieces with nothing reserved up front, and 704 MB on 6.7.0's mimalloc after
// a 512 MB reservation at start. Here: several dlmalloc 2.8.6 mspaces (Doug Lea, public domain / CC0) with the game's
// own settings (64 KB granularity, 2 MB trim threshold), each with its own lock, threads spread over them, and a small
// per-thread cache of freed blocks so the common free / malloc pairs take no lock at all.
// Blocks carry their mspace in a footer (FOOTERS), so a block freed on any thread goes back to the mspace it came from.
#include <windows.h>
#include <stdint.h>
#include <string.h>

// ---- dlmalloc, built for this
#define ONLY_MSPACES 1
#define MSPACES 1
#ifdef POOLS_SRW   // test switch (wip/allocbench): an SRW lock, waiting in the kernel, instead of dlmalloc's spin-and-yield
#define USE_LOCKS 2
#define MLOCK_T SRWLOCK
#define INITIAL_LOCK(l) (InitializeSRWLock(l), 0)
#define DESTROY_LOCK(l) (0)
#define ACQUIRE_LOCK(l) (AcquireSRWLockExclusive(l), 0)
#define RELEASE_LOCK(l) ReleaseSRWLockExclusive(l)
#define TRY_LOCK(l) TryAcquireSRWLockExclusive(l)
static MLOCK_T malloc_global_mutex = SRWLOCK_INIT;
#else
#define USE_LOCKS 1
#endif
#define FOOTERS 1
#define HAVE_MORECORE 0
#define DEFAULT_GRANULARITY ((size_t)64U * (size_t)1024U)              // the game's (read from its mparams)
// automatic trim only once 64 MB at the top is free (a battle unloading), not at the game's 2 MB: giving memory back on
// frees took 1-4 ms per call, stutter that 6.2's heap did not have (AGENT_LOG 169-170, wip/heap_vanilla/tools/
// hitchbench.c); no periodic scan for unused segments; the game's own trim still gives memory back (pool_trim)
#define DEFAULT_TRIM_THRESHOLD ((size_t)64U << 20)
#define MAX_RELEASE_CHECK_RATE MAX_SIZE_T
#define DEFAULT_MMAP_THRESHOLD ((size_t)256U * (size_t)1024U)        // never reached: only blocks <= 16 KB come here
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define MALLOC_FAILURE_ACTION
#ifdef POOLS_DEBUG   // tests only: dlmalloc checks its structures on every call and aborts on any inconsistency
#define DEBUG 1
#endif
#undef FORCEINLINE   // windows.h makes it extern inline, which dlmalloc's static functions cannot take
#define FORCEINLINE __inline__ __attribute__((always_inline))
static void *pool_mmap(size_t s);
static int pool_munmap(void *a, size_t s);
#define MMAP(s) pool_mmap(s)
#define DIRECT_MMAP(s) pool_mmap(s)
#define MUNMAP(a, s) pool_munmap((a), (s))
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wexpansion-to-defined"
#pragma GCC diagnostic ignored "-Wnull-pointer-arithmetic"
#include "dlmalloc/malloc-2.8.6.c"
#pragma GCC diagnostic pop

// ---- which 64 KB of the address space are the pools' (Windows hands memory out in 64 KB units): one byte each, so a
// free can tell a pool block from one of the game's heap without a lock
static volatile unsigned char g_own[65536];
static volatile LONG g_pool_kb;   // address space the pools hold, KB

static void own_mark(void *a, size_t s, unsigned char v) {
    for (UINT_PTR g = (UINT_PTR)a >> 16, e = ((UINT_PTR)a + s - 1) >> 16; g <= e; g++) g_own[g] = v;
}

static void *pool_mmap(size_t s) {   // as dlmalloc's own win32mmap, and the pieces marked
    void *p = VirtualAlloc(NULL, s, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return MFAIL;
    own_mark(p, s, 1);
    InterlockedExchangeAdd(&g_pool_kb, (LONG)(s >> 10));
    return p;
}

static int pool_munmap(void *a, size_t s) {   // as dlmalloc's win32munmap: the range can span several allocations
    own_mark(a, s, 0);
    InterlockedExchangeAdd(&g_pool_kb, -(LONG)(s >> 10));
    MEMORY_BASIC_INFORMATION m;
    char *p = a;
    while (s) {
        if (VirtualQuery(p, &m, sizeof m) == 0 || m.BaseAddress != p || m.AllocationBase != p || m.State != MEM_COMMIT ||
            m.RegionSize > s)
            return -1;
        if (VirtualFree(p, 0, MEM_RELEASE) == 0) return -1;
        p += m.RegionSize;
        s -= m.RegionSize;
    }
    return 0;
}

int pool_owns(const void *p) { return g_own[(UINT_PTR)p >> 16]; }
unsigned pool_mb(void) { return (unsigned)(g_pool_kb >> 10); }

// ---- the mspaces and the threads' share of them
#define NPOOL 8
static mspace g_pool[NPOOL];
static volatile LONG g_next;
static DWORD g_tls = TLS_OUT_OF_INDEXES;

// Per-thread cache: freed blocks of up to 2 KB by size class (16-byte steps), at most CACHE_N per class and CACHE_BYTES
// in all, so a thread holds a bounded amount (the address space a cache keeps is the cost; 256 KB a thread).
#ifndef CACHE_MAX   // test switch (wip/allocbench): -DCACHE_MAX=16384
#define CACHE_MAX 2048
#endif
#define NCLASS (CACHE_MAX / 16 + 1)
#define CACHE_N 32
#define CACHE_BYTES (256u << 10)
typedef struct { int pool; unsigned bytes; void *bin[NCLASS]; unsigned char n[NCLASS]; } Cache;

// TlsGetValue / TlsSetValue set the thread's last error to 0: the game's malloc / free must leave it as the game left it
// (its own dlmalloc did; ultrareview 2026-11-02), as the MSVC CRT does around its own TLS reads
static Cache *tls_cache(void) {
    DWORD err = GetLastError();
    Cache *c = TlsGetValue(g_tls);
    SetLastError(err);
    return c;
}
static Cache *cache(void) {
    Cache *c = tls_cache();
    if (c) return c;
    DWORD err = GetLastError();
    c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *c);
    if (c) {
        c->pool = (int)((unsigned)InterlockedIncrement(&g_next) % NPOOL);
        TlsSetValue(g_tls, c);
    }
    SetLastError(err);
    return c;
}

#ifdef POOLS_REMOTE
// test switch (wip/allocbench, AGENT_LOG 199): a block freed by a thread of another pool is pushed on that pool's
// lock-free list instead of taking its lock (its own threads' mallocs waited on it: the slow frames, AGENT_LOG 198);
// the next thread of that pool that takes the lock anyway frees the list in one bulk free. Pushes only from other
// threads, the whole list taken at once: no ABA.
static void *volatile g_remote[NPOOL];
static void remote_push(int i, void *p) {
    void *h;
    do { h = g_remote[i]; *(void **)p = h; } while (InterlockedCompareExchangePointer(&g_remote[i], p, h) != h);
}
static void remote_drain(int i) {
    void *p = InterlockedExchangePointer(&g_remote[i], NULL), *batch[64];
    size_t n = 0;
    while (p) {
        void *next = *(void **)p;   // before the block is freed
        batch[n++] = p;
        if (n == 64) { mspace_bulk_free(g_pool[i], batch, n); n = 0; }
        p = next;
    }
    if (n) mspace_bulk_free(g_pool[i], batch, n);
}
static int pool_of(void *p) {   // FOOTERS: the block's mspace, as an index
    mstate m = get_mstate_for(mem2chunk(p));
    for (int i = 0; i < NPOOL; i++) if ((mstate)g_pool[i] == m) return i;
    return -1;
}
#endif

int pools_init(void) {
    if ((g_tls = TlsAlloc()) == TLS_OUT_OF_INDEXES) return 0;
    for (int i = 0; i < NPOOL; i++)
        if (!(g_pool[i] = create_mspace(0, 1))) return 0;
    return 1;
}

void *pool_alloc(size_t n) {
    Cache *c = cache();
    if (!c) return mspace_malloc(g_pool[0], n);
    if (n <= CACHE_MAX) {
        unsigned k = (unsigned)(n + 15) >> 4;   // class k holds blocks with at least k * 16 usable bytes
        if (k == 0) k = 1;
        void *p = c->bin[k];
#ifdef POOLS_FIT   // test switch (AGENT_LOG 199): a cached block up to 12.5 % larger rather than a locked malloc
        for (unsigned j = k + 1, top = k + k / 8 < NCLASS ? k + k / 8 : NCLASS - 1; !p && j <= top; j++)
            if (c->bin[j]) { k = j; p = c->bin[j]; }
#endif
        if (p) {
            c->bin[k] = *(void **)p;
            c->n[k]--;
            c->bytes -= (unsigned)mspace_usable_size(p);
            return p;
        }
#ifdef POOLS_REMOTE
        if (g_remote[c->pool]) remote_drain(c->pool);
#endif
        return mspace_malloc(g_pool[c->pool], (size_t)k << 4);
    }
#ifdef POOLS_REMOTE
    if (g_remote[c->pool]) remote_drain(c->pool);
#endif
    return mspace_malloc(g_pool[c->pool], n);
}

void pool_free(void *p) {   // p is a pool block (pool_owns)
    size_t u = mspace_usable_size(p);
    Cache *c = u <= CACHE_MAX + 15 ? cache() : NULL;
    if (c) {
        unsigned k = (unsigned)(u >> 4);
        if (k > NCLASS - 1) k = NCLASS - 1;
        if (k && c->n[k] < CACHE_N && c->bytes + u <= CACHE_BYTES) {
            *(void **)p = c->bin[k];
            c->bin[k] = p;
            c->n[k]++;
            c->bytes += (unsigned)u;
            return;
        }
    }
#ifdef POOLS_REMOTE
    if (c || (c = cache())) {
        int i = pool_of(p);
        if (i >= 0 && i != c->pool) { remote_push(i, p); return; }   // another pool's block: no lock
        if (i >= 0 && g_remote[i]) remote_drain(i);
    }
#endif
    mspace_free(g_pool[0], p);   // FOOTERS: the block's own mspace, whichever is passed
}

void *pool_realloc(void *p, size_t n) { return mspace_realloc(g_pool[0], p, n); }   // FOOTERS: the block's own mspace
size_t pool_usable(const void *p) { return mspace_usable_size(p); }

// On the game's own trim (dlmalloc_trim): this thread's cache back to its pools, then each pool gives back the 64 KB
// pieces that are wholly free (dlmalloc's release of unused segments), as the game's heap does on its trim.
void pool_trim(void) {
    Cache *c = g_tls == TLS_OUT_OF_INDEXES ? NULL : tls_cache();
    if (c) {
        for (unsigned k = 0; k < NCLASS; k++) {
            for (void *p = c->bin[k], *q; p; p = q) {
                q = *(void **)p;
                mspace_free(g_pool[0], p);
            }
            c->bin[k] = NULL;
            c->n[k] = 0;
        }
        c->bytes = 0;
    }
#ifdef POOLS_REMOTE
    for (int i = 0; i < NPOOL; i++)
        if (g_pool[i]) remote_drain(i);
#endif
    for (int i = 0; i < NPOOL; i++)
        if (g_pool[i]) mspace_trim(g_pool[i], 0);
}

void pool_thread_exit(void) {   // the thread's cached blocks go back to their mspaces
    if (g_tls == TLS_OUT_OF_INDEXES) return;
    Cache *c = tls_cache();
    if (!c) return;
    DWORD err = GetLastError();
    TlsSetValue(g_tls, NULL);
    SetLastError(err);
    for (unsigned k = 0; k < NCLASS; k++)
        for (void *p = c->bin[k], *q; p; p = q) {
            q = *(void **)p;
            mspace_free(g_pool[0], p);
        }
    HeapFree(GetProcessHeap(), 0, c);
}
