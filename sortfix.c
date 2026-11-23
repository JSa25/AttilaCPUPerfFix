// Faster replacements for two render-list sorts that battle jobs run while the main thread waits.
// FUN_117ad5d0 and FUN_117acb50 are std::sort (introsort) instantiations over pointers to render items, ordered
// ascending by two unsigned keys read through the pointers: +0x6C then +0x68 (the battle draw list) and +0x4C then
// +0x48. Every comparison reads both keys through the pointers, so in big battles they miss the cache constantly, and
// the main thread waits for them. This reads each item's keys once into a compact array, radix-sorts that (stable:
// equal keys keep their input order; the originals left their order unspecified) and writes the pointers back.
#include <windows.h>
#include <string.h>
#include "gamerva.h"
#include "patch.h"

#define RVA_DRAWSORT 0x17AD5D0  // VA 0x117AD5D0 at the preferred base 0x10000000
#define DRAWSORT_LEN 630

struct kp { unsigned long long key; void *p; };

static __thread struct kp *t_a, *t_b;
static __thread unsigned t_cap;

typedef void(__cdecl *sort_fn)(void **, void **, int, void *);

static struct sort_site {
    DWORD rva, fnv, fnv_gog;   // FNV-1a of the whole 630-byte function in the 2026-09-19 build (no relocations); GOG 6A4F4723:
                               // the same code, one call target moved (wip/gog)
    unsigned hi, lo;           // key offsets: compared as (hi, lo)
    const char *name;
    unsigned char tramp[16];   // the overwritten prologue + jmp back: the original sort
} g_sorts[2] = {
    {0x17AD5D0, 0xD811EF5Au, 0x3A3DA72Au, 0x6C, 0x68, "battle draw-list sort", {0}},
    {0x17ACB50, 0xC33528C2u, 0x1193F5A9u, 0x4C, 0x48, "render-list sort (+0x4C/+0x48)", {0}},
};

static unsigned long long key_at(const void *p, const struct sort_site *s) {
    const unsigned char *o = p;
    return ((unsigned long long)*(const unsigned *)(o + s->hi) << 32) | *(const unsigned *)(o + s->lo);
}

// The draw-list key, kept for the tests.
static __attribute__((unused)) unsigned long long key_of(void *p) { return key_at(p, &g_sorts[0]); }

static int grow(unsigned n) {
    if (n <= t_cap) return 1;
    unsigned cap = n + n / 2 + 64;
    HANDLE h = GetProcessHeap();
    void *a = t_a ? HeapReAlloc(h, 0, t_a, cap * sizeof *t_a) : HeapAlloc(h, 0, cap * sizeof *t_a);
    if (!a) return 0;
    t_a = a;
    void *b = t_b ? HeapReAlloc(h, 0, t_b, cap * sizeof *t_b) : HeapAlloc(h, 0, cap * sizeof *t_b);
    if (!b) return 0;
    t_b = b;
    t_cap = cap;
    return 1;
}

static void sort_by_keys(void **first, void **last, int ideal, void *ctx, const struct sort_site *s) {
    unsigned n = (unsigned)(last - first);
    if (n < 2) return;
    if (n <= 32) {  // small: insertion sort (stable)
        for (unsigned i = 1; i < n; i++) {
            void *v = first[i];
            unsigned long long k = key_at(v, s);
            unsigned j = i;
            while (j && key_at(first[j - 1], s) > k) { first[j] = first[j - 1]; j--; }
            first[j] = v;
        }
        return;
    }
    if (!grow(n)) {
        ((sort_fn)(void *)s->tramp)(first, last, ideal, ctx);
        return;
    }
    struct kp *a = t_a, *b = t_b;
    unsigned hist[8][256];
    memset(hist, 0, sizeof hist);
    for (unsigned i = 0; i < n; i++) {
        unsigned long long k = key_at(first[i], s);
        a[i].key = k;
        a[i].p = first[i];
        for (int d = 0; d < 8; d++) hist[d][(k >> (8 * d)) & 0xFF]++;
    }
    for (int d = 0; d < 8; d++) {
        unsigned *h = hist[d];
        if (h[(a[0].key >> (8 * d)) & 0xFF] == n) continue;  // every key has the same digit here
        unsigned sum = 0;
        for (int i = 0; i < 256; i++) { unsigned c = h[i]; h[i] = sum; sum += c; }
        for (unsigned i = 0; i < n; i++) b[h[(a[i].key >> (8 * d)) & 0xFF]++] = a[i];
        struct kp *t = a; a = b; b = t;
    }
    for (unsigned i = 0; i < n; i++) first[i] = a[i].p;
}

// Set by the list job's hook (below) on its own thread while the game's loop runs: every soldier list was merged and
// sorted before the loop started, so the loop's own sort calls have nothing to do.
static __thread int t_presorted;

