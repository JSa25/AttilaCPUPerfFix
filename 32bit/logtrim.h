#define LOG_MAX (1024 * 1024)
#define LOG_KEEP (256 * 1024)
void trim_log(const wchar_t *path);  // logtrim.c

// Appends to heapfix.log through one handle per file that includes this, opened on the first write and kept: with
// FILE_APPEND_DATA only, Windows appends each WriteFile whole at the end, so lines from several threads and from
// crash records never overwrite each other (round 2: fopen "a" seeks then writes; ~5 % of crash-record lines were
// lost in a race test), and a write needs no file open - an open takes the process heap's lock, which a crash or a
// freeze may hold. Open it early (one empty append) so a crash never has to.
static HANDLE log_append_h;
static inline void log_append(const wchar_t *path, const char *s, int n) {
    HANDLE h = log_append_h;
    if (!h) {
        HANDLE nh = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, 0, NULL);
        if (nh == INVALID_HANDLE_VALUE) return;
        if (InterlockedCompareExchangePointer((void **)&log_append_h, nh, NULL)) CloseHandle(nh);
        h = log_append_h;
    }
    DWORD w;
    if (n > 0) WriteFile(h, s, (DWORD)n, &w, NULL);
}
