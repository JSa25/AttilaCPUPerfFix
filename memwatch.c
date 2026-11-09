// Address-space watch (AGENT_LOG 215). The 32-bit game crashes when it runs out of address space and does not check:
// leaving a battle with a big mod, MapViewOfFile of pack data returned NULL and FUN_10104400 read address 0
// (reports/2026-11-06_limpnoodles_battle_exit_crash). This hooks the game DLL's imports MapViewOfFile and VirtualAlloc:
// a failure is logged with the size and the address space left; for a file map the pools and the game's heap give
// back what they can (trim) and the map is tried once more. A thread logs each time the largest free block first drops below
// 512 / 256 / 128 / 64 MB, so a crash log shows how close the game was.
#include <windows.h>
#include <stdio.h>

void address_space(size_t *free_total, size_t *largest);   // heapfix.c

static void (*g_log)(const char *fmt, ...);
static void (*g_trim)(void);   // the pools and the game's heap give free memory back; NULL with the heap fix off
static LPVOID(WINAPI *o_MapViewOfFile)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
static LPVOID(WINAPI *o_VirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD);
static volatile LONG g_fail_logged, g_va_logged;   // file maps, VirtualAlloc: separate caps (round 2)
enum { MAX_FAIL_LOGS = 40 };   // a failing loop must not fill the log

static void failed(const char *what, SIZE_T n, DWORD err, int retried_ok) {
    if (InterlockedIncrement(&g_fail_logged) > MAX_FAIL_LOGS) return;
    size_t fr, big;
    address_space(&fr, &big);
    g_log("memory: %s of %lu KB FAILED (error %lu; address space %u MB free, largest free block %u MB)%s", what,
          (unsigned long)(n >> 10), err, (unsigned)(fr >> 20), (unsigned)(big >> 20),
          !g_trim ? "" : retried_ok ? " - freed the mod's spare memory and tried again: worked"
                                    : " - freed the mod's spare memory and tried again: failed too");
}

static LPVOID WINAPI h_MapViewOfFile(HANDLE h, DWORD acc, DWORD hi, DWORD lo, SIZE_T n) {
    LPVOID p = o_MapViewOfFile(h, acc, hi, lo, n);
    if (p) return p;
    DWORD err = GetLastError();
    if (g_trim) {
        g_trim();
        p = o_MapViewOfFile(h, acc, hi, lo, n);
    }
    failed("game file map (MapViewOfFile)", n, err, p != NULL);
    if (!p) SetLastError(err);
    return p;
}

// log only, no trim and retry: the game's own heap calls this while holding its lock, and the trim takes that lock
static LPVOID WINAPI h_VirtualAlloc(LPVOID a, SIZE_T n, DWORD type, DWORD prot) {
    LPVOID p = o_VirtualAlloc(a, n, type, prot);
    if (p || a) return p;   // with an address asked for, failing is a normal answer (the game probes)
    // reserve only: the game's free-address probe before a file map (FUN_100df470) - its failure is followed by the
    // map, which is logged; not counted here (round 2)
    if (!(type & MEM_COMMIT)) return p;
    DWORD err = GetLastError();
    if (InterlockedIncrement(&g_va_logged) <= MAX_FAIL_LOGS / 2) {
        size_t fr, big;
        address_space(&fr, &big);
        g_log("memory: game VirtualAlloc of %lu KB FAILED (error %lu; address space %u MB free, largest free block %u MB)",
              (unsigned long)(n >> 10), err, (unsigned)(fr >> 20), (unsigned)(big >> 20));
    }
    SetLastError(err);
    return p;
}

static DWORD WINAPI low_water(LPVOID arg) {
    (void)arg;
    static const unsigned marks[] = {512, 256, 128, 64};
    unsigned next = 0;
    while (next < sizeof marks / sizeof *marks) {
        Sleep(5000);
        size_t fr, big;
        address_space(&fr, &big);
        unsigned mb = (unsigned)(big >> 20);
        if (mb >= marks[next]) continue;
        while (next < sizeof marks / sizeof *marks && mb < marks[next]) next++;
        g_log("memory: address space getting low - largest free block %u MB, %u MB free in all%s", mb,
              (unsigned)(fr >> 20), mb < 128 ? " (the game may crash when it next loads something big)" : "");
    }
    return 0;
}

// the game DLL's import of KERNEL32!name now goes to `to`; returns the old target or NULL
static void *iat_swap(HMODULE m, const char *name, void *to) {
    unsigned char *b = (unsigned char *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd->VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(b + d->Name), "KERNEL32.dll") != 0 || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(b + d->OriginalFirstThunk), *iat = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(b + names->u1.AddressOfData))->Name, name) != 0) continue;
            void *old = (void *)iat->u1.Function;
            DWORD prot;
            if (!old || !VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &prot)) return NULL;
            iat->u1.Function = (DWORD_PTR)to;
            VirtualProtect(&iat->u1.Function, sizeof(void *), prot, &prot);
            return old;
        }
    }
    return NULL;
}

// Returns the number of imports hooked (0-2).
int install_memwatch(HMODULE m, void (*trim)(void), void (*log)(const char *fmt, ...)) {
    g_log = log;
    g_trim = trim;
    HMODULE k = GetModuleHandleW(L"kernel32.dll");   // set before the swap: another thread may call in at once
    o_MapViewOfFile = (void *)GetProcAddress(k, "MapViewOfFile");
    o_VirtualAlloc = (void *)GetProcAddress(k, "VirtualAlloc");
    void *was;
    int n = 0;
    if ((was = iat_swap(m, "MapViewOfFile", (void *)h_MapViewOfFile))) o_MapViewOfFile = was, n++;
    if ((was = iat_swap(m, "VirtualAlloc", (void *)h_VirtualAlloc))) o_VirtualAlloc = was, n++;
    HANDLE t = CreateThread(NULL, 0, low_water, NULL, 0, NULL);
    if (t) CloseHandle(t);
    size_t fr, big;
    address_space(&fr, &big);
    log("memory watch: %d of 2 game imports watched (file maps, VirtualAlloc)%s; address space now %u MB free, largest "
        "free block %u MB", n, trim ? ", spare memory freed and retried on a failure" : "", (unsigned)(fr >> 20),
        (unsigned)(big >> 20));
    return n;
}
