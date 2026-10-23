// Self-update from the Steam Workshop copy. Steam keeps steamapps\workshop\content\325610\3824249154\dinput8.dll
// up to date, but players forget to run install.bat again. At startup this compares the version marker in that file
// with ours; if it is higher, the new file replaces the one in the game folder for the next launch (a loaded DLL can
// be renamed: ours becomes dinput8.dll.old, deleted on the next start). It never downgrades, ignores files without
// the marker, uses no network, and on any failure keeps the current DLL and says so in the log.
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "version.h"
#define WORKSHOP_ITEM L"3824249154"
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

// The game folder is steamapps\common\<game>; the Workshop copy lives in the same library's steamapps\workshop.
void update_check(HMODULE self, void (*log)(const char *fmt, ...)) {
    wchar_t me[MAX_PATH], ws[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, me, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wcscpy(ws, me);
    for (int up = 0; up < 3; up++) {  // strip dinput8.dll, <game>, common
        wchar_t *s = wcsrchr(ws, L'\\');
        if (!s) return;
        *s = 0;
    }
    if (wcslen(ws) + 64 >= MAX_PATH) return;
    wcscat(ws, L"\\workshop\\content\\325610\\" WORKSHOP_ITEM L"\\dinput8.dll");
    // Our version is read from our own file, the same way as the Workshop copy's, so the comparison can never
    // disagree with the marker and re-install the same file on every start.
    int mine = file_version(me);
    update_from(me, ws, mine ? mine : MOD_VERSION_NUM, log);
}