void __cdecl draw_sort(void **first, void **last, int ideal, void *ctx) {
    if (!t_presorted) sort_by_keys(first, last, ideal, ctx, &g_sorts[0]);
}
void __cdecl list_sort(void **first, void **last, int ideal, void *ctx) { sort_by_keys(first, last, ideal, ctx, &g_sorts[1]); }

static int g_draw_sort_in;   // the draw-list sort is ours: what the list job's hook needs

// Returns how many of the two sorts were redirected (each is independent: either one alone is correct).
int install_sortfix(HMODULE m, void (*log)(const char *fmt, ...)) {
    void *repl[2] = {(void *)draw_sort, (void *)list_sort};
    int n = 0;
    for (int i = 0; i < 2; i++) {
        struct sort_site *s = &g_sorts[i];
        unsigned char *site = (unsigned char *)m + game_rva(m, s->rva);
        DWORD h = fnv1a(site, DRAWSORT_LEN);
        if (h != s->fnv && h != s->fnv_gog) {
            log("%s at +0x%X does not match this game version - not patched", s->name, s->rva);
            continue;
        }
        // Trampoline = the first 5 bytes (sub esp,14h; push ebx; push ebp) + jmp to site+5.
        DWORD old;
        if (!VirtualProtect(s->tramp, sizeof s->tramp, PAGE_EXECUTE_READWRITE, &old)) continue;
        patch_tramp(s->tramp, site, 5);
        if (!patch_jump(site, 5, repl[i])) {
            log("%s: VirtualProtect failed (%lu) - not patched", s->name, GetLastError());
            continue;
        }
        log("%s replaced (key extraction + radix sort)", s->name);
        if (i == 0) g_draw_sort_in = 1;
        n++;
    }
    return n;
}

// ---- The soldiers' draw lists, sorted on several threads (AGENT_LOG 238, PERF_DEEP_DIVE.md B1) ----
// Every frame the main thread waits (in FUN_1186be70) for one job that merges and sorts the soldiers' draw lists:
// FUN_117b92d0 walks 23 pass lists x every soldier mesh, and for each list merges the per-thread vectors, sorts
// (draw_sort above) and appends the items to the mesh's master list - all on one thread, about 2.6 ms in a big melee,
// 2 ms of it the sorts, while the other workers idle. Here the job first merges every list itself (the game's own merge,
// on the job's thread as before), then the lists are sorted by this thread and up to three helper threads of ours, and
// then the game's loop runs unchanged: its merges find nothing to do, its sort calls return at once (t_presorted), and
// it appends in its own order. A list's content after merge + sort depends on that list alone, so the result is the same.
// The helpers run nothing but sort_by_keys: no game code, no game allocator.
#define RVA_LISTJOB 0x17B92D0        // FUN_117b92d0(node, ctx), stdcall: the job's loop
#define RVA_SOLDIER_STUB 0x186BDC0   // soldier meshes' vtable +0x130: list = mesh + 0x274 + pass * 0x20, then merge + sort + append
#define RVA_LISTMERGE 0x178FD90      // thiscall(list): per-thread vectors -> the list's own vector (count +8, array +0xC)
#define LIST_PASSES 23
#define LIST_FIRST 0x274
#define LIST_SIZE 0x20

struct sort_task { void **first; unsigned n; };
static struct sort_task *volatile g_tasks;
static unsigned g_tasks_cap;
static volatile LONG g_task_next, g_task_count, g_task_pending, g_task_inloop, g_par_busy;
static HANDLE g_par_sem;
static int g_par_threads;                     // helper threads running
int g_parsort_helpers = -1;                   // helpers to use: -1 = by the CPU (set at install); the test sets it
unsigned g_parsort_min = 8192;                // fewer items than this in a frame: not worth waking anyone
volatile unsigned char g_parsort_on = 1;      // off: the game's loop does it all (the A/B below, and the test)
static int g_parsort_ab;                      // parsort_ab.txt (FPS test): on in even 10 s windows of UTC time, off in odd (the same
                                              // windows as flagwave_ab.txt and the dev A/B switches), the job's time both ways to the log
volatile LONG g_parsort_jobs;                 // jobs whose lists were sorted here (the test reads it)
static void *g_soldier_stub;
static void(__thiscall *g_list_merge)(void *list);
static unsigned char g_job_tramp[16];
static void (*g_par_log)(const char *fmt, ...);

