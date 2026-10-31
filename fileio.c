// Faster loading, ported from the Rome 2 CPU Performance Fix (AGENT_LOG 57): two file-access changes, through
// empire.retail.dll's own import table (kernel32 is not touched; other DLLs are not affected).
//  1. The game opens some .pack files with FILE_FLAG_NO_BUFFERING (Attila: 33, read through file mappings). The flag
//     is dropped for .pack files, so plain reads go through the Windows file cache. (The flag only adds alignment
//     rules; reading without it returns the same bytes.)
//  2. The save is unpacked into a file in the user folder and read back mostly a few bytes per ReadFile call (Mk1212:
//     ~435,000 reads and ~735,000 seeks per load). For files the game opens read-only in its user folder
//     (%APPDATA%\The Creative Assembly\Attila\), small reads are served from a 64 KB read-ahead buffer. The game keeps
//     its writer handle open while reading (write sharing), so any WriteFile or SetEndOfFile it makes to a file being
//     buffered (same volume + file index) drops that file's buffers first. The logical file position is kept exactly:
//     SetFilePointer(Ex) on such a handle sets it, a read with an OVERLAPPED offset or a read of 64 KB or more goes
//     straight to Windows, and DuplicateHandle stops the buffering for that handle after putting Windows' file
//     position where the game expects it.
#include <windows.h>
#include <string.h>
#include <wchar.h>
#include "gamerva.h"

#define BUFSZ 65536
#define MAXH 64

typedef HANDLE(WINAPI *createw_fn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL(WINAPI *read_fn)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef DWORD(WINAPI *sfp_fn)(HANDLE, LONG, PLONG, DWORD);
typedef BOOL(WINAPI *sfpx_fn)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
typedef BOOL(WINAPI *close_fn)(HANDLE);
typedef BOOL(WINAPI *dup_fn)(HANDLE, HANDLE, HANDLE, LPHANDLE, DWORD, BOOL, DWORD);
typedef BOOL(WINAPI *write_fn)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL(WINAPI *eof_fn)(HANDLE);

static createw_fn r_CreateFileW;
static read_fn r_ReadFile;
static sfp_fn r_SetFilePointer;
static sfpx_fn r_SetFilePointerEx;
static close_fn r_CloseHandle;
static dup_fn r_DuplicateHandle;
static write_fn r_WriteFile;
static eof_fn r_SetEndOfFile;

struct bh {
    HANDLE h;  // NULL = free slot
    CRITICAL_SECTION cs;
    LONGLONG pos, bstart;  // logical position; file offset of buf[0]
    DWORD blen;            // valid bytes in buf
    unsigned char *buf;
    DWORD vol, idx_hi, idx_lo;  // file identity
};
static struct bh g_bh[MAXH];
static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile LONG g_open;  // slots in use (fast path: 0 = nothing buffered)
static wchar_t g_userdir[MAX_PATH];
static size_t g_userdir_len;
volatile LONG g_fio_unbuffered_packs, g_fio_buffered_files, g_fio_reads, g_fio_refills;
static void (*g_fio_log)(const char *fmt, ...) __attribute__((unused));  // used by the dev builds' stats lines

static struct bh *find(HANDLE h) {
    if (!g_open || h == NULL || h == INVALID_HANDLE_VALUE) return NULL;
    struct bh *r = NULL;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < MAXH; i++)
        if (g_bh[i].h == h) { r = &g_bh[i]; break; }
    ReleaseSRWLockShared(&g_lock);
    return r;
}

static int file_id(HANDLE h, DWORD id[3]) {
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(h, &fi)) return 0;
    id[0] = fi.dwVolumeSerialNumber;
    id[1] = fi.nFileIndexHigh;
    id[2] = fi.nFileIndexLow;
    return 1;
}

