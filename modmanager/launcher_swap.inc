// ---- Steam's Play button runs <game>\launcher\launcher.exe. This puts Total War Mod Manager there and keeps CA's launcher as
// launcher_ca.exe beside it (Total War Mod Manager's "CA's launcher" button runs that). Used by --install (the engine fix's
// install.bat) and by the engine fix DLL at each game start, which also repairs it after Steam's "Verify files" or a CA
// update has written CA's launcher.exe again (the DLL skips it with launcher=2 in heapfix.ini). Which file is which: the
// Total War Mod Manager carries "AttilaLauncher-version:N". --uninstall (the engine fix's uninstall.bat) undoes it.
// Needs exists(), launcher_version() and pe_complete() (update.c) before it.

// src = a complete Total War Mod Manager exe. 1 = put in place, 0 = nothing to do, -1 = failed (CA's launcher stays usable).
static int launcher_swap(const char *game, const char *src) {
    char dir[MAX_PATH], cur[MAX_PATH], ca[MAX_PATH], old[MAX_PATH + 8];
    snprintf(dir, sizeof dir, "%s\\launcher", game);
    if (!exists(dir)) return 0;
    snprintf(cur, sizeof cur, "%s\\launcher.exe", dir);
    snprintf(ca, sizeof ca, "%s\\launcher_ca.exe", dir);
    wchar_t w[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, src, -1, w, MAX_PATH * 2);
    int theirs = launcher_version(src), mine = exists(cur) ? launcher_version(cur) : 0;
    if (!theirs || !pe_complete(w)) return -1;   // no launcher in the Workshop copy, or Steam is still downloading it
    if (mine >= theirs) return 0;               // ours, up to date (it also updates itself from the Workshop)
    if (exists(cur)) {
        if (!mine) { if (!MoveFileExA(cur, ca, MOVEFILE_REPLACE_EXISTING)) return -1; }   // CA's (maybe a newer one of CA's)
        else {   // an older copy of ours, maybe running: moved aside (deleted the next time)
            snprintf(old, sizeof old, "%s.old", cur);
            DeleteFileA(old);
            if (!MoveFileExA(cur, old, MOVEFILE_REPLACE_EXISTING)) return -1;
        }
    }
    if (CopyFileA(src, cur, FALSE) && launcher_version(cur) == theirs) return 1;
    DeleteFileA(cur);
    if (!mine && exists(ca)) MoveFileExA(ca, cur, 0);   // CA's launcher back where Steam looks
    return -1;
}

// The engine fix is uninstalled: CA's launcher goes back where Steam looks and Total War Mod Manager's exe is deleted (its
// settings and presets in TotalWarModManager\ stay, for a reinstall). 1 = done, or nothing to do; 0 = failed.
static int launcher_restore_ca(const char *game) {
    char cur[MAX_PATH], ca[MAX_PATH], old[MAX_PATH + 8];
    snprintf(cur, sizeof cur, "%s\\launcher\\launcher.exe", game);
    snprintf(ca, sizeof ca, "%s\\launcher\\launcher_ca.exe", game);
    snprintf(old, sizeof old, "%s.old", cur);
    DeleteFileA(old);
    if (exists(cur) && !launcher_version(cur)) {   // CA's launcher is in place already (Steam's "Verify files"): a stale copy
        if (exists(ca)) DeleteFileA(ca);
        return 1;
    }
    if (!exists(ca)) return !exists(cur);   // ours without CA's beside it: nothing to put back (Steam's "Verify files" does)
    if (exists(cur) && !MoveFileExA(cur, old, MOVEFILE_REPLACE_EXISTING)) return 0;   // a running exe can be renamed
    if (!MoveFileExA(ca, cur, 0)) { MoveFileExA(old, cur, 0); return 0; }
    DeleteFileA(old);   // still running: it stays as launcher.exe.old, which the next install deletes
    return 1;
}
