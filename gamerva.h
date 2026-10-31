// Game builds: empire.retail.dll addresses the mod uses, from the normal Steam build (1.6.0, stamp 69CE1786) to
// the GOG build 6A4F4723 (same code, moved by 0..+192 bytes; .rdata vtables -0x200 in places). Made by
// wip/gog/gogmap.py: code by its bytes (relocated and relative-branch dwords masked) found exactly once, data by
// the pointers in matched code; the 13 render-pass sites that occur in several copies were checked at +192.
// game_rva(base, steam rva): this build's rva; 0 for an address not in the table on the GOG build. Other builds
// (Steam, old GOG 698089B0, unknown) get the Steam rva: each fix's byte check decides there, as before.
// Also the helpers the fixes use on the game module: fnv1a (a hash of its code), iat_hook (its imports) and
// paused_write (changing its code while the game runs).
#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <string.h>

static const unsigned GOG_6A4F4723[][2] = {
    {0x00C1950, 0x00C1950}, {0x00C1C10, 0x00C1C10}, {0x01126E0, 0x01126E0}, {0x0112730, 0x0112730},
    {0x0112D80, 0x0112D80}, {0x0113040, 0x0113040}, {0x01766B0, 0x01766B0}, {0x03FEA50, 0x03FEA50},
    {0x042D9C0, 0x042D9C0}, {0x04552F0, 0x04552F0}, {0x04790D0, 0x04790D0}, {0x07B6800, 0x07B6800},
    {0x07B6940, 0x07B6940}, {0x07B6A80, 0x07B6A80}, {0x07B6BC0, 0x07B6BC0}, {0x07B6D00, 0x07B6D00},
    {0x07B6E40, 0x07B6E40}, {0x07B6F60, 0x07B6F60}, {0x07B7070, 0x07B7070}, {0x08F91C0, 0x08F91C0},
    {0x0D79AC8, 0x0D79AC8}, {0x0D79B12, 0x0D79B12}, {0x16862B0, 0x1686300}, {0x16FCD40, 0x16FCDA0},
    {0x170AF80, 0x170AFF0}, {0x1722420, 0x17224B0}, {0x17293B0, 0x1729440}, {0x172B570, 0x172B600},
    {0x172B690, 0x172B720}, {0x173EED0, 0x173EF70}, {0x1748A80, 0x1748B20}, {0x1749620, 0x17496C0},
    {0x174A8BD, 0x174A95D}, {0x174B470, 0x174B510}, {0x174CC20, 0x174CCC0}, {0x174CCA0, 0x174CD40},
    {0x174CD88, 0x174CE28}, {0x174D880, 0x174D920}, {0x175D5D0, 0x175D670}, {0x1768A00, 0x1768A90},
    {0x1768FC0, 0x1769050}, {0x176A450, 0x176A4E0}, {0x176DF00, 0x176DF90}, {0x1778390, 0x1778430},
    {0x1778E90, 0x1778F30}, {0x1779170, 0x1779210}, {0x1779250, 0x17792F0}, {0x177BB10, 0x177BBB0},
    {0x1785C60, 0x1785D00}, {0x179A6B0, 0x179A750}, {0x179E890, 0x179E950}, {0x17ACB50, 0x17ACC10},
    {0x17AD5D0, 0x17AD690}, {0x17AEF40, 0x17AF000}, {0x17AF2B0, 0x17AF370}, {0x17AF720, 0x17AF7E0},
    {0x17AF7F5, 0x17AF8B5}, {0x17AFC10, 0x17AFCD0}, {0x17AFCE2, 0x17AFDA2}, {0x17AFF90, 0x17B0050},
    {0x17B0065, 0x17B0125}, {0x17B0400, 0x17B04C0}, {0x17B0450, 0x17B0510}, {0x17B0456, 0x17B0516},
    {0x17B0466, 0x17B0526}, {0x17B04BF, 0x17B057F}, {0x17B05A3, 0x17B0663}, {0x17B09F0, 0x17B0AB0},
    {0x17B0F80, 0x17B1040}, {0x17B0FC0, 0x17B1080}, {0x17B0FCB, 0x17B108B}, {0x17B0FD9, 0x17B1099},
    {0x17B1032, 0x17B10F2}, {0x17B1117, 0x17B11D7}, {0x17B1250, 0x17B1310}, {0x17B129B, 0x17B135B},
    {0x17B12A9, 0x17B1369}, {0x17B1302, 0x17B13C2}, {0x17B13E7, 0x17B14A7}, {0x17B1520, 0x17B15E0},
    {0x17B1560, 0x17B1620}, {0x17B156B, 0x17B162B}, {0x17B1579, 0x17B1639}, {0x17B15D2, 0x17B1692},
    {0x17B16AD, 0x17B176D}, {0x17B17F0, 0x17B18B0}, {0x17B1840, 0x17B1900}, {0x17B1A10, 0x17B1AD0},
    {0x17B20E0, 0x17B21A0}, {0x17B2C80, 0x17B2D40}, {0x17B3280, 0x17B3340}, {0x17B3880, 0x17B3940},
    {0x17B4480, 0x17B4540}, {0x17DABAF, 0x17DAC6F}, {0x17DB0F0, 0x17DB1B0}, {0x17FA310, 0x17FA3A0},
    {0x182E080, 0x182E110}, {0x182E0DA, 0x182E16A}, {0x186BE70, 0x186BF30}, {0x18933C0, 0x1893480},
    {0x1893CC0, 0x1893D80}, {0x1897A30, 0x1897AF0}, {0x189B363, 0x189B423}, {0x18E8390, 0x18E8450},
    {0x18F9150, 0x18F9200}, {0x19237A0, 0x1923850}, {0x192B640, 0x192B6F0}, {0x192EEE0, 0x192EF90},
    {0x1A27CA0, 0x1A27CD0}, {0x1AFADE4, 0x1AFADE4}, {0x1C9A900, 0x1C9A700}, {0x1CA0858, 0x1CA0658},
    {0x1CCE4E4, 0x1CCE2E4}, {0x1F20BBC, 0x1F20BBC}, {0x2A21D18, 0x2A21D18}, {0x2A3669C, 0x2A3669C},
    {0x2A601F8, 0x2A601F8}, {0x2A621D0, 0x2A621D0}, {0x2A77BF8, 0x2A77BF8}, {0x2A78008, 0x2A78008},
};

