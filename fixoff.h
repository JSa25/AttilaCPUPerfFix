// heapfix.ini's reader and the on / off switch of a fix (heapfix.c). ini_test.c and fwswitch_test.c include this same
// file: both functions used to be cut out of the sources with sed for their tests, and the flag wave had a second copy
// of the switch rule of its own in bridgeprobe.c.
// The including file provides: wchar_t g_ini[] (heapfix.ini's path, empty: none), wchar_t g_log_path[] (heapfix.log's
// path: the game folder is taken from it) and void wlog(const char *fmt, ...).
#pragma once
// numbers, or on / true / yes = 1 and off / false / no = 0 (GetPrivateProfileIntW read "on" as 0 = the fix off;
// ultrareview 2026-11-02); anything else keeps the default
static int ini_int(const wchar_t *section, const wchar_t *key, int def) {
    wchar_t v[32];
    if (!g_ini[0] || !GetPrivateProfileStringW(section, key, L"", v, 32, g_ini)) return def;
    if ((v[0] >= L'0' && v[0] <= L'9') || v[0] == L'-') return _wtoi(v);
    static const wchar_t *const yes[3] = {L"on", L"true", L"yes"}, *const no[3] = {L"off", L"false", L"no"};
    for (int i = 0; i < 3; i++) {
        if (!_wcsicmp(v, yes[i])) return 1;
        if (!_wcsicmp(v, no[i])) return 0;
    }
    return def;
}
// 1 when a fix is switched off: <key>=0 in [fixes] of heapfix.ini, or the older <name>_off.txt in the game folder.
static __attribute__((unused)) int fix_off(const wchar_t *key, const wchar_t *old_file, const char *name, const char *then) {
    wchar_t p[MAX_PATH];
    wcscpy(p, g_log_path);
    wchar_t *sl = wcsrchr(p, L'\\');
    int file = sl && wcslen(old_file) + (sl + 1 - p) < MAX_PATH && (wcscpy(sl + 1, old_file), GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES);
    if (ini_int(L"fixes", key, 1) && !file) return 0;
    wlog("%s switched off (heapfix.ini or %ls) - %s", name, old_file, then);
    return 1;
}
