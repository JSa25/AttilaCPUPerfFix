// Loading fix: a cache of missing folders behind the game's GetFileAttributesW and CreateFileW imports (ported from
// the Rome 2 CPU Performance Fix, where a DEI save load made 3.4 million failed checks, 11.6 s of 28 s).
// Before using a file from the packs, Attila checks whether a loose copy exists under each of its search roots
// (+0xEC8D0 -> GetFileAttributesW; the user maps folder, Data, every Workshop mod folder, a language folder) and
// also tries to open loose files there (CreateFileW, read-only). With Mk1212 a save load made ~218,000 checks and
// ~52,000 opens, nearly all "not found", ~1.5 s together (measured, AGENT_LOG 57).
// A failed check for path P finds the deepest existing folder A above P and records M, the first missing name
// below A. Later checks of exactly M fail with ERROR_FILE_NOT_FOUND and checks of anything below M fail with
// ERROR_PATH_NOT_FOUND, which is what Windows returns. Exactness: A and its parent are watched for created/
// renamed/deleted files and folders (watches registered before M is confirmed missing) and the watches are
// polled on every call; any change clears the whole cache. Paths in unusual forms are passed straight through.
// Opens use the same answers only where Windows' answer for a missing path is the same as for the check: read-only,
// OPEN_EXISTING, no template, plain flags.
#include <windows.h>
#include <string.h>

#define MAXPATHC   400       // longer paths are passed through
#define TABLE      16384     // hash slots (power of two)
#define ARENA      (1 << 20) // wchar_t for the cached names
#define MAXWATCH   189       // polled in groups of 63 (WaitForMultipleObjects takes at most 64 handles)

