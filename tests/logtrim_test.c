// Tests logtrim.c (AGENT_LOG 73): a log over 1 MB keeps only the newest whole sessions (~256 KB), with a note at the
// top; a short log is left alone; one giant session keeps its newest whole lines.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static wchar_t g_log_path[MAX_PATH];
#include "../logtrim.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static long fsize(void) {
    FILE *f = _wfopen(g_log_path, L"rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

// two writers on one log, as diag.c's and heapfix.c's copies of log_append (two handles): no line may be lost
static wchar_t g_race[MAX_PATH];
static DWORD WINAPI race_a(void *x) {
    (void)x;
    char line[64];
    for (int i = 0; i < 5000; i++) { int n = snprintf(line, sizeof line, "A %05d the quick brown fox\r\n", i); log_append(g_race, line, n); }
    return 0;
}
static DWORD WINAPI race_b(void *x) {
    (void)x;
    HANDLE h = CreateFileW(g_race, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, 0, NULL);
    char line[64];
    DWORD w;
    for (int i = 0; i < 5000; i++) { int n = snprintf(line, sizeof line, "B %05d jumps over the lazy dog\r\n", i); WriteFile(h, line, n, &w, NULL); }
    CloseHandle(h);
    return 0;
}

int main(void) {
    {   // log_append: concurrent appends keep every line
        GetTempPathW(MAX_PATH, g_race);
        wcscat(g_race, L"logappend_race.log");
        DeleteFileW(g_race);
        HANDLE t[2] = {CreateThread(NULL, 0, race_a, NULL, 0, NULL), CreateThread(NULL, 0, race_b, NULL, 0, NULL)};
        WaitForMultipleObjects(2, t, TRUE, INFINITE);
        CloseHandle(t[0]); CloseHandle(t[1]);
        CloseHandle(log_append_h); log_append_h = NULL;
        FILE *f = _wfopen(g_race, L"rb");
        static char buf[1 << 20];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        int a = 0, b = 0;
        for (char *q = buf; *q;) {
            if (q[0] == 'A' && q[1] == ' ') a++;
            if (q[0] == 'B' && q[1] == ' ') b++;
            char *e = strstr(q, "\r\n");
            if (!e) break;
            q = e + 2;
        }
        printf("log_append race: %d of 5000 A lines, %d of 5000 B lines\n", a, b);
        CHECK(a == 5000 && b == 5000);
        DeleteFileW(g_race);
    }
    GetTempPathW(MAX_PATH, g_log_path);
    wcscat(g_log_path, L"logtrim_test.log");
    // 1) many sessions, 2.4 MB: trimmed to the newest sessions
    FILE *f = _wfopen(g_log_path, L"wb");
    for (int i = 0; i < 300; i++) {
        fprintf(f, "\n======== session %04d ========\n", i);
        for (int k = 0; k < 100; k++) fprintf(f, "[00:00:00.000] session %04d line %03d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n", i, k);
    }
    fclose(f);
    long before = fsize();
    trim_log(g_log_path);
    long after = fsize();
    printf("many sessions: %ld -> %ld bytes\n", before, after);
    CHECK(before > LOG_MAX && after > 0 && after <= LOG_KEEP + 64);
    static char buf[LOG_KEEP + 128];
    f = _wfopen(g_log_path, L"rb");
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    CHECK(strncmp(buf, "(older lines removed", 20) == 0);
    char *first = strstr(buf, "\n======== session ");
    CHECK(first && first == strchr(buf, '\n') + 1);             // the kept part starts at a session header (blank line + header)
    CHECK(strstr(buf, "session 0299 line 099") != NULL);          // the newest session is complete
    CHECK(strstr(buf, "session 0000 line") == NULL);              // the oldest is gone
    // 2) a short log is left alone
    f = _wfopen(g_log_path, L"wb");
    fputs("\n======== session ========\nshort\n", f);
    fclose(f);
    before = fsize();
    trim_log(g_log_path);
    CHECK(fsize() == before);
    // 3) one session longer than LOG_KEEP and over LOG_MAX: no header inside the kept part, so its newest lines are kept
    f = _wfopen(g_log_path, L"wb");
    fputs("\n======== one long session ========\n", f);
    for (int k = 0; k < 30000; k++) fprintf(f, "[00:00:00.000] line %05d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n", k);
    fclose(f);
    before = fsize();
    trim_log(g_log_path);
    after = fsize();
    printf("one long session: %ld -> %ld bytes\n", before, after);
    CHECK(before > LOG_MAX && after > 0 && after <= LOG_KEEP + 64);
    f = _wfopen(g_log_path, L"rb");
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    CHECK(strncmp(buf, "(older lines removed", 20) == 0);
    CHECK(strncmp(strchr(buf, '\n') + 2, "[00:00:00.000] line ", 20) == 0);   // the kept part starts at a whole line
    CHECK(strstr(buf, "line 29999 ") != NULL);                     // the newest line is kept
    _wremove(g_log_path);
    printf("all log trim tests passed\n");
    return 0;
}