static inline unsigned game_rva(const void *base, unsigned rva) {
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x6A4F4723u) return rva;
    for (unsigned i = 0; i < sizeof GOG_6A4F4723 / sizeof GOG_6A4F4723[0]; i++)
        if (GOG_6A4F4723[i][0] == rva) return GOG_6A4F4723[i][1];
    return 0;
}

static inline DWORD fnv1a(const unsigned char *b, DWORD n) {
    DWORD h = 0x811C9DC5u;
    while (n--) h = (h ^ *b++) * 0x01000193u;
    return h;
}

// Points the IAT entry of `name` (imported from KERNEL32.dll by module m) at `to`; returns the old target or NULL.
static inline void *iat_hook(HMODULE m, const char *name, void *to) {
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

// 1 if the thread was started inside the game's own code (the game module or the exe): only those run the game's code.
// Others set their own state and never run it (XAudio2, thread pool, Steam, EOS, driver threads).
static inline int game_thread(HANDLE h, HMODULE game) {
    static LONG(NTAPI *q)(HANDLE, int, PVOID, ULONG, PULONG);
    if (!q) q = (LONG(NTAPI *)(HANDLE, int, PVOID, ULONG, PULONG))(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    void *start = NULL;
    if (!q || q(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, NULL) != 0) return 0;
    HMODULE m = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, start, &m)) return 0;
    return m == game || m == GetModuleHandleW(NULL);
}

// Game code changed while other game threads may run it (Rome 2's x87 undo fix): write() runs while every other game
// thread is suspended, unless one stands where busy(eip) says - then nothing is written and 0 comes back (try again
// next frame). All threads are found before any is suspended: one suspended inside LoadLibrary holds the loader's module
// lock, which the lookups take (x87fix_test: that deadlocked within ~300 rounds). Nothing that takes a lock (heap,
// loader, CRT) may run in busy() or write().
static inline int paused_write(HMODULE game, int (*busy)(UINT_PTR eip), void (*write)(void)) {
    HANDLE th[1024];
    int nth = 0, ns = 0, clear = 1;   // th[0..ns) are suspended
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te = {.dwSize = sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok && nth < 1024; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (game_thread(h, game)) th[nth++] = h;
        else CloseHandle(h);
    }
    CloseHandle(snap);
    while (ns < nth && clear) {
        if (SuspendThread(th[ns]) == (DWORD)-1) { CloseHandle(th[ns]); th[ns] = th[--nth]; continue; }
        CONTEXT c = {.ContextFlags = CONTEXT_CONTROL};
        if (!GetThreadContext(th[ns++], &c) || busy(c.Eip)) clear = 0;
    }
    if (clear) write();
    for (int i = 0; i < nth; i++) {
        if (i < ns) ResumeThread(th[i]);
        CloseHandle(th[i]);
    }
    return clear;
}