static DWORD(WINAPI *r_attr)(LPCWSTR);
typedef HANDLE(WINAPI *ac_open_fn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static ac_open_fn r_open;  // what the game's import pointed at before (kernel32, or fileio.c's hook)
static void (*g_log)(const char *fmt, ...);
static int g_verify;  // run the real call too and count differences (profiling builds, attrcache_verify.txt)

typedef struct { DWORD hash, len, off; } slot_t;
static slot_t g_tab[TABLE];
static wchar_t *g_arena;
static DWORD g_used, g_count;
static SRWLOCK g_lock = SRWLOCK_INIT;
static HANDLE g_watch[MAXWATCH];
static wchar_t *g_watch_dir[MAXWATCH];
static volatile LONG g_nwatch;
static volatile LONG g_hits, g_calls, g_inserts, g_resets, g_mismatch, g_skips, g_open_calls, g_open_hits;
static int g_nowatch;  // tests only: ignore the watches, to prove the test catches a stale cache

static DWORD fnv_step(DWORD h, wchar_t c) { return (h ^ c) * 0x01000193u; }

// DOS device names (con, nul, com1, lpt1.txt, ...) mean the device in any folder: never cached
static int device_name(const wchar_t *c, DWORD n) {  // c: lowercase component, n chars
    DWORD b = 0;
    while (b < n && c[b] != L'.' && c[b] != L' ') b++;  // the name before any extension / trailing spaces
    if (b == 3) return !wcsncmp(c, L"con", 3) || !wcsncmp(c, L"prn", 3) || !wcsncmp(c, L"aux", 3) || !wcsncmp(c, L"nul", 3);
    if (b == 4) return (!wcsncmp(c, L"com", 3) || !wcsncmp(c, L"lpt", 3)) && ((c[3] >= L'0' && c[3] <= L'9') || c[3] == 0xB9 || c[3] == 0xB2 || c[3] == 0xB3);
    if (b == 6) return !wcsncmp(c, L"conin$", 6);
    if (b == 7) return !wcsncmp(c, L"conout$", 7);
    return 0;
}

// lowercase ASCII copy of an absolute drive path "x:\..."; 0 for anything unusual (passed through)
static DWORD normalize(LPCWSTR p, wchar_t *out) {
    if (!p) return 0;
    wchar_t c0 = p[0] | 0x20;
    if (c0 < L'a' || c0 > L'z' || p[1] != L':' || p[2] != L'\\') return 0;
    DWORD n = 0;
    for (; p[n]; n++) {
        if (n >= MAXPATHC) return 0;
        wchar_t c = p[n];
        if (c == L'/' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c < 32) return 0;
        if (c == L':' && n != 1) return 0;
        if (c == L'\\' && n > 2 && p[n - 1] == L'\\') return 0;                // empty component
        if (c == L'.' && p[n - 1] == L'\\' && (p[n + 1] == L'\\' || !p[n + 1] || (p[n + 1] == L'.' && (p[n + 2] == L'\\' || !p[n + 2]))))
            return 0;                                                          // "." or ".." component
        out[n] = (c >= L'A' && c <= L'Z') ? (wchar_t)(c | 0x20) : c;
    }
    out[n] = 0;
    for (DWORD s = 3, i = 3; i <= n; i++)  // components after "x:\"
        if (i == n || out[i] == L'\\') {
            if (device_name(out + s, i - s)) return 0;
            s = i + 1;
        }
    return n;
}

static int find(DWORD h, const wchar_t *s, DWORD len) {
    for (DWORD i = h & (TABLE - 1), k = 0; k < TABLE; k++, i = (i + 1) & (TABLE - 1)) {
        if (!g_tab[i].len) return 0;
        if (g_tab[i].hash == h && g_tab[i].len == len && !memcmp(g_arena + g_tab[i].off, s, len * sizeof(wchar_t))) return 1;
    }
    return 0;
}

static void clear_table(void) {
    memset(g_tab, 0, sizeof g_tab);
    g_used = g_count = 0;
    InterlockedIncrement(&g_resets);
}

// Has any watched folder changed? (caller holds the lock, shared or exclusive; a failed wait counts as a change)
static int any_change(void) {
    if (g_nowatch) return 0;
    for (LONG g = 0; g < g_nwatch; g += 63) {
        DWORD cnt = (DWORD)(g_nwatch - g < 63 ? g_nwatch - g : 63);
        DWORD r = WaitForMultipleObjects(cnt, g_watch + g, FALSE, 0);
        if (r != WAIT_TIMEOUT) return 1;
    }
    return 0;
}

// After a change: forget everything, including the watches. A watch follows its folder when the folder is moved,
// so new watches must be opened by path again. Caller holds the lock exclusively; 1 if something changed.
static int drain_changes(void) {
    if (!any_change()) return 0;
    clear_table();
    for (LONG i = 0; i < g_nwatch; i++) {
        FindCloseChangeNotification(g_watch[i]);
        HeapFree(GetProcessHeap(), 0, g_watch_dir[i]);
    }
    g_nwatch = 0;
    return 1;
}

// cached answer: 0 = unknown, else the Windows error to return with INVALID_FILE_ATTRIBUTES
static DWORD lookup(const wchar_t *q, DWORD n) {
    int trail = q[n - 1] == L'\\';
    DWORD len = trail ? n - 1 : n, h = 0x811C9DC5u, err = 0;
    AcquireSRWLockShared(&g_lock);
    if (g_count && any_change()) {
        ReleaseSRWLockShared(&g_lock);
        AcquireSRWLockExclusive(&g_lock);
        drain_changes();
        ReleaseSRWLockExclusive(&g_lock);
        return 0;
    }
    if (g_count) {
        for (DWORD i = 0; i < len; i++) {
            if (q[i] == L'\\' && i > 2 && find(h, q, i)) { err = ERROR_PATH_NOT_FOUND; break; }  // below a missing name
            h = fnv_step(h, q[i]);
        }
        if (!err && !trail && find(h, q, len)) err = ERROR_FILE_NOT_FOUND;  // the missing name itself
    }
    ReleaseSRWLockShared(&g_lock);
    return err;
}

static LONG watch_dir(const wchar_t *dir, DWORD len) {  // index of a watch on dir, or -1
    for (LONG i = 0; i < g_nwatch; i++)
        if (!wcsncmp(g_watch_dir[i], dir, len) && !g_watch_dir[i][len]) return i;
    if (g_nwatch >= MAXWATCH) return -1;
    wchar_t buf[MAXPATHC + 2];
    memcpy(buf, dir, len * sizeof(wchar_t));
    buf[len] = 0;
    if (len == 2) { buf[2] = L'\\'; buf[3] = 0; }  // drive root
    HANDLE w = FindFirstChangeNotificationW(buf, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
    if (w == INVALID_HANDLE_VALUE) return -1;
    wchar_t *copy = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(wchar_t));
    if (!copy) { FindCloseChangeNotification(w); return -1; }
    memcpy(copy, dir, len * sizeof(wchar_t));
    copy[len] = 0;
    g_watch_dir[g_nwatch] = copy;
    g_watch[g_nwatch] = w;
    InterlockedIncrement(&g_nwatch);
    return g_nwatch - 1;
}

static DWORD real_attr_n(const wchar_t *q, DWORD len, DWORD *err) {  // real call on the first len chars of q
    wchar_t buf[MAXPATHC + 2];
    memcpy(buf, q, len * sizeof(wchar_t));
    buf[len] = 0;
    if (len == 2) { buf[2] = L'\\'; buf[3] = 0; }
    DWORD a = r_attr(buf);
    *err = a == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
    return a;
}

// After a real "not found" for q: record the first missing name below the deepest existing folder.
static void learn(const wchar_t *q, DWORD n, DWORD err) {
    int trail = q[n - 1] == L'\\';
    DWORD len = trail ? n - 1 : n;
    if (trail || (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)) { InterlockedIncrement(&g_skips); return; }
    DWORD seps[MAXPATHC / 2], ns = 0;  // positions of '\' after the drive
    for (DWORD i = 2; i < len; i++) if (q[i] == L'\\') seps[ns++] = i;
    if (!ns) return;
    AcquireSRWLockExclusive(&g_lock);
    // deepest existing ancestor A = q[0..seps[k]) (k = 0: the drive "x:")
    LONG k = (LONG)ns - 1;
    DWORD e;
    for (; k > 0; k--) {
        DWORD a = real_attr_n(q, seps[k], &e);
        if (a != INVALID_FILE_ATTRIBUTES) {
            if (!(a & FILE_ATTRIBUTE_DIRECTORY)) k = -1;  // a file in the middle of the path: leave it alone
            break;
        }
    }
    if (k < 0) goto out;
    DWORD mlen = (DWORD)k + 1 < ns ? seps[k + 1] : len;
    // Watch A and every folder above it, top-down, before confirming M is missing: M appearing changes A; A being
    // replaced or moved changes its parent, and so on up to the drive. Each watch's parents are older than it.
    for (LONG j = 0; j <= k; j++)
        if (watch_dir(q, seps[j]) < 0) goto out;
    if (real_attr_n(q, mlen, &e) != INVALID_FILE_ATTRIBUTES || e != ERROR_FILE_NOT_FOUND) goto out;
    // the cache must give the answer Windows just gave
    if (err != (mlen == len ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND)) { InterlockedIncrement(&g_skips); goto out; }
    if (drain_changes()) goto out;  // something changed since the watches were armed: M may exist now
    if (g_count * 2 >= TABLE || g_used + mlen > ARENA) goto out;
    DWORD h = 0x811C9DC5u;
    for (DWORD i = 0; i < mlen; i++) h = fnv_step(h, q[i]);
    if (find(h, q, mlen)) goto out;
    DWORD i = h & (TABLE - 1);
    while (g_tab[i].len) i = (i + 1) & (TABLE - 1);
    memcpy(g_arena + g_used, q, mlen * sizeof(wchar_t));
    g_tab[i].hash = h;
    g_tab[i].off = g_used;
    g_tab[i].len = mlen;
    g_used += mlen;
    g_count++;
    InterlockedIncrement(&g_inserts);
out:
    ReleaseSRWLockExclusive(&g_lock);
}

DWORD WINAPI ac_GetFileAttributesW(LPCWSTR p) {
    wchar_t q[MAXPATHC + 2];
    DWORD entry_le = GetLastError();  // Windows leaves it unchanged on success
    InterlockedIncrement(&g_calls);
    DWORD n = normalize(p, q);
    if (!n) return r_attr(p);
    DWORD cached = lookup(q, n);
    if (cached && !g_verify) {
        InterlockedIncrement(&g_hits);
        SetLastError(cached);
        return INVALID_FILE_ATTRIBUTES;
    }
    SetLastError(entry_le);  // the lookup may have changed it
    DWORD a = r_attr(p), le = GetLastError(), err = a == INVALID_FILE_ATTRIBUTES ? le : 0;
    if (cached) {
        InterlockedIncrement(&g_hits);
        if (a != INVALID_FILE_ATTRIBUTES || err != cached) {
            if (InterlockedIncrement(&g_mismatch) <= 20 && g_log)
                g_log("attribute cache MISMATCH: %ls cached error %lu, Windows %08lX error %lu", p, cached, a, err);
        }
    } else if (a == INVALID_FILE_ATTRIBUTES)
        learn(q, n, err);
    SetLastError(le);  // as Windows left it (the checks above may have changed it)
    return a;
}

// Flags whose open of a missing path fails exactly like the check: attributes (ignored when opening), buffering hints,
// backup semantics (the game's stat, FUN_119fd930: FILE_READ_ATTRIBUTES, used per search root when a menu opens).
#define AC_OPEN_FLAGS (0x7FFFu | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN |                        FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_WRITE_THROUGH)

HANDLE WINAPI ac_CreateFileW(LPCWSTR p, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                             HANDLE tmpl) {
    wchar_t q[MAXPATHC + 2];
    DWORD n = 0;
    if (disp == OPEN_EXISTING && (access == GENERIC_READ || access == FILE_READ_ATTRIBUTES) && !tmpl && !(flags & ~AC_OPEN_FLAGS)) n = normalize(p, q);
    if (!n) return r_open(p, access, share, sa, disp, flags, tmpl);
    DWORD entry_le = GetLastError();
    InterlockedIncrement(&g_open_calls);
    DWORD cached = lookup(q, n);
    if (cached && !g_verify) {
        InterlockedIncrement(&g_open_hits);
        SetLastError(cached);
        return INVALID_HANDLE_VALUE;
    }
    SetLastError(entry_le);
    HANDLE h = r_open(p, access, share, sa, disp, flags, tmpl);
    DWORD le = GetLastError();
    if (cached) {
        InterlockedIncrement(&g_open_hits);
        if (h != INVALID_HANDLE_VALUE || le != cached) {
            if (InterlockedIncrement(&g_mismatch) <= 20 && g_log)
                g_log("attribute cache MISMATCH (open): %ls cached error %lu, Windows %s error %lu", p, cached,
                      h != INVALID_HANDLE_VALUE ? "opened it," : "failed,", h != INVALID_HANDLE_VALUE ? 0 : le);
        }
    } else if (h == INVALID_HANDLE_VALUE)
        learn(q, n, le);
    SetLastError(le);
    return h;
}

void attrcache_stats(void (*log)(const char *fmt, ...)) {
    log("attribute cache: %ld checks, %ld answered from the cache; %ld opens, %ld answered from the cache; %ld missing "
        "names learned, %ld resets, %ld watches, %ld not cacheable", g_calls, g_hits, g_open_calls, g_open_hits,
        g_inserts, g_resets, g_nwatch, g_skips);
    if (g_verify) log("attribute cache verify: %ld mismatches", g_mismatch);
}

static DWORD WINAPI stats_thread(void *a) {  // one line after each burst of calls (start, each load)
    (void)a;
    LONG logged = 0, last = 0;
    for (;;) {
        Sleep(2000);
        LONG c = g_calls + g_open_calls;
        if (c != last) { last = c; continue; }
        if (c - logged < 10000) continue;
        logged = c;
        attrcache_stats(g_log);
    }
    return 0;
}

static void *ac_iat_hook(HMODULE m, const char *name, void *to) {
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

// also used by the test (m = NULL: no import patch, only the cache)
int attrcache_init(int verify, void (*log)(const char *fmt, ...)) {
    g_log = log;
    g_verify = verify;
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    r_attr = (DWORD(WINAPI *)(LPCWSTR))(void *)GetProcAddress(k, "GetFileAttributesW");
    r_open = (ac_open_fn)(void *)GetProcAddress(k, "CreateFileW");
    if (!g_arena) g_arena = VirtualAlloc(NULL, ARENA * sizeof(wchar_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return r_attr && r_open && g_arena;
}

int install_attrcache(HMODULE m, int verify, void (*log)(const char *fmt, ...)) {
    if (!attrcache_init(verify, log) || !ac_iat_hook(m, "GetFileAttributesW", (void *)ac_GetFileAttributesW)) {
        log("attribute cache: GetFileAttributesW import not found - not installed");
        return 0;
    }
    // Opens: chain to whatever the import pointed at (fileio.c's hook when that fix is installed first). r_open is
    // kernel32's until then, which is the same function when nothing else hooked it.
    void *old = ac_iat_hook(m, "CreateFileW", (void *)ac_CreateFileW);
    if (old) r_open = (ac_open_fn)old;
    else log("attribute cache: CreateFileW import not found - only file checks are cached");
    HANDLE t = CreateThread(NULL, 64 * 1024, stats_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    log("attribute cache installed: missing loose-file folders are remembered (cleared when a watched folder changes)%s",
        verify ? " - VERIFY mode, every answer is checked against Windows" : "");
    return 1;
}
