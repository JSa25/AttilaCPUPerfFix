// Frame hook on the game's per-frame function (FUN_100c1950, called once per main-loop iteration).
// Release builds use it only to run the per-frame safety checks (campfix.c, x87fix.c).
// Dev builds also timestamp every call: while <game folder>\fps_on.txt exists, a background thread
// writes one line per second to fps_log.csv, and a summary for the whole recording when it stops.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>
#include "gamerva.h"

#define RVA_FRAME 0x0C1950
static const unsigned char FRAME_PROLOGUE[10] = {0x56, 0x8B, 0xF1, 0x80, 0xBE, 0xC8, 0x00, 0x00, 0x00, 0x00};

static unsigned char g_tramp[16];
void *g_fps_tramp_ptr = g_tramp;
#define RING 16384
static LONG64 g_ring[RING];
static volatile LONG g_count;  // frames recorded so far (only the main thread writes)
static wchar_t g_dir[MAX_PATH];
static int g_timing;   // release: frames are timed only when fps_on.txt was in the game folder at start (improvement)

void (*g_frame_check)(void);  // set by heapfix.c: per-frame checks on the main thread (campfix.c, x87fix.c)
volatile LONG g_campfix_state = -1;  // campfix.c: -1 not installed, 0 off, 1 on (A/B runs alternate)

volatile LONG g_frame_no;   // frames finished, for diag.c's crash records and freeze watchdog (main thread only writes)

void fps_record(void) {
    g_frame_no++;
    if (g_frame_check) g_frame_check();
    if (!g_timing) return;   // without fps_on.txt: one check per frame
    {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    g_ring[g_count & (RING - 1)] = t.QuadPart;
    InterlockedIncrement(&g_count);
    }
}

// Saves every register and flag, records the frame, then runs the original function via the trampoline.
__attribute__((naked)) static void fps_stub(void) {
    __asm__ volatile(
        "pushal\n\t"
        "pushfl\n\t"
        "call _fps_record\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *_g_fps_tramp_ptr\n\t");
}


static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

// fps from the 99th-percentile frame time ("1% low"), the worst frame, and the average.
static void stats(double *ms, int n, double secs, double *avg_fps, double *low1, double *worst) {
    qsort(ms, n, sizeof *ms, cmp_d);
    *avg_fps = n / secs;
    *worst = ms[n - 1];
    double p99 = ms[(int)(n * 0.99) < n ? (int)(n * 0.99) : n - 1];
    *low1 = p99 > 0 ? 1000.0 / p99 : 0;
}


// Release frame-time log for players (improvement, AGENT_LOG 220): with fps_on.txt in the game folder at start, one
// line per second to fps_log.csv (fps, 1 % low, worst frame) while the file exists, then a summary of the recording.
static DWORD WINAPI fps_logger_release(LPVOID unused) {
    (void)unused;
    wchar_t flag[MAX_PATH], csv[MAX_PATH];
    _snwprintf(flag, MAX_PATH, L"%ls\\fps_on.txt", g_dir);
    _snwprintf(csv, MAX_PATH, L"%ls\\fps_log.csv", g_dir);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    FILE *f = _wfopen(csv, L"a");
    if (!f) return 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "# recording %04d-%02d-%02d %02d:%02d:%02d (delete fps_on.txt to stop)\nsecond,fps,one_percent_low_fps,worst_frame_ms\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fflush(f);
    int cap = 1 << 20, all_n = 0, sec = 0;
    static double ms[RING];   // not on this thread's small stack
    double *all = malloc(cap * sizeof *all);
    LONG last = g_count;
    while (GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES) {
        Sleep(1000);
        LONG now = g_count;
        if (now - last > RING - 1) last = now - (RING - 1);   // a slow second for this thread: the newest frames only
        int n = 0;
        for (LONG i = last + 1; i < now; i++) {
            double d = (double)(g_ring[i & (RING - 1)] - g_ring[(i - 1) & (RING - 1)]) * 1000.0 / freq.QuadPart;
            ms[n++] = d;
            if (all && all_n < cap) all[all_n++] = d;
        }
        last = now - 1;   // the frame in progress ends after this sample: counted next time (a frame over 1 s always spans one)
        sec++;
        if (n < 2) continue;   // a loading screen or a pause
        double secs = 0;
        for (int i = 0; i < n; i++) secs += ms[i];
        double avg, low, worst;
        qsort(ms, n, sizeof *ms, cmp_d);
        stats(ms, n, secs / 1000.0, &avg, &low, &worst);
        fprintf(f, "%d,%.1f,%.1f,%.2f\n", sec, avg, low, worst);
        fflush(f);
    }
    if (all && all_n > 1) {
        double secs = 0, avg, low, worst;   // the logged frames' own time: right past the cap too
        for (int i = 0; i < all_n; i++) secs += all[i] / 1000.0;
        qsort(all, all_n, sizeof *all, cmp_d);
        stats(all, all_n, secs > 0 ? secs : 1, &avg, &low, &worst);
        fprintf(f, "# summary: %d frames in %.1f s, average %.1f fps, 1%% low %.1f fps, worst frame %.1f ms\n", all_n, secs, avg, low, worst);
    }
    free(all);
    fclose(f);
    InterlockedExchange((volatile LONG *)&g_timing, 0);
    return 0;
}

// Returns 1 if the frame hook was installed.
int install_fps(HMODULE m, const wchar_t *dir, void (*log)(const char *fmt, ...)) {
    unsigned char *site = (unsigned char *)m + game_rva(m, RVA_FRAME);
    if (memcmp(site, FRAME_PROLOGUE, sizeof FRAME_PROLOGUE) != 0) {
        log("frame function at +0x%X does not match this game version - FPS logging off", RVA_FRAME);
        return 0;
    }
    DWORD old;
    if (!VirtualProtect(g_tramp, sizeof g_tramp, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(g_tramp, site, sizeof FRAME_PROLOGUE);  // push esi; mov esi,ecx; cmp byte [esi+0C8h],0
    g_tramp[10] = 0xE9;
    DWORD rel = (DWORD)((site + 10) - (g_tramp + 15));
    memcpy(g_tramp + 11, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), g_tramp, sizeof g_tramp);
    if (!VirtualProtect(site, 10, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE9;
    rel = (DWORD)((unsigned char *)fps_stub - (site + 5));
    memcpy(site + 1, &rel, 4);
    memset(site + 5, 0x90, 5);  // never executed (the jmp leaves first); keeps the disassembly tidy
    VirtualProtect(site, 10, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 10);
    wcsncpy(g_dir, dir, MAX_PATH - 1);
    wchar_t flag[MAX_PATH];
    _snwprintf(flag, MAX_PATH, L"%ls\\fps_on.txt", g_dir);
    if (GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES) {
        g_timing = 1;
        HANDLE t = CreateThread(NULL, 64 << 10, fps_logger_release, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (t) CloseHandle(t); else g_timing = 0;
    }
    log("frame hook ready (per-frame safety checks)%s", g_timing ? "; frame times to fps_log.csv (fps_on.txt found)" : "");
    return 1;
}
