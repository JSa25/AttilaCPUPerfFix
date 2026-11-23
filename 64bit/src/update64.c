// update64: self-update from the Steam Workshop copy, ported from heapfix/update.c, whose own code does the work
// (the version marker, the completeness check of the downloaded file, the rename-and-replace of a loaded DLL): that
// file is included here unchanged. What differs is where the files are:
//   this plugin:        <library>\steamapps\common\<game>\attila64\plugins\perffix64.dll
//   the Workshop copy:  <library>\steamapps\workshop\content\325610\3824249154\perffix64.dll
// The Workshop item is the engine fix's own (3824249154). It does not carry a perffix64.dll yet: until one is uploaded
// there this finds no file and does nothing. It never downgrades, uses no network, and update_off.txt in the attila64
// folder switches it off.
#include "../out/gen/update.c"   // heapfix/update.c, beside the plugin's own version.h (tools/gen_shared64.py)

// me: this DLL's path. Returns 1 when a newer version was installed for the next start.
int update64_check_path(const wchar_t *me, void (*log)(const char *fmt, ...)) {
    wchar_t ws[MAX_PATH], off[MAX_PATH];
    wcscpy(off, me);
    for (int up = 0; up < 2; up++) {   // strip perffix64.dll, plugins: the attila64 folder
        wchar_t *s = wcsrchr(off, L'\\');
        if (!s) return 0;
        *s = 0;
    }
    if (wcslen(off) + 20 >= MAX_PATH) return 0;
    wcscat(off, L"\\update_off.txt");
    if (GetFileAttributesW(off) != INVALID_FILE_ATTRIBUTES) {
        log("update: update_off.txt found - no updates from the Steam Workshop (this plugin stays as installed)");
        return 0;
    }
    wcscpy(ws, me);
    for (int up = 0; up < 5; up++) {   // strip perffix64.dll, plugins, attila64, <game>, common
        wchar_t *s = wcsrchr(ws, L'\\');
        if (!s) return 0;
        *s = 0;
    }
    if (wcslen(ws) + 64 >= MAX_PATH) return 0;
    wcscat(ws, L"\\workshop\\content\\325610\\" WORKSHOP_ITEM L"\\perffix64.dll");
    int mine = file_version(me);   // read from our own file, the same way as the Workshop copy's
    return update_from(me, ws, mine ? mine : MOD_VERSION_NUM, log);
}

int update64_check(HMODULE self, void (*log)(const char *fmt, ...)) {
    wchar_t me[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, me, MAX_PATH);
    return n && n < MAX_PATH ? update64_check_path(me, log) : 0;
}
