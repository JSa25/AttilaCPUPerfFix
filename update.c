// Self-update from the Steam Workshop copy. Steam keeps steamapps\workshop\content\325610\3824249154\dinput8.dll
// up to date, but players forget to run install.bat again. At startup this compares the version marker in that file
// with ours; if it is higher, the new file replaces the one in the game folder for the next launch (a loaded DLL can
// be renamed: ours becomes dinput8.dll.old, deleted on the next start). It never downgrades, ignores files without
// the marker, uses no network, and on any failure keeps the current DLL and says so in the log.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "version.h"
#define WORKSHOP_ITEM L"3824249154"
#define ADDON_ITEM L"3828791891"   // the add-on, "Upscaling and Framegen for Attila"
static const char MARKER[] = "AttilaCPUPerfFix-version:";
#define STR_(x) #x
#define STR(x) STR_(x)
__attribute__((used)) static const char g_marker[] = "AttilaCPUPerfFix-version:" STR(MOD_VERSION_NUM);

// The version marker in a file, or 0 (missing, unreadable, not a PE file, or no marker).
static int file_version(const wchar_t *path) {
    static char buf[2 << 20];
    FILE *f = _wfopen(path, L"rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 0x400 || buf[0] != 'M' || buf[1] != 'Z') return 0;
    for (size_t i = 0; i + sizeof MARKER <= n; i++)  // room for the prefix and at least one digit
        if (buf[i] == 'A' && memcmp(buf + i, MARKER, sizeof MARKER - 1) == 0) {
            int v = 0;
            for (size_t j = i + sizeof MARKER - 1; j < n && buf[j] >= '0' && buf[j] <= '9' && v < 100000; j++)
                v = v * 10 + (buf[j] - '0');
            if (v) return v;  // the bare prefix (MARKER itself) is in the file too; skip it
        }
    return 0;
}

// self = this DLL's path, ws = the Workshop copy. Returns 1 if the game folder now holds the newer version.
int update_from(const wchar_t *self, const wchar_t *ws, int mine, void (*log)(const char *fmt, ...)) {
    wchar_t old_[MAX_PATH], new_[MAX_PATH];
    if (wcslen(self) + 5 >= MAX_PATH) return 0;
    swprintf(old_, MAX_PATH, L"%ls.old", self);
    swprintf(new_, MAX_PATH, L"%ls.new", self);
    DeleteFileW(old_);  // left by the previous update; not loaded any more
    if (GetFileAttributesW(ws) == INVALID_FILE_ATTRIBUTES) return 0;  // not subscribed on the Workshop
    int theirs = file_version(ws);
    if (theirs <= mine) return 0;
    if (!CopyFileW(ws, new_, FALSE) || file_version(new_) != theirs) {
        log("update: the Workshop has version %d, but copying it failed (error %lu) - run install.bat in the Workshop "
            "folder to update", theirs, GetLastError());
        DeleteFileW(new_);
        return 0;
    }
    if (!MoveFileExW(self, old_, MOVEFILE_REPLACE_EXISTING)) {
        log("update: the Workshop has version %d, but this DLL could not be renamed (error %lu) - run install.bat",
            theirs, GetLastError());
        DeleteFileW(new_);
        return 0;
    }
    if (!MoveFileExW(new_, self, MOVEFILE_REPLACE_EXISTING)) {
        DWORD e = GetLastError();
        MoveFileExW(old_, self, MOVEFILE_REPLACE_EXISTING);  // put ours back
        DeleteFileW(new_);
        log("update: installing version %d failed (error %lu) - run install.bat", theirs, e);
        return 0;
    }
    log("update: installed version %d from the Steam Workshop; it takes effect the next time the game starts", theirs);
    return 1;
}

// The add-on (Workshop item 3828791891) is copied into <game>\FrameBridge by its install.bat, which players forget to
// run again after Steam updates the item. Its version is the number in FrameBridge\version.txt ("addon_version=N",
// written by tools/pack_framebridge.sh; none = 0). If the add-on is installed and the Workshop copy's number is higher,
// all its files are copied over, version.txt last, so a failed update is tried again next start. Never downgrades
// (an older Workshop copy, a newer Nexus or test install stays). Files only in the game folder (settings.txt, the
// logs) are left alone. sync_dir returns the number of files that could not be copied.
static int addon_version(const wchar_t *dir) {
    wchar_t p[MAX_PATH];
    char buf[64] = {0};
    if (swprintf(p, MAX_PATH, L"%ls\\version.txt", dir) < 0) return 0;
    FILE *f = _wfopen(p, L"rb");
    if (!f) return 0;
    fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    const char *v = strstr(buf, "addon_version=");
    return v ? atoi(v + 14) : 0;
}

static int sync_dir(const wchar_t *dst, const wchar_t *src, int *copied) {
    wchar_t pat[MAX_PATH], s[MAX_PATH], d[MAX_PATH];
    if (swprintf(pat, MAX_PATH, L"%ls\\*", src) < 0) return 1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    int failed = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !_wcsicmp(fd.cFileName, L"version.txt")) continue;
        if (swprintf(s, MAX_PATH, L"%ls\\%ls", src, fd.cFileName) < 0 || swprintf(d, MAX_PATH, L"%ls\\%ls", dst, fd.cFileName) < 0) {
            failed++;
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CreateDirectoryW(d, NULL);
            failed += sync_dir(d, s, copied);
            continue;
        }
        if (CopyFileW(s, d, FALSE)) (*copied)++;
        else failed++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return failed;
}

