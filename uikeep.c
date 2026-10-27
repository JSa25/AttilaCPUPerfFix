// Menu stutter fix (AGENT_LOG 75): opening a panel such as a province's menu froze one frame for 100-180 ms
// (captures/ab/province_menu), also without any mods. Menu pictures (ui\skins\default\*.png and the like) are
// counted (+0x38 of their entry in the table at +0x2A3669C). When a panel closes, the release (FUN_1174d080) puts
// every picture whose count reached 0 on a deletion queue, and the queue step (FUN_1174cca0) deletes them
// (FUN_1174cc20), so the next open loads the same files again: 3,000 loads of only 611 names in 45 s of province
// clicks with Mk1212 (loadnames.c). Here the queue step's delete call keeps the picture instead and remembers its
// name; the game's own acquire (FUN_1175d5d0) treats a count of 0 as loaded and just counts it up again. Past KEEP
// names the oldest is deleted the game's way if it is still unused. Kept pictures are found again by name, never
// by a stored pointer.
#include <windows.h>
#include <string.h>

#define RVA_STEP 0x174CCA0u         // the queue step, checked through its delete call
#define LEN_STEP 0xEDu
#define FNV_STEP 0x19D53C88u
#define RVA_CALL 0x174CD88u         // call FUN_1174cc20 inside it (rel32 replaced)
#define RVA_DELETE 0x174CC20u       // thiscall(manager, name): releases its sub-texture, deletes the picture
#define LEN_DELETE 0x27u
#define FNV_DELETE 0x5BD91966u
#define RVA_ACQUIRE 0x175D5D0u      // checked only: its count-of-0 handling is what this relies on
#define LEN_ACQUIRE 0xECu
#define FNV_ACQUIRE 0x26B7C51Fu
#define RVA_TEX_FIND 0x174B470u     // thiscall(manager, name) -> picture (name at +4)
#define RVA_REFS_FIND 0x174D880u    // thiscall(table, name) -> entry: count +0x38, pinned +0x3C
#define RVA_REFS 0x2A3669Cu
#define KEEP 1024                   // shortcut: a count, not bytes (611 names covered every menu in the test)

typedef struct { DWORD len, cap; const char *text; } gname;  // the game's string as its hash and compare read it
typedef char *(__attribute__((thiscall)) * find_fn)(void *self, const gname *name);
typedef void(__attribute__((thiscall)) * delete_fn)(void *mgr, const gname *name);

static find_fn g_refs_find, g_tex_find;
static delete_fn g_delete;
static char *g_refs;
volatile unsigned char g_uikeep_on;  // heapfix.c: off with uikeep_ab.txt in the off windows

extern volatile DWORD g_uikeep_tid;  // diag.c: a crash record says when it happened inside this fix's work
static struct { DWORD len; char *text; } g_ring[KEEP];
static unsigned g_head, g_count;
static volatile LONG g_kept, g_again, g_freed, g_gone;  // volatile: only read by the dev log

// Deletes the kept picture of that name if nothing took it again and the game still has it (the queue step's own
// test: count 0 or less, not pinned). Called inside the queue step, under the game's lock.
static void free_kept(void *mgr, DWORD len, char *text) {
    gname n = {len, len, text};
    char *tex = g_tex_find(mgr, &n);
    char *e = tex ? g_refs_find(g_refs, (gname *)(tex + 4)) : NULL;
    if (e && *(int *)(e + 0x38) <= 0 && !*(int *)(e + 0x3C)) {
        g_delete(mgr, (gname *)(tex + 4));
        g_freed++;
    } else
        g_gone++;
    HeapFree(GetProcessHeap(), 0, text);
}

static void free_all(void *mgr) {
    for (; g_count; g_count--) {
        unsigned k = (g_head + KEEP - g_count) % KEEP;  // oldest
        free_kept(mgr, g_ring[k].len, g_ring[k].text);
    }
}

// Replaces the queue step's delete call (same arguments). The step then takes the name off the queue as usual.
static void uk_delete_body(void *mgr, const gname *name);
static void __attribute__((thiscall)) uk_delete(void *mgr, const gname *name) {
    g_uikeep_tid = GetCurrentThreadId();
    uk_delete_body(mgr, name);
    g_uikeep_tid = 0;
}

