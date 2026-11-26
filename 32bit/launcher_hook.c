// Total War Mod Manager hook for the engine fix (dinput8.dll; the same file fits the 64-bit plugin). At each game start: if Steam's
// Play button still runs CA's launcher (<game>\launcher\launcher.exe without "AttilaLauncher-version:"), Total War Mod Manager
// from the engine fix's Workshop item puts itself there (it runs with --install; CA's launcher is kept as launcher_ca.exe).
// That covers players who installed the engine fix before the launcher existed (their DLL updates itself, then this runs),
// and Steam's "Verify files" or a CA update writing CA's launcher.exe again. Nothing happens with launcher=2 in heapfix.ini
// ([mod], in the game folder: the player chose CA's launcher, in Total War Mod Manager or by hand), or when the game has no
// CA launcher folder. Cost when there is nothing to do: one read of the first 2 MB of launcher.exe.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int al_has_marker(const wchar_t *path) {   // 1 = the file carries "AttilaLauncher-version:<n>"
    static const char mark[] = "AttilaLauncher-version:";
    enum { CAP = 2 << 20 };
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    char *buf = HeapAlloc(GetProcessHeap(), 0, CAP);
    DWORD n = 0;
    BOOL ok = buf && ReadFile(f, buf, CAP, &n, NULL);
    CloseHandle(f);
    int found = 0;
    for (DWORD i = 0; ok && !found && i + sizeof mark < n; i++)
        if (buf[i] == 'A' && !memcmp(buf + i, mark, sizeof mark - 1) && buf[i + sizeof mark - 1] >= '1' && buf[i + sizeof mark - 1] <= '9') found = 1;
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    return found;
}

// self = this DLL (in the game folder, or in attila64\plugins for the 64-bit plugin)
void attila_launcher_check(HMODULE self, void (*log)(const char *fmt, ...)) {
    wchar_t game[MAX_PATH], p[MAX_PATH], ws[MAX_PATH], cmd[MAX_PATH * 3];
    DWORD n = GetModuleFileNameW(self, game, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    *wcsrchr(game, L'\\') = 0;
    swprintf(p, MAX_PATH, L"%ls\\Attila.exe", game);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) {   // the 64-bit plugin: <game>\attila64\plugins
        for (int up = 0; up < 2; up++) { wchar_t *s = wcsrchr(game, L'\\'); if (s) *s = 0; }
        swprintf(p, MAX_PATH, L"%ls\\Attila.exe", game);
        if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return;
    }
    swprintf(p, MAX_PATH, L"%ls\\heapfix.ini", game);
    if (GetPrivateProfileIntW(L"mod", L"launcher", 1, p) == 2) return;   // the player's choice: CA's launcher
    swprintf(p, MAX_PATH, L"%ls\\launcher\\launcher.exe", game);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES || al_has_marker(p)) return;   // no CA launcher here, or ours already
    wcscpy(ws, game);   // <steamapps>\common\<game> -> <steamapps>\workshop\content\325610\3824249154\TotalWarModManager.exe
    for (int up = 0; up < 2; up++) { wchar_t *s = wcsrchr(ws, L'\\'); if (!s) return; *s = 0; }
    if (wcslen(ws) + 80 >= MAX_PATH) return;
    wcscat(ws, L"\\workshop\\content\\325610\\3824249154");
    if (GetFileAttributesW(ws) == INVALID_FILE_ATTRIBUTES) return;   // installed by hand (Nexus, GitHub): no Workshop copy, nothing to do
    wcscat(ws, L"\\TotalWarModManager.exe");
    if (GetFileAttributesW(ws) == INVALID_FILE_ATTRIBUTES) { log("launcher: the Workshop copy has no TotalWarModManager.exe (yet)"); return; }
    swprintf(cmd, MAX_PATH * 3, L"\"%ls\" --install \"%ls\"", ws, game);
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof si;
    if (!CreateProcessW(ws, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, game, &si, &pi)) {
        log("launcher: could not run Total War Mod Manager's install (error %lu)", GetLastError());
        return;
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log(code ? "launcher: putting Total War Mod Manager in CA's launcher's place failed (code %lu); CA's launcher stays"
             : "launcher: Steam's Play button now opens Total War Mod Manager (CA's launcher kept as launcher\\launcher_ca.exe)", code);
}
