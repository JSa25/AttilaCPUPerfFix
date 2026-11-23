// The flag wave's pack switch. A flag pack says it is one with a single file inside it:
//
//     flagwave\flag_wave.txt        first character 1 = on, 0 = off
//
// At game start the loaded mod packs are asked for that file, in load order, and the first pack that has it decides.
// No pack has it: the flag wave is not installed at all - no shader of ours, no hook, no buffer looked at. That is the
// whole test; no mesh, vertex buffer or matrix is read to decide it. The file travels with the pack: a renamed pack
// keeps it, and a pack merged into another brings it along like any other file (wip/flagmerge/merge_waved.py).
// Read with plain file calls: only the index of each pack (11 MB over the 13 packs of 1212 AD, 11 ms from the cache).
// Included by bridgeprobe.c, not linked (as flagsim.c is); fwpack_test.c builds it as source too.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FWP_NAME "flagwave\\flag_wave.txt"

// 1 / 0: the pack carries the switch file and that is what it says; -1: no such file in it (or not a pack we read)
static int fwp_read(const wchar_t *pack) {
    FILE *f = _wfopen(pack, L"rb");
    if (!f) return -1;
    int r = -1;
    unsigned h[7];
    unsigned char *idx = NULL;
    // PFH4 (tools/packlist.py): magic, flags, pack-list count / size, file count, index size, time; 0x90 = encrypted
    if (fread(h, 4, 7, f) == 7 && h[0] == 0x34484650 && !(h[1] & 0x90) && h[5] > 0 && h[5] < (64u << 20) &&
        !fseek(f, 28 + (long)h[3], SEEK_SET) && (idx = malloc(h[5] + 1)) != NULL && fread(idx, 1, h[5], f) == h[5]) {
        idx[h[5]] = 0;
        unsigned long long off = 28ull + h[3] + h[5];
        unsigned pos = 0, skip = (h[1] & 0x40) ? 8 : 4;   // each entry: size, (a time,) the name
        for (unsigned i = 0; i < h[4] && pos + skip < h[5]; i++) {
            unsigned size;
            memcpy(&size, idx + pos, 4);
            const char *name = (const char *)idx + pos + skip;
            size_t nl = strlen(name);
            if (nl == sizeof FWP_NAME - 1) {
                size_t k = 0;
                for (; k < nl; k++) {
                    char a = name[k] == '/' ? '\\' : name[k];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (a != FWP_NAME[k]) break;
                }
                if (k == nl) {
                    int c = EOF;
                    if (size && !_fseeki64(f, (long long)off, SEEK_SET)) c = fgetc(f);
                    r = c == '1';   // anything but a 1 is off: an empty or damaged file must not turn it on
                    break;
                }
            }
            pos += skip + (unsigned)nl + 1;
            off += size;
        }
    }
    free(idx);
    fclose(f);
    return r;
}

// the value between the quotes of the next `key "value";` line at or after *q, NULL when there is no more
static const char *fwp_line(const char **q, const char *key, int *len) {
    size_t kl = strlen(key);
    for (const char *p = *q; (p = strstr(p, key)) != NULL; p++) {
        if ((p != *q && p[-1] != '\n' && p[-1] != '\r') || p[kl] != ' ' || p[kl + 1] != '"') continue;   // a whole line
        const char *e = strchr(p + kl + 2, '"');
        if (!e || e - (p + kl + 2) > 250) continue;
        *len = (int)(e - (p + kl + 2));
        *q = e;
        return p + kl + 2;
    }
    return NULL;
}

// The switch for a mod list (diag_mods_text): 1 on, 0 off, -1 no loaded pack has the file. who gets the deciding
// pack's name. A mod is looked for in <game>\data, then in each add_working_directory folder, as the game does.
static int fwp_from_list(const char *text, const wchar_t *gamedir, char *who, int wn) {
    if (who && wn) who[0] = 0;
    const char *q = text;
    int ml;
    for (const char *m; (m = fwp_line(&q, "mod", &ml)) != NULL;) {
        wchar_t wname[260], path[MAX_PATH * 2];
        int wl = MultiByteToWideChar(CP_UTF8, 0, m, ml, wname, 259);
        if (wl <= 0) continue;
        wname[wl] = 0;
        _snwprintf(path, MAX_PATH * 2, L"%ls\\data\\%ls", gamedir, wname);
        int r = fwp_read(path);
        const char *d = text;
        int dl;
        for (const char *wd; r < 0 && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES && (wd = fwp_line(&d, "add_working_directory", &dl)) != NULL;) {
            wchar_t wdir[MAX_PATH];
            int n = MultiByteToWideChar(CP_UTF8, 0, wd, dl, wdir, MAX_PATH - 1);
            if (n <= 0) continue;
            wdir[n] = 0;
            _snwprintf(path, MAX_PATH * 2, L"%ls\\%ls", wdir, wname);
            r = fwp_read(path);
        }
        if (r >= 0) {
            if (who && wn) _snprintf(who, wn - 1, "%.*s", ml, m), who[wn - 1] = 0;
            return r;
        }
    }
    return -1;
}