static void uk_delete_body(void *mgr, const gname *name) {
    if (!g_uikeep_on || !name->len || name->len > 4096) {
        g_delete(mgr, name);  // first: name points into that picture, which free_all could delete
        free_all(mgr);        // A/B off window: start from nothing kept
        return;
    }
    unsigned i = 0;
    for (; i < g_count; i++) {  // kept before, used again, queued again: already in the ring
        unsigned k = (g_head + KEEP - 1 - i) % KEEP;
        if (g_ring[k].len == name->len && !memcmp(g_ring[k].text, name->text, name->len)) break;
    }
    if (i < g_count) {
        g_again++;  // shortcut: keeps its old place in the ring, not moved to newest
        return;
    }
    char *copy = HeapAlloc(GetProcessHeap(), 0, name->len);
    if (!copy) { g_delete(mgr, name); return; }
    memcpy(copy, name->text, name->len);
    g_kept++;
    if (g_count == KEEP)
        free_kept(mgr, g_ring[g_head].len, g_ring[g_head].text);
    else
        g_count++;
    g_ring[g_head].len = name->len;
    g_ring[g_head].text = copy;
    g_head = (g_head + 1) % KEEP;
}

#ifndef UIKEEP_TEST
// FNV-1a of the code as on disk: these functions hold absolute addresses, which the loader moves with the DLL
// (it is never at its preferred base 0x10000000 in game). The relocation table says where they are.
static DWORD uk_fnv(const unsigned char *b, DWORD rva, DWORD n) {
    unsigned char buf[0x200];
    if (n + 8 > sizeof buf) return 0;
    memcpy(buf, b + rva - 4, n + 8);  // 4 bytes either side: an address can straddle the start or end
    DWORD delta = (DWORD)(UINT_PTR)b - 0x10000000u;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    for (DWORD off = 0; delta && off + 8 <= dir->Size;) {
        const IMAGE_BASE_RELOCATION *r = (const IMAGE_BASE_RELOCATION *)(b + dir->VirtualAddress + off);
        if (r->SizeOfBlock < 8) break;
        const WORD *e = (const WORD *)(r + 1);
        for (DWORD i = 0; i < (r->SizeOfBlock - 8) / 2; i++) {
            DWORD at = r->VirtualAddress + (e[i] & 0xFFF);
            if (e[i] >> 12 == IMAGE_REL_BASED_HIGHLOW && at >= rva - 4 && at + 4 <= rva + n + 4)
                *(DWORD *)(buf + at - (rva - 4)) -= delta;
        }
        off += r->SizeOfBlock;
    }
    DWORD h = 0x811C9DC5u;
    for (DWORD i = 4; i < n + 4; i++) h = (h ^ buf[i]) * 0x01000193u;
    return h;
}

int install_uikeep(HMODULE m, void (*log)(const char *fmt, ...)) {
    unsigned char *b = (unsigned char *)m, *site = b + RVA_CALL;
    if (uk_fnv(b, RVA_STEP, LEN_STEP) != FNV_STEP || uk_fnv(b, RVA_DELETE, LEN_DELETE) != FNV_DELETE ||
        uk_fnv(b, RVA_ACQUIRE, LEN_ACQUIRE) != FNV_ACQUIRE) {
        log("menu textures (+0x%X) differ from this game version - not kept", RVA_STEP);
        return 0;
    }
    g_refs_find = (find_fn)(b + RVA_REFS_FIND);
    g_tex_find = (find_fn)(b + RVA_TEX_FIND);
    g_delete = (delete_fn)(b + RVA_DELETE);
    g_refs = (char *)b + RVA_REFS;
    g_uikeep_on = 1;
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
        log("menu textures: VirtualProtect failed (%lu) - not kept", GetLastError());
        return 0;
    }
    DWORD rel = (DWORD)((unsigned char *)uk_delete - (site + 5));
    memcpy(site + 1, &rel, 4);  // the call stays a call, to uk_delete
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    log("menu textures: the last %d closed ones stay loaded (faster reopening)", KEEP);
    return 1;
}
#endif
