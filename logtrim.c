// Keeps heapfix.log short enough to post (AGENT_LOG 73): past LOG_MAX bytes, the oldest whole sessions are dropped so
// that about LOG_KEEP bytes of the newest remain (a session starts at a "========" header line). Called once per game
// start, before the header line is written.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "logtrim.h"

void trim_log(const wchar_t *path) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;
    long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (size <= LOG_MAX) { fclose(f); return; }
    static char keep[LOG_KEEP + 1];
    fseek(f, size - LOG_KEEP, SEEK_SET);
    size_t n = fread(keep, 1, LOG_KEEP, f);
    fclose(f);
    keep[n] = 0;
    char *start = strstr(keep, "\n======== ");  // the first session that starts inside the kept part
    if (!start) return;  // one session longer than LOG_KEEP: leave the file alone
    f = _wfopen(path, L"wb");
    if (!f) return;
    fputs("(older sessions removed to keep this log short)\n", f);
    fwrite(start, 1, n - (size_t)(start - keep), f);
    fclose(f);
}