static void track(HANDLE h) {
    DWORD id[3];
    if (!file_id(h, id)) return;
    unsigned char *buf = VirtualAlloc(NULL, BUFSZ, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < MAXH; i++)
        if (!g_bh[i].h) {
            g_bh[i].pos = g_bh[i].bstart = 0;
            g_bh[i].blen = 0;
            g_bh[i].buf = buf;
            g_bh[i].vol = id[0];
            g_bh[i].idx_hi = id[1];
            g_bh[i].idx_lo = id[2];
            g_bh[i].h = h;
            InterlockedIncrement(&g_open);
            InterlockedIncrement(&g_fio_buffered_files);
            buf = NULL;
            break;
        }
    ReleaseSRWLockExclusive(&g_lock);
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);  // table full: this file is simply not buffered
}

// Stops buffering h (the handle stays open). Puts Windows' file position at the logical one first.
static void untrack(HANDLE h, int sync) {
    struct bh *e = find(h);
    if (!e) return;
    EnterCriticalSection(&e->cs);
    if (sync) {
        LARGE_INTEGER p = {.QuadPart = e->pos};
        r_SetFilePointerEx(h, p, NULL, FILE_BEGIN);
    }
    AcquireSRWLockExclusive(&g_lock);
    e->h = NULL;
    InterlockedDecrement(&g_open);
    ReleaseSRWLockExclusive(&g_lock);
    unsigned char *buf = e->buf;
    e->buf = NULL;
    LeaveCriticalSection(&e->cs);
    VirtualFree(buf, 0, MEM_RELEASE);
}

static int ends_with_pack(LPCWSTR n) {
    size_t l = wcslen(n);
    return l >= 5 && _wcsicmp(n + l - 5, L".pack") == 0;
}

HANDLE WINAPI fio_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                              HANDLE tmpl) {
    if (name && (flags & FILE_FLAG_NO_BUFFERING) && ends_with_pack(name)) {
        flags &= ~FILE_FLAG_NO_BUFFERING;
        InterlockedIncrement(&g_fio_unbuffered_packs);
    }
    HANDLE h = r_CreateFileW(name, access, share, sa, disp, flags, tmpl);
    if (find(h)) untrack(h, 0);  // a stale slot: the old handle was closed without our CloseHandle, Windows reused the value
    if (h != INVALID_HANDLE_VALUE && name && g_userdir_len && _wcsnicmp(name, g_userdir, g_userdir_len) == 0 &&
        !(access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) &&
        !(flags & (FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING)) && GetFileType(h) == FILE_TYPE_DISK)
        track(h);
    return h;
}

BOOL WINAPI fio_ReadFile(HANDLE h, LPVOID out, DWORD n, LPDWORD got, LPOVERLAPPED ov) {
    struct bh *e = find(h);
    if (!e) return r_ReadFile(h, out, n, got, ov);
    EnterCriticalSection(&e->cs);
    if (e->h != h) {  // untracked meanwhile
        LeaveCriticalSection(&e->cs);
        return r_ReadFile(h, out, n, got, ov);
    }
    BOOL ok;
    InterlockedIncrement(&g_fio_reads);
    if (ov) {  // positional read on a synchronous handle: Windows' position ends at offset + bytes read
        ok = r_ReadFile(h, out, n, got, ov);
        DWORD g = 0;
        if (ok) g = got ? *got : (DWORD)ov->InternalHigh;
        if (ok) e->pos = ((LONGLONG)ov->OffsetHigh << 32 | ov->Offset) + g;
    } else {
        LARGE_INTEGER p = {.QuadPart = e->pos};
        LONGLONG have = e->bstart + e->blen - e->pos;  // buffered bytes at the logical position
        if (e->pos < e->bstart || have < 0) have = 0;
        if (have >= n) {  // all from the buffer
            memcpy(out, e->buf + (e->pos - e->bstart), n);
            if (got) *got = n;
            e->pos += n;
            ok = TRUE;
        } else if (n >= BUFSZ) {  // big read: straight from Windows
            DWORD g = 0;
            ok = r_SetFilePointerEx(h, p, NULL, FILE_BEGIN) && r_ReadFile(h, out, n, &g, NULL);
            if (got) *got = ok ? g : 0;
            if (ok) e->pos += g;
        } else {  // refill at the logical position (buffered bytes are re-read: simple and exact)
            DWORD g = 0;
            InterlockedIncrement(&g_fio_refills);
            ok = r_SetFilePointerEx(h, p, NULL, FILE_BEGIN) && r_ReadFile(h, e->buf, BUFSZ, &g, NULL);
            if (ok) {
                e->bstart = e->pos;
                e->blen = g;
                DWORD take = g < n ? g : n;  // fewer bytes at the end of the file, as Windows returns
                memcpy(out, e->buf, take);
                if (got) *got = take;
                e->pos += take;
            } else {
                e->blen = 0;
                if (got) *got = 0;
            }
        }
    }
    DWORD err = GetLastError();
    LeaveCriticalSection(&e->cs);
    SetLastError(err);
    return ok;
}

