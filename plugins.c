// Plugin loader (2026-11-09): every <game>\plugins\*.dll, in name order, once the fixes are installed - the same point of
// the game's start as a DLL in a hook slot such as xinput1_3.dll (both load with empire.retail.dll's imports, before any
// of its code runs) - so a mod can be smoke.dll instead of taking a slot another mod needs. heapfix.ini [mod] plugins=0
// switches it off (to rule plugins out of a crash report).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define MAX_PLUGINS 64

static int by_name(const void *a, const void *b) { return _wcsicmp((const wchar_t *)a, (const wchar_t *)b); }

// Returns how many loaded. Nothing is logged when there is no plugins folder or no DLL in it.
int load_plugins(const wchar_t *dir, void (*log)(const char *fmt, ...)) {
    wchar_t pat[MAX_PATH];
    if (swprintf(pat, MAX_PATH, L"%ls\\plugins\\*.dll", dir) < 0) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    static wchar_t names[MAX_PLUGINS][MAX_PATH];
    int n = 0, more = 0;
    do {
        // "*.dll" also matches "x.dll_old" through its 8.3 short name: only a real .dll (renaming one switches it off)
        const wchar_t *ext = wcsrchr(fd.cFileName, L'.');
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !ext || _wcsicmp(ext, L".dll")) continue;
        if (n < MAX_PLUGINS) wcscpy(names[n++], fd.cFileName); else more++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!n) return 0;
    qsort(names, n, sizeof names[0], by_name);   // FindFirstFile's order is the file system's: a fixed load order
    int ok = 0;
    for (int i = 0; i < n; i++) {
        wchar_t path[MAX_PATH];
        if (swprintf(path, MAX_PATH, L"%ls\\plugins\\%ls", dir, names[i]) < 0) continue;
        // the plugin's own DLLs are found next to it
        HMODULE m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m) { ok++; log("plugin loaded: plugins\\%ls", names[i]); }
        else log("plugin NOT loaded: plugins\\%ls (Windows error %lu)", names[i], GetLastError());
    }
    if (more) log("plugins: only the first %d (by name) loaded, %d more skipped", MAX_PLUGINS, more);
    log("plugins: %d of %d loaded (heapfix.ini [mod] plugins=0 switches them off)", ok, n);
    return ok;
}