// Takes tasks until none is left. A task is taken with a compare-and-swap on the next index, so a thread that read an
// old index can only take a task that is still free; the job's thread waits for g_task_inloop == 0 before it hands out
// the next frame's tasks, so no thread checks an index against one frame's count and takes it in another.
static void run_sort_tasks(void) {
    for (;;) {
        LONG i = g_task_next;
        if (i >= g_task_count) return;
        if (InterlockedCompareExchange(&g_task_next, i + 1, i) != i) continue;
        struct sort_task *t = &g_tasks[i];
        sort_by_keys(t->first, t->first + t->n, (int)t->n, NULL, &g_sorts[0]);
        InterlockedDecrement(&g_task_pending);
    }
}

static DWORD WINAPI sort_helper(void *x) {
    (void)x;
    for (;;) {
        WaitForSingleObject(g_par_sem, INFINITE);
        InterlockedIncrement(&g_task_inloop);
        run_sort_tasks();
        InterlockedDecrement(&g_task_inloop);
    }
}

// 1: every soldier list of this job is merged and sorted. 0: nothing was sorted here (another instance type's job, a
// small frame, no helpers, or another soldier job is using the helpers) and the game's loop does it all, as before.
static int sort_lists_parallel(unsigned char *node) {
    g_list_merge(node);   // the node's own per-thread mesh lists; the game's loop calls this again and finds it done
    unsigned meshes = *(unsigned *)(node + 8);
    void **mesh = *(void ***)(node + 0xC);
    if (!meshes || (*(void ***)mesh[0])[0x130 / 4] != g_soldier_stub) return 0;
    if (g_parsort_helpers <= 0 || InterlockedCompareExchange(&g_par_busy, 1, 0)) return 0;
    int done = 0;
    unsigned need = meshes * LIST_PASSES, n = 0, items = 0;
    if (need > g_tasks_cap) {   // only this thread, and only while no task is handed out
        void *t = g_tasks ? HeapReAlloc(GetProcessHeap(), 0, g_tasks, need * 2 * sizeof *g_tasks) : HeapAlloc(GetProcessHeap(), 0, need * 2 * sizeof *g_tasks);
        if (!t) goto out;
        g_tasks = t;
        g_tasks_cap = need * 2;
    }
    while (g_task_inloop) YieldProcessor();   // a helper that woke late is still looking at the last frame's counters
    for (unsigned m = 0; m < meshes; m++) {
        unsigned char *o = mesh[m];
        if ((*(void ***)o)[0x130 / 4] != g_soldier_stub) goto out;   // a list of mixed mesh classes: left to the game
        for (unsigned p = 0; p < LIST_PASSES; p++) {
            unsigned char *list = o + LIST_FIRST + p * LIST_SIZE;
            g_list_merge(list);
            unsigned c = *(unsigned *)(list + 8);
            if (c < 2) continue;
            g_tasks[n].first = *(void ***)(list + 0xC);
            g_tasks[n++].n = c;
            items += c;
        }
    }
    if (n < 2 || items < g_parsort_min) goto out;
    if (!g_par_sem) g_par_sem = CreateSemaphoreW(NULL, 0, 64, NULL);
    while (g_par_sem && g_par_threads < g_parsort_helpers) {
        // 256 KB of address space each, not the exe's default stack: the 32-bit game has none to spare
        HANDLE h = CreateThread(NULL, 0x40000, sort_helper, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!h) break;
        // The job (and the main thread behind it) waits for a helper that holds a list: one step above normal, so a
        // helper is not parked in the middle of a list when every core is busy. They run about a millisecond a frame.
        SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(h);
        g_par_threads++;
    }
    g_task_next = 0;
    g_task_pending = (LONG)n;
    InterlockedExchange(&g_task_count, (LONG)n);   // open: tasks can be taken from here on
    int wake = g_par_threads < g_parsort_helpers ? g_par_threads : g_parsort_helpers;
    if (wake > 0) ReleaseSemaphore(g_par_sem, wake, NULL);
    run_sort_tasks();
    for (unsigned spin = 0; g_task_pending; spin++) {   // a helper is finishing the list it took
        if (spin < 4000) YieldProcessor(); else SwitchToThread();
    }
    InterlockedExchange(&g_task_count, 0);         // closed
    InterlockedIncrement(&g_parsort_jobs);
    done = 1;
out:
    InterlockedExchange(&g_par_busy, 0);
    return done;
}