// game_fb = <game>\FrameBridge, ws_fb = the add-on's Workshop FrameBridge folder. Returns the number of files copied.
int addon_update_from(const wchar_t *game_fb, const wchar_t *ws_fb, void (*log)(const char *fmt, ...)) {
    if (GetFileAttributesW(game_fb) == INVALID_FILE_ATTRIBUTES) return 0;  // add-on not installed: never install it
    if (GetFileAttributesW(ws_fb) == INVALID_FILE_ATTRIBUTES) return 0;    // not subscribed (e.g. installed from Nexus)
    int theirs = addon_version(ws_fb), mine = addon_version(game_fb);
    if (theirs <= mine) return 0;
    wchar_t s[MAX_PATH], d[MAX_PATH];
    int copied = 0, failed = sync_dir(game_fb, ws_fb, &copied);
    if (!failed && (swprintf(s, MAX_PATH, L"%ls\\version.txt", ws_fb) < 0 || swprintf(d, MAX_PATH, L"%ls\\version.txt", game_fb) < 0 ||
                    !CopyFileW(s, d, FALSE)))
        failed++;
    if (failed)
        log("update: add-on version %d from the Steam Workshop: %d file(s) could not be copied into FrameBridge - run the "
            "add-on's install.bat", theirs, failed);
    else
        log("update: add-on version %d installed from the Steam Workshop (was %d, %d files)", theirs, mine, copied + 1);
    return failed ? 0 : copied + 1;
}

// The game folder is steamapps\common\<game>; the Workshop copy lives in the same library's steamapps\workshop.
void update_check(HMODULE self, void (*log)(const char *fmt, ...)) {
    wchar_t me[MAX_PATH], ws[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, me, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    // Windows reports the name as the game imports it ("DINPUT8.dll"); the update would be saved under that casing
    wchar_t *fn = wcsrchr(me, L'\\');
    if (!fn || fn + 12 >= me + MAX_PATH) return;
    // update_off.txt in the game folder: nothing is taken from the Workshop (a dev or other build installed by hand
    // stays, whatever version the Workshop copies have)
    wcscpy(fn + 1, L"update_off.txt");
    if (GetFileAttributesW(me) != INVALID_FILE_ATTRIBUTES) {
        log("update: update_off.txt found - no updates from the Steam Workshop (this DLL and the add-on stay as installed)");
        return;
    }
    wcscpy(fn + 1, L"dinput8.dll");
    wcscpy(ws, me);
    for (int up = 0; up < 3; up++) {  // strip dinput8.dll, <game>, common
        wchar_t *s = wcsrchr(ws, L'\\');
        if (!s) return;
        *s = 0;
    }
    if (wcslen(ws) + 64 >= MAX_PATH) return;
    wchar_t game_fb[MAX_PATH], ws_fb[MAX_PATH];
    swprintf(ws_fb, MAX_PATH, L"%ls\\workshop\\content\\325610\\" ADDON_ITEM L"\\FrameBridge", ws);
    wcscat(ws, L"\\workshop\\content\\325610\\" WORKSHOP_ITEM L"\\dinput8.dll");
    // Our version is read from our own file, the same way as the Workshop copy's, so the comparison can never
    // disagree with the marker and re-install the same file on every start.
    int mine = file_version(me);
    // a new DLL first: the add-on's new files may need it, so they follow on the next start, when it runs
    if (update_from(me, ws, mine ? mine : MOD_VERSION_NUM, log)) return;
    wcscpy(game_fb, me);
    wcscpy(wcsrchr(game_fb, L'\\') + 1, L"FrameBridge");
    addon_update_from(game_fb, ws_fb, log);
}