// Target of a seek (FALSE + last error for a bad method or a negative position, as Windows).
static BOOL target(struct bh *e, HANDLE h, LONGLONG dist, DWORD method, LONGLONG *np) {
    if (method == FILE_BEGIN) *np = dist;
    else if (method == FILE_CURRENT) *np = e->pos + dist;
    else if (method == FILE_END) {
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(h, &sz)) return FALSE;
        *np = sz.QuadPart + dist;
    } else {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (*np < 0) {
        SetLastError(ERROR_NEGATIVE_SEEK);
        return FALSE;
    }
    return TRUE;
}

// Moves the logical position only. Windows' own position is set where it matters: before every real read (above)
// and when the buffering stops (untrack). The game asks for its position very often while reading the save (a
// DEI load spent ~0.65 s in these calls when each one went to Windows).
static BOOL move_to(struct bh *e, HANDLE h, LONGLONG np) {
    (void)h;
    e->pos = np;
    return TRUE;
}

BOOL WINAPI fio_SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER newpos, DWORD method) {
    struct bh *e = find(h);
    if (!e) return r_SetFilePointerEx(h, dist, newpos, method);
    EnterCriticalSection(&e->cs);
    BOOL ok;
    LONGLONG np;
    if (e->h != h) ok = r_SetFilePointerEx(h, dist, newpos, method);
    else if ((ok = target(e, h, dist.QuadPart, method, &np) && move_to(e, h, np)) && newpos) newpos->QuadPart = np;
    DWORD err = GetLastError();
    LeaveCriticalSection(&e->cs);
    SetLastError(err);
    return ok;
}

// SetFilePointer: without hi, lo is a signed 32-bit distance and the result must fit in 32 bits
// (else ERROR_INVALID_PARAMETER and no move); a successful result of 0xFFFFFFFF comes with NO_ERROR.
DWORD WINAPI fio_SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD method) {
    struct bh *e = find(h);
    if (!e) return r_SetFilePointer(h, lo, hi, method);
    EnterCriticalSection(&e->cs);
    DWORD ret = INVALID_SET_FILE_POINTER;
    LONGLONG np;
    if (e->h != h) ret = r_SetFilePointer(h, lo, hi, method);
    else if (target(e, h, hi ? ((LONGLONG)*hi << 32 | (DWORD)lo) : (LONGLONG)lo, method, &np)) {
        if (!hi && (np >> 32)) SetLastError(ERROR_INVALID_PARAMETER);
        else if (move_to(e, h, np)) {
            if (hi) *hi = (LONG)(np >> 32);
            ret = (DWORD)np;
            if (ret == INVALID_SET_FILE_POINTER) SetLastError(NO_ERROR);
        }
    }
    DWORD err = GetLastError();
    LeaveCriticalSection(&e->cs);
    SetLastError(err);
    return ret;
}

// Before the game changes a file: drop the buffers of every buffered handle on the same file.
static void invalidate(HANDLE h) {
    if (!g_open) return;
    DWORD id[3];
    if (!file_id(h, id)) return;
    for (int i = 0; i < MAXH; i++) {
        struct bh *e = &g_bh[i];
        if (!e->h || e->vol != id[0] || e->idx_hi != id[1] || e->idx_lo != id[2]) continue;
        EnterCriticalSection(&e->cs);
        e->blen = 0;
        LeaveCriticalSection(&e->cs);
    }
}