// With parsort_ab.txt: how long the soldiers' list job takes in the on windows and in the off windows, to heapfix.log
// every 4096 jobs. `on`: the window; `par`: this job's lists were sorted here (a small frame in an on window is not).
static void job_stats(unsigned char *node, int on, int par, LONGLONG ticks) {
    static LONGLONG t[2], worst[2], f;
    static unsigned jobs[2], sorted;
    unsigned meshes = *(unsigned *)(node + 8);
    if (!meshes || (*(void ***)**(void ****)(node + 0xC))[0x130 / 4] != g_soldier_stub) return;   // not the soldiers' job
    t[on] += ticks;
    sorted += par;
    if (ticks > worst[on]) worst[on] = ticks;   // a helper that held the job up shows here, not in the average
    if (++jobs[on], (jobs[0] + jobs[1]) % 4096) return;
    if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = q.QuadPart; }
    g_par_log("soldier list job A/B: ON windows %u jobs (%u with the lists sorted on %d threads), %.0f us each (worst %.0f); "
              "off windows %u jobs on one thread, %.0f us each (worst %.0f)",
              jobs[1], sorted, g_par_threads + 1, jobs[1] ? t[1] * 1e6 / f / jobs[1] : 0.0, worst[1] * 1e6 / f,
              jobs[0], jobs[0] ? t[0] * 1e6 / f / jobs[0] : 0.0, worst[0] * 1e6 / f);
    t[0] = t[1] = worst[0] = worst[1] = 0;
    jobs[0] = jobs[1] = sorted = 0;
}

static void __stdcall list_job(void *node, void *ctx) {
    LARGE_INTEGER t0 = {0}, t1;
    if (g_parsort_ab) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);   // 100 ns units
        g_parsort_on = !((((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 100000000ULL) & 1);
        QueryPerformanceCounter(&t0);
    }
    int on = g_parsort_on, par = on && sort_lists_parallel(node);
    t_presorted = par;
    ((void(__stdcall *)(void *, void *))(void *)g_job_tramp)(node, ctx);
    t_presorted = 0;
    if (g_parsort_ab) {
        QueryPerformanceCounter(&t1);
        job_stats(node, on, par, t1.QuadPart - t0.QuadPart);
    }
}

// 1: the list job sorts the soldiers' lists on several threads. Needs the draw-list sort above (install_sortfix first).
// ab: parsort_ab.txt is in the game folder.
int install_parsort(HMODULE m, int ab, void (*log)(const char *fmt, ...)) {
    static const struct { DWORD rva, len, fnv, fnv_gog; const char *what; } code[] = {
        {RVA_LISTJOB, 0xAB, 0x11198F21u, 0x340B5FA6u, "the list job's loop"},
        {RVA_SOLDIER_STUB, 0x19, 0x3D8ED0ABu, 0x3D8ED0ABu, "the soldier meshes' list stub"},
        {0x17B9270, 0x35, 0x396AA0E1u, 0x1272C1EFu, "the list's merge-and-sort"},
        {RVA_LISTMERGE, 0x108, 0x7A26CBCBu, 0x6056D5A6u, "the list merge"},
    };
    unsigned char *b = (unsigned char *)m;
    if (!g_draw_sort_in) { log("soldier draw lists on several threads: not installed (it needs the draw-list sort)"); return 0; }
    for (unsigned i = 0; i < sizeof code / sizeof *code; i++) {
        DWORD rva = game_rva(m, code[i].rva), h = rva ? fnv1a_unreloc(b, rva, code[i].len) : 0;
        if (!rva || (h != code[i].fnv && h != code[i].fnv_gog)) {
            log("soldier draw lists on several threads: %s at +0x%X does not match this game version - not installed", code[i].what, code[i].rva);
            return 0;
        }
    }
    if (g_parsort_helpers < 0) {   // the game's own threads keep theirs: one helper from 8 logical processors, three from 16
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        g_parsort_helpers = si.dwNumberOfProcessors >= 16 ? 3 : si.dwNumberOfProcessors >= 12 ? 2 : si.dwNumberOfProcessors >= 8;
    }
    if (!g_parsort_helpers) { log("soldier draw lists on several threads: not installed (fewer than 8 logical processors)"); return 0; }
    g_soldier_stub = b + game_rva(m, RVA_SOLDIER_STUB);
    g_list_merge = (void(__thiscall *)(void *))(void *)(b + game_rva(m, RVA_LISTMERGE));
    g_par_log = log;
    unsigned char *site = b + game_rva(m, RVA_LISTJOB);
    DWORD old;
    if (!VirtualProtect(g_job_tramp, sizeof g_job_tramp, PAGE_EXECUTE_READWRITE, &old)) return 0;
    patch_tramp(g_job_tramp, site, 5);   // sub esp,8; push ebx; push ebp
    if (!patch_jump(site, 5, (void *)list_job)) {
        log("soldier draw lists on several threads: VirtualProtect failed (%lu) - not installed", GetLastError());
        return 0;
    }
    log("soldier draw lists sorted on %d threads (the list job + %d helpers)", g_parsort_helpers + 1, g_parsort_helpers);
    g_parsort_ab = ab;
    if (ab) log("soldier draw lists A/B: on in even 10 s windows of UTC time, off in odd (parsort_ab.txt)");
    return 1;
}
