// Tests logtrim.c (AGENT_LOG 73): a log over 1 MB keeps only the newest whole sessions (~256 KB), with a note at the
// top; a short log and one giant session are left alone.
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

int main(void) {
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
    CHECK(strncmp(buf, "(older sessions removed", 23) == 0);
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
    // 3) one session longer than LOG_KEEP and over LOG_MAX: no header inside the kept part, so the file stays
    f = _wfopen(g_log_path, L"wb");
    fputs("\n======== one long session ========\n", f);
    for (int k = 0; k < 30000; k++) fprintf(f, "[00:00:00.000] line %05d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n", k);
    fclose(f);
    before = fsize();
    trim_log(g_log_path);
    CHECK(before > LOG_MAX && fsize() == before);
    _wremove(g_log_path);
    printf("all log trim tests passed\n");
    return 0;
}