BOOL WINAPI fio_WriteFile(HANDLE h, LPCVOID data, DWORD n, LPDWORD done, LPOVERLAPPED ov) {
    invalidate(h);
    BOOL ok = r_WriteFile(h, data, n, done, ov);
    DWORD err = GetLastError();
    invalidate(h);  // also after: a reader may have refilled while the write was in flight
    SetLastError(err);
    return ok;
}

BOOL WINAPI fio_SetEndOfFile(HANDLE h) {
    invalidate(h);
    BOOL ok = r_SetEndOfFile(h);
    DWORD err = GetLastError();
    invalidate(h);
    SetLastError(err);
    return ok;
}

BOOL WINAPI fio_CloseHandle(HANDLE h) {
    if (find(h)) {
        untrack(h, 0);
    }
    return r_CloseHandle(h);
}

BOOL WINAPI fio_DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, LPHANDLE out, DWORD access, BOOL inherit, DWORD opts) {
    if (sp == GetCurrentProcess() && find(src)) untrack(src, 1);  // the copy shares Windows' file position
    return r_DuplicateHandle(sp, src, tp, out, access, inherit, opts);
}

// The real functions (also used by the tests, which call fio_* directly).
void fio_init(const wchar_t *userdir) {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    r_CreateFileW = (createw_fn)(void *)GetProcAddress(k, "CreateFileW");
    r_ReadFile = (read_fn)(void *)GetProcAddress(k, "ReadFile");
    r_SetFilePointer = (sfp_fn)(void *)GetProcAddress(k, "SetFilePointer");
    r_SetFilePointerEx = (sfpx_fn)(void *)GetProcAddress(k, "SetFilePointerEx");
    r_CloseHandle = (close_fn)(void *)GetProcAddress(k, "CloseHandle");
    r_DuplicateHandle = (dup_fn)(void *)GetProcAddress(k, "DuplicateHandle");
    r_WriteFile = (write_fn)(void *)GetProcAddress(k, "WriteFile");
    r_SetEndOfFile = (eof_fn)(void *)GetProcAddress(k, "SetEndOfFile");
    for (int i = 0; i < MAXH; i++) InitializeCriticalSection(&g_bh[i].cs);
    if (userdir && wcslen(userdir) < MAX_PATH) {
        wcscpy(g_userdir, userdir);
        g_userdir_len = wcslen(userdir);
    }
}

// Hooks empire.retail.dll's imports. All eight or nothing (a partial set could lose the logical position).
int install_fileio(HMODULE m, void (*log)(const char *fmt, ...)) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    if (!n || n > MAX_PATH - 40) {
        log("file I/O fix: APPDATA not found - not installed");
        return 0;
    }
    wcscat(dir, L"\\The Creative Assembly\\Attila\\");
    fio_init(dir);
    g_fio_log = log;
    static const char *names[8] = {"CreateFileW", "ReadFile", "SetFilePointer", "SetFilePointerEx", "CloseHandle",
                                   "DuplicateHandle", "WriteFile", "SetEndOfFile"};
    void *to[8] = {(void *)fio_CreateFileW, (void *)fio_ReadFile, (void *)fio_SetFilePointer, (void *)fio_SetFilePointerEx,
                   (void *)fio_CloseHandle, (void *)fio_DuplicateHandle, (void *)fio_WriteFile, (void *)fio_SetEndOfFile};
    void *old[8];
    for (int i = 0; i < 8; i++) {
        old[i] = iat_hook(m, names[i], to[i]);
        if (!old[i]) {
            for (int j = 0; j < i; j++) iat_hook(m, names[j], old[j]);  // undo
            log("file I/O fix: import %s not found - not installed", names[i]);
            return 0;
        }
    }
    log("file I/O fix installed: .pack files go through the Windows file cache; small reads of save/temp files are buffered");
    return 1;
}
