// Portions of this file (the interface-size choices (g_sizes) and the name=value line reading) come from Sol Invictus's 64-bit Attila (attila64/loader/picker.c) and are under its MIT license: see SOL_INVICTUS_MIT.txt.
// Total War Mod Manager (JGdaCaesar, 2026-11-25): one window for normal 32-bit Total War: ATTILA and Sol Invictus's 64-bit Attila.
// It sits where Steam's Play button looks (<game>\launcher\launcher.exe; CA's launcher is kept as launcher_ca.exe), so Play
// opens it with no Launch Options and no Steam restart. Before the game starts it takes newer files from the Steam Workshop
// (the engine fix, its 64-bit plugin, the upscaling add-on and this launcher), so an update works on the same Play.
//   32-bit: the list is the game folder's used_mods.txt (CA's launcher uses the same file); Play starts Attila.exe with it.
//   64-bit: the list is attila64\mod_list.txt and the settings attila64\settings.txt, both in 64-bit Attila's own format;
//           Play starts attila64.exe with ATTILA_NO_PICKER=1 (its published switch: no second window). "64-bit Attila's own
//           window" starts it without the switch. Nothing of Sol's code is in here.
// Command line: --install <game folder>  puts this launcher in CA's spot (the engine fix's install.bat runs it).
// The window itself is in the .inc files (shared with the start window made for 64-bit Attila).
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#ifndef LAUNCHER_VERSION
#define LAUNCHER_VERSION 100   // 1.00; the number only grows (tests build others with -DLAUNCHER_VERSION=n)
#endif
#define APP_VERSION_TEXT "1.0"
#define STR_(x) #x
#define STR(x) STR_(x)
__attribute__((used)) static const char g_launcher_marker[] = "AttilaLauncher-version:" STR(LAUNCHER_VERSION);
static const char LMARK[] = "AttilaLauncher-version:";
#define ENGINE_ITEM "3824249154"   // the engine fix's Workshop item; it carries this launcher too
#define SOL_ITEM "3834958414"      // 64-bit Attila on the Workshop
#define SOL_ITEM_ID 3834958414ULL
#define SOL_TESTED_MAJOR 1         // 64-bit Attila versions this launcher drives with its own window; newer: its own window
#define SOL_TESTED_MINOR 3

#define MAX_MODS 2000
typedef struct {
    char name[160]; char dir[MAX_PATH]; int on;
    unsigned long long ws;        // Workshop item number, 0 = game data folder or a folder of the player's own
    unsigned long long size;      // bytes
    FILETIME time;                // last written
    int missing;                  // in a list (ticked) but not on this PC: a row that waits for the Workshop download
    unsigned long long fp; int fp_done;   // its version for the list code (list_version), read once
    int under_n, under_by, over_n, over_of;   // conflict hints, by row number; -1 = not known yet
    void *thumb; BYTE *thumb_px; int thumb_size, thumb_state;   // small picture: GDI+ bitmap over thumb_px; 0 not asked, 1 asked, 2 have, 3 none
} Mod;
static Mod *g_mods;
static int g_nmods;
static int g_result;

// ---- Folders. The game folder is the working folder while the launcher runs (the list files name packs relative to it).
static char g_game[MAX_PATH];    // the game folder (Attila.exe)
static char g_home[MAX_PATH];    // <game>\TotalWarModManager: this launcher's settings, presets and log
static char g_exe[MAX_PATH];     // this exe
static int g_mode;               // 0 = 32-bit, 1 = 64-bit
static int g_have64;             // 64-bit Attila is installed (attila64\attila64.exe)
static int g_sol_ok;             // its version is one this launcher drives with its own window
static int g_sol_always;         // Play 64-bit always opens 64-bit Attila's own window (this one is skipped; Shift shows it)

static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static void joinp(char *out, size_t n, const char *a, const char *b) { snprintf(out, n, "%s\\%s", a, b); }

static int find_game(void) {
    char p[MAX_PATH], t[MAX_PATH + 64];
    const char *over = getenv("ATTILA_GAME_DIR");   // tests
    if (over) { snprintf(g_game, sizeof g_game, "%s", over); joinp(t, sizeof t, g_game, "Attila.exe"); return exists(t); }
    snprintf(p, sizeof p, "%s", g_exe);
    for (int up = 0; up < 2; up++) {   // <game>\launcher\launcher.exe, or next to Attila.exe
        char *s = strrchr(p, '\\');
        if (!s) break;
        *s = 0;
        joinp(t, sizeof t, p, "Attila.exe");
        if (exists(t)) { snprintf(g_game, sizeof g_game, "%s", p); return 1; }
    }
    // the copy in the engine fix's Workshop item, started by hand: <steamapps>\workshop\content\325610\<item>\this exe
    snprintf(p, sizeof p, "%s", g_exe);
    for (int up = 0; up < 5; up++) { char *s = strrchr(p, '\\'); if (!s) return 0; *s = 0; }
    snprintf(t, sizeof t, "%s\\common\\Total War Attila\\Attila.exe", p);
    if (exists(t)) { snprintf(g_game, sizeof g_game, "%s\\common\\Total War Attila", p); return 1; }
    return 0;
}

// ---- name=value files: this launcher's launcher.ini and 64-bit Attila's settings.txt (the last line of a name counts)
static int ini_get(const char *path, const char *name, char *out, size_t size) {
    char line[512];
    size_t nl = strlen(name);
    int found = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        if (_strnicmp(line, name, nl) || line[nl] != '=') continue;
        line[strcspn(line, "\r\n")] = 0;
        snprintf(out, size, "%s", line + nl + 1);
        found = 1;
    }
    fclose(f);
    return found;
}
static void ini_put(const char *path, const char *name, const char *value) {   // keeps every other line; writes only on a change
    char old[512];
    if (ini_get(path, name, old, sizeof old) && !strcmp(old, value)) return;
    size_t cap = 4096, len = 0, nl = strlen(name);
    char *text = malloc(cap), line[512];
    if (!text) return;
    text[0] = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0] || (!_strnicmp(line, name, nl) && line[nl] == '=')) continue;
            size_t add = strlen(line) + 1;
            if (len + add + 1 > cap) { char *n = realloc(text, cap *= 2); if (!n) { fclose(f); free(text); return; } text = n; }
            memcpy(text + len, line, add - 1);
            text[len + add - 1] = '\n';
            len += add;
            text[len] = 0;
        }
        fclose(f);
    }
    f = fopen(path, "w");
    if (f) { fputs(text, f); fprintf(f, "%s=%s\n", name, value); fclose(f); }
    free(text);
}
static char g_ini[MAX_PATH], g_sol_ini[MAX_PATH];
static int settings_get(const char *name, char *out, size_t size) { return ini_get(g_ini, name, out, size); }
static void settings_put(const char *name, const char *value) { ini_put(g_ini, name, value); }

// ---- 64-bit Attila's settings, written in its own format (the keys its start window writes)
static const struct { const char *text; int percent; } g_sizes[] = {
    {"100%", 100}, {"Auto (like 1080p)", 0}, {"125%", 125}, {"150%", 150}, {"175%", 175}, {"200%", 200}, {"250%", 250}};
enum { N_SIZES = sizeof g_sizes / sizeof g_sizes[0] };
static const char *const g_sharp_texts[] = {"Game setting", "Off", "2x", "4x", "8x", "16x"};
enum { N_SHARP = sizeof g_sharp_texts / sizeof g_sharp_texts[0] };
static int sol_get(const char *name, char *out, size_t size) { return ini_get(g_sol_ini, name, out, size); }
static int settings_size_index(void) {
    char v[64];
    if (!sol_get("interface_size", v, sizeof v) || !_strnicmp(v, "like1080p", 9) || !_strnicmp(v, "auto", 4)) return 1;
    for (int i = 0; i < N_SIZES; i++) if (atoi(v) > 0 && g_sizes[i].percent == atoi(v)) return i;
    return 1;
}
static void settings_save_size(int i) {
    char v[16];
    snprintf(v, sizeof v, "%d", g_sizes[i < 0 || i >= N_SIZES ? 0 : i].percent);
    ini_put(g_sol_ini, "interface_size", i == 1 ? "like1080p" : v);
}
static int sol_int(const char *name, int dflt, int lo, int hi) {
    char v[64];
    if (!sol_get(name, v, sizeof v) || v[0] < '0' || v[0] > '9') return dflt;
    int n = atoi(v);
    return n < lo || n > hi ? dflt : n;
}
static int sol_flag(const char *name, int dflt) { char v[16]; return sol_get(name, v, sizeof v) ? v[0] != '0' : dflt; }

// ---- The engine fix and its add-ons
enum { AD_PERF, AD_UPSCALE, AD_SHARP, N_ADDONS };
static const unsigned long long g_addon_ws[N_ADDONS] = {3824249154ULL, 3828791891ULL, 3845260607ULL};
static const char *const g_addon_name[N_ADDONS] = {"Engine performance fix", "Upscaling and frame generation", "Sharp 4K Interface"};
static void workshop_root(char *out, size_t size) {   // <steamapps>\workshop\content\325610 with / (the working folder is the game's)
    if (!GetFullPathNameA("..\\..\\workshop\\content\\325610", (DWORD)size, out, NULL)) out[0] = 0;
    for (char *c = out; *c; c++) if (*c == '\\') *c = '/';
}
// which = 0: the installed copy (32-bit: <game>\dinput8.dll; 64-bit: attila64\plugins\perffix64.dll), 1: the Workshop copy
static void perffix_path(int which, char *out, size_t size) {
    char root[MAX_PATH];
    workshop_root(root, sizeof root);
    if (which) snprintf(out, size, "%s/" ENGINE_ITEM "/%s", root, g_mode ? "perffix64.dll" : "dinput8.dll");
    else if (g_mode) snprintf(out, size, "%s\\attila64\\plugins\\perffix64.dll", g_game);
    else snprintf(out, size, "%s\\dinput8.dll", g_game);
}
static int marker_version(const char *path, const char *mark) {   // "<mark><number>" in the first 2 MB of a PE file; 0 = none
    enum { CAP = 2 << 20 };
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char *buf = malloc(CAP);
    size_t n = buf ? fread(buf, 1, CAP, f) : 0, ml = strlen(mark);
    fclose(f);
    int v = 0;
    if (n >= 0x400 && buf[0] == 'M' && buf[1] == 'Z')
        for (size_t i = 0; !v && i + ml < n; i++)
            if (buf[i] == mark[0] && !memcmp(buf + i, mark, ml))
                for (size_t j = i + ml; j < n && buf[j] >= '0' && buf[j] <= '9' && v < 100000; j++) v = v * 10 + (buf[j] - '0');
    free(buf);
    return v;
}
int perffix_version(const char *path) { return marker_version(path, "AttilaCPUPerfFix-version:"); }
static int launcher_version(const char *path) { return marker_version(path, LMARK); }

static int addon_on_disk(int a) {
    char p[MAX_PATH * 2];
    if (a == AD_PERF) {
        for (int i = 0; i < 2; i++) { perffix_path(i, p, sizeof p); if (exists(p)) return 1; }
        if (!g_mode) { joinp(p, sizeof p, g_game, "dinput8.dll.off"); return exists(p); }
        return 0;
    }
    if (a == AD_UPSCALE) {
        joinp(p, sizeof p, g_game, "FrameBridge\\helper64.exe");
        if (exists(p)) return 1;
        char root[MAX_PATH];
        workshop_root(root, sizeof root);
        snprintf(p, sizeof p, "%s/3828791891/FrameBridge/helper64.exe", root);
        return exists(p);
    }
    joinp(p, sizeof p, g_game, "attila64\\ui4k");
    if (exists(p)) return 1;
    char root[MAX_PATH];
    workshop_root(root, sizeof root);
    snprintf(p, sizeof p, "%s/3845260607/ui4k/attila64_sharp_ui.txt", root);
    return exists(p);
}
// The upscaling add-on's own switch: addon=on|off in <game>\FrameBridge\settings.txt
static int addon_upscale_on(void) {
    char p[MAX_PATH], v[32];
    joinp(p, sizeof p, g_game, "FrameBridge\\settings.txt");
    return !(ini_get(p, "addon", v, sizeof v) && !_stricmp(v, "off"));
}

// ---- Logs: this launcher's own (TotalWarModManager\launcher.log), and the last game's for the crash note and "Copy last log"
static void lg(const char *fmt, ...) {
    char p[MAX_PATH];
    joinp(p, sizeof p, g_home, "launcher.log");
    FILE *f = fopen(p, "a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
static void lgw(const char *fmt, ...) {   // for the engine fix's update code (its messages)
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    lg("%s", b);
}
static void last_log_path(char *out, size_t size) {   // 64-bit: attila64_log.txt; 32-bit: the engine fix's heapfix.log
    if (g_mode) snprintf(out, size, "%s\\attila64\\attila64_log.txt", g_game);
    else snprintf(out, size, "%s\\heapfix.log", g_game);
}
static char *last_log_text(void) {
    char path[MAX_PATH];
    last_log_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 8u << 20, n = 0;
    char *text = malloc(cap + 1);
    if (text) { n = fread(text, 1, cap, f); text[n] = 0; }
    fclose(f);
    if (text && !n) { free(text); text = NULL; }
    return text;
}
static int last_game_crashed(void) {   // 64-bit Attila writes "CRASH ..." lines into its log
    if (!g_mode) return 0;
    char *t = last_log_text();
    int c = t && (!strncmp(t, "CRASH ", 6) || strstr(t, "\nCRASH "));
    free(t);
    return c;
}

static unsigned long long list_version(const char *name);   // a ticked pack's version, for the list code
static void have64_check(void);                              // g_have64 and g_sol_ok from attila64\attila64.exe
#include "picker_lists.inc"
#include "picker_packs.inc"
#include "picker_gfx.inc"

static void ui_toast(int kind, const char *msg);
static void copy_text(HWND w, const char *utf8);
static void copy_last_log(HWND w) {
    char *text = last_log_text();
    if (!text) { ui_toast(1, "There is no log yet. One is written each time the game runs."); return; }
    copy_text(w, text);
    free(text);
    ui_toast(0, "The log of the last game is copied: paste it (Ctrl+V) where you report the problem.");
}
static void open_explorer(const char *args) {   // a folder, a file (its own program), a link or a steam:// address
    char exe[MAX_PATH], cmd[MAX_PATH * 3 + 64];
    GetWindowsDirectoryA(exe, MAX_PATH - 16);
    strcat(exe, "\\explorer.exe");   // Windows' own, never one in the game folder
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, args);
    lg("open: %s", args);
    if (getenv("ATTILA_PICKER_TEST")) return;   // tests read the line above
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof si;
    if (CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}
// ---- Which launcher Steam's Play opens: heapfix.ini (the engine fix's settings, in the game folder), [mod] launcher=
// 1 = this one (also when the line is missing), 2 = CA's launcher. The engine fix DLL reads the same line
// (dll/launcher_hook.c): with 2 it leaves launcher\launcher.exe alone. A player can always change it by hand.
static const char LAUNCHER_NOTE[] = "; which launcher Steam's Play button opens: 1 = Total War Mod Manager, 2 = CA's launcher";
static void fix_ini(char *out, size_t n) { joinp(out, n, g_game, "heapfix.ini"); }
static int launcher_choice(void) {
    char p[MAX_PATH];
    fix_ini(p, sizeof p);
    return GetPrivateProfileIntA("mod", "launcher", 1, p) == 2 ? 2 : 1;
}
// Writes launcher=<v>. A heapfix.ini without the line gets it, with its note, at the top of [mod] (the section, or the file,
// is made when missing; only_if_file: not when there is no heapfix.ini). 1 = written.
static int launcher_choice_put(int v, int only_if_file) {
    char p[MAX_PATH], val[8], have[8];
    fix_ini(p, sizeof p);
    snprintf(val, sizeof val, "%d", v);
    if (GetPrivateProfileStringA("mod", "launcher", "", have, sizeof have, p)) return WritePrivateProfileStringA("mod", "launcher", val, p) != 0;
    char *t = read_text_file(p);
    if (!t && only_if_file) return 0;
    size_t len = t ? strlen(t) : 0, cap = len + sizeof LAUNCHER_NOTE + 64;
    char *o = malloc(cap), *sec = NULL;
    if (!o) { free(t); return 0; }
    for (char *l = t; l && *l; l += strcspn(l, "\n"), l += *l == '\n') {
        char *s = l;
        while (*s == ' ' || *s == '\t') s++;
        if (!_strnicmp(s, "[mod]", 5)) { sec = l + strcspn(l, "\n"); if (*sec == '\n') sec++; break; }
    }
    if (sec) snprintf(o, cap, "%.*s%s\r\nlauncher=%d\r\n%s", (int)(sec - t), t, LAUNCHER_NOTE, v, sec);
    else snprintf(o, cap, "%s%s[mod]\r\n%s\r\nlauncher=%d\r\n", t ? t : "", len && t[len - 1] != '\n' ? "\r\n\r\n" : len ? "\r\n" : "", LAUNCHER_NOTE, v);
    int ok = write_text_file(p, o);
    free(o);
    free(t);
    return ok;
}
// The line in heapfix.ini, so players see the setting (only when the file is there and has no such line yet)
static void launcher_choice_note(void) {
    char p[MAX_PATH], have[8];
    fix_ini(p, sizeof p);
    if (!GetPrivateProfileStringA("mod", "launcher", "", have, sizeof have, p)) launcher_choice_put(1, 1);
}

static void show_log_file(HWND w) {
    char path[MAX_PATH], arg[MAX_PATH + 32];
    last_log_path(path, sizeof path);
    if (!exists(path)) { ui_toast(1, "There is no log yet. One is written each time the game runs."); return; }
    snprintf(arg, sizeof arg, "/select,\"%s\"", path);
    open_explorer(arg);
}

// ---- The mods on this PC
static Mod *find_mod(const char *name) {
    for (int i = 0; i < g_nmods; i++) if (!_stricmp(g_mods[i].name, name)) return &g_mods[i];
    return NULL;
}
static Mod *add_mod(const char *name, const char *dir, const WIN32_FIND_DATAA *fd) {
    if (g_nmods >= MAX_MODS || find_mod(name)) return NULL;
    Mod *m = &g_mods[g_nmods++];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    snprintf(m->dir, sizeof m->dir, "%s", dir);
    m->ws = ws_from_dir(dir);
    if (fd) { m->size = (unsigned long long)fd->nFileSizeHigh << 32 | fd->nFileSizeLow; m->time = fd->ftLastWriteTime; }
    m->under_n = m->over_n = m->under_by = m->over_of = -1;
    return m;
}
// A pack's version for the list code: its size and its 28-byte pack header (the pack's own save time, file count and index
// sizes), the same bytes on every PC that has that version. 0 = not on this PC.
static unsigned long long list_version(const char *name) {
    static int next;   // the list code asks for the ticked packs in their order, which is the order of g_mods
    Mod *m = next < g_nmods && !_stricmp(g_mods[next].name, name) ? &g_mods[next] : find_mod(name);
    if (!m || m->missing) return 0;
    next = (int)(m - g_mods) + 1;
    if (!m->fp_done) {
        unsigned char hdr[28] = {0};
        char p[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s\\%s", m->dir, m->name);
        FILE *f = fopen(p, "rb");
        if (f) { if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) memset(hdr, 0, sizeof hdr); fclose(f); }
        unsigned long long h = 1469598103934665603ULL;
        for (int i = 0; i < 8; i++) { h ^= (m->size >> (8 * i)) & 255; h *= 1099511628211ULL; }
        for (size_t i = 0; i < sizeof hdr; i++) { h ^= hdr[i]; h *= 1099511628211ULL; }
        m->fp = h;
        m->fp_done = 1;
    }
    return m->fp;
}
static void add_packs_in(const char *dir) {   // every .pack in a folder
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s/*.pack", dir);
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) add_mod(f.cFileName, dir, &f); while (FindNextFileA(h, &f));
    FindClose(h);
}
// A pack in data is a mod when the game's manifest does not name it and its header says mod (3) or movie (4).
static void add_data_mods(void) {
    char *manifest = read_text_file("data/manifest.txt");
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA("data/*.pack", &f);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char line[MAX_PATH + 4], path[MAX_PATH + 8];
            snprintf(line, sizeof line, "%s\t", f.cFileName);
            if (manifest && strstr(manifest, line)) continue;
            snprintf(path, sizeof path, "data/%s", f.cFileName);
            FILE *p = fopen(path, "rb");
            unsigned char hd[8] = {0};
            size_t got = p ? fread(hd, 1, 8, p) : 0;
            if (p) fclose(p);
            if (got == 8 && !memcmp(hd, "PFH", 3) && ((hd[4] & 15) == 3 || (hd[4] & 15) == 4)) add_mod(f.cFileName, "", &f);
        } while (FindNextFileA(h, &f));
        FindClose(h);
    }
    free(manifest);
}
static void add_workshop_mods(void) {
    char root[MAX_PATH], pat[MAX_PATH + 4], dir[MAX_PATH * 2];
    workshop_root(root, sizeof root);
    if (!root[0]) return;
    snprintf(pat, sizeof pat, "%s/*", root);
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || f.cFileName[0] == '.') continue;
        snprintf(dir, sizeof dir, "%s/%s", root, f.cFileName);
        add_packs_in(dir);
    } while (FindNextFileA(h, &f));
    FindClose(h);
}

static int ticked_count(void) { int n = 0; while (n < g_nmods && g_mods[n].on) n++; return n; }
static void free_thumb(Mod *m) {
    if (m->thumb) gp.dispose(m->thumb);
    free(m->thumb_px);
    m->thumb = NULL; m->thumb_px = NULL; m->thumb_state = 0;
}
static int by_name(const void *a, const void *b) { return _stricmp(((const Mod *)a)->name, ((const Mod *)b)->name); }
static void sort_rest(void) { int t = ticked_count(); qsort(g_mods + t, (size_t)(g_nmods - t), sizeof(Mod), by_name); }
static void move_row(int from, int to) {
    if (from < 0 || to < 0 || from >= g_nmods || to >= g_nmods || from == to) return;
    Mod t = g_mods[from];
    if (from < to) memmove(&g_mods[from], &g_mods[from + 1], (size_t)(to - from) * sizeof(Mod));
    else memmove(&g_mods[to + 1], &g_mods[to], (size_t)(from - to) * sizeof(Mod));
    g_mods[to] = t;
}
static void drop_row(int i) { free_thumb(&g_mods[i]); memmove(&g_mods[i], &g_mods[i + 1], (size_t)(g_nmods - i - 1) * sizeof(Mod)); g_nmods--; }
static int set_tick(int i, int on) {   // returns the mod's new row (-1: a missing one was dropped)
    if (i < 0 || i >= g_nmods || g_mods[i].on == on) return i;
    int t = ticked_count();
    if (on) { g_mods[i].on = 1; move_row(i, t); return t; }
    if (g_mods[i].missing) { drop_row(i); return -1; }
    char name[160];
    snprintf(name, sizeof name, "%s", g_mods[i].name);
    g_mods[i].on = 0;
    move_row(i, t - 1);
    sort_rest();
    Mod *m = find_mod(name);
    return m ? (int)(m - g_mods) : -1;
}
static int ticked_entries(ListEntry *out, int max) {
    int n = 0;
    for (int i = 0; i < g_nmods && g_mods[i].on && n < max; i++) { snprintf(out[n].name, sizeof out[n].name, "%s", g_mods[i].name); out[n++].ws = g_mods[i].ws; }
    return n;
}
static int apply_list(const ListEntry *e, int n) {   // these mods ticked in this order; returns how many are not on this PC
    for (int i = 0; i < g_nmods; i++) g_mods[i].on = 0;
    for (int i = g_nmods - 1; i >= 0; i--) if (g_mods[i].missing) drop_row(i);
    int t = 0, missing = 0;
    for (int k = 0; k < n; k++) {
        Mod *m = find_mod(e[k].name);
        if (m && m->on) continue;
        if (!m) {
            if (!(m = add_mod(e[k].name, "", NULL))) continue;
            m->missing = 1;
            m->ws = e[k].ws;
            missing++;
        }
        m->on = 1;
        move_row((int)(m - g_mods), t++);
    }
    sort_rest();
    return missing;
}
static void rescan_item(unsigned long long ws) {   // Steam finished an item: its packs join, a missing row becomes the mod
    char root[MAX_PATH], dir[MAX_PATH * 2], pat[MAX_PATH * 2 + 8];
    workshop_root(root, sizeof root);
    if (!root[0] || !ws) return;
    snprintf(dir, sizeof dir, "%s/%llu", root, ws);
    snprintf(pat, sizeof pat, "%s/*.pack", dir);
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        Mod *m = find_mod(f.cFileName);
        if (m && m->missing) {
            snprintf(m->dir, sizeof m->dir, "%s", dir);
            m->ws = ws;
            m->size = (unsigned long long)f.nFileSizeHigh << 32 | f.nFileSizeLow;
            m->time = f.ftLastWriteTime;
            m->missing = 0;
            m->fp_done = 0;
            free_thumb(m);
        } else if (!m) add_mod(f.cFileName, dir, &f);
    } while (FindNextFileA(h, &f));
    FindClose(h);
    sort_rest();
}

// ---- The two lists. Both are the game's own launcher format: add_working_directory "<folder>"; then mod "<pack>";
// lines; anything else in the file is kept as it is. 32-bit: <game>\used_mods.txt; 64-bit: <game>\attila64\mod_list.txt.
static char *g_other[2];   // the other lines of each file
static ListEntry g_snap[2][MAX_MODS];
static int g_snap_n[2] = {-1, -1};   // each version's ticked list while the window is open (-1 = not read yet)
static int g_same_list;              // one list for both versions
static void list_file(int mode, char *out, size_t size) { joinp(out, size, g_game, mode ? "attila64\\mod_list.txt" : "used_mods.txt"); }
static int read_list(int mode, ListEntry *out, int max) {
    char path[MAX_PATH];
    list_file(mode, path, sizeof path);
    free(g_other[mode]);
    g_other[mode] = calloc(1, 1);
    FILE *f = fopen(path, "r");
    // no 64-bit list yet: start from the 32-bit one, as 64-bit Attila does (its mod_list.txt is made once from used_mods.txt)
    if (!f && mode) { list_file(0, path, sizeof path); f = fopen(path, "r"); }
    int n = 0;
    if (!f) return 0;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        char q[1024];
        const char *a = strchr(line, '"'), *b = a ? strchr(a + 1, '"') : NULL;
        int quoted = b && (size_t)(b - a - 1) < sizeof q;
        if (quoted) { memcpy(q, a + 1, (size_t)(b - a - 1)); q[b - a - 1] = 0; }
        if (!strncmp(line, "add_working_directory", 21)) { if (quoted) add_packs_in(q); }
        else if (!strncmp(line, "mod ", 4)) { if (quoted && n < max && pack_name_ok(q)) { snprintf(out[n].name, 160, "%s", q); out[n++].ws = 0; } }
        else if (line[0] && line[0] != '\n' && line[0] != '\r') {
            size_t have = strlen(g_other[mode]), add = strlen(line);
            char *o = realloc(g_other[mode], have + add + 1);
            if (o) { g_other[mode] = o; memcpy(o + have, line, add + 1); }
        }
    }
    fclose(f);
    return n;
}
static int write_list(int mode) {   // the ticked mods that are on this PC; 1 = written
    char path[MAX_PATH], tmp[MAX_PATH + 8];
    list_file(mode, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return 0;
    for (int i = 0; i < g_nmods && g_mods[i].on; i++) {
        if (g_mods[i].missing || !g_mods[i].dir[0]) continue;
        int seen = 0;
        for (int j = 0; j < i && !seen; j++) seen = !g_mods[j].missing && !_stricmp(g_mods[j].dir, g_mods[i].dir);
        if (!seen) fprintf(f, "add_working_directory \"%s\";\n", g_mods[i].dir);
    }
    for (int i = 0; i < g_nmods && g_mods[i].on; i++) if (!g_mods[i].missing) fprintf(f, "mod \"%s\";\n", g_mods[i].name);
    if (g_other[mode]) fputs(g_other[mode], f);
    int ok = !ferror(f);
    fclose(f);
    if (ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) return 1;
    DeleteFileA(tmp);
    return 0;
}
static void save_list(void) {
    if (!write_list(g_mode) || (g_same_list && g_have64 && !write_list(!g_mode)))
        MessageBoxA(NULL, "Could not save the mod list. The game starts with the previous list.", "Total War Mod Manager", MB_ICONWARNING);
}

static void load_all(void) {
    g_mods = calloc(MAX_MODS, sizeof(Mod));
    static ListEntry e[MAX_MODS];
    int n = read_list(g_mode, e, MAX_MODS);
    if (g_have64) { static ListEntry o[MAX_MODS]; g_snap_n[!g_mode] = read_list(!g_mode, o, MAX_MODS); memcpy(g_snap[!g_mode], o, sizeof(ListEntry) * (size_t)g_snap_n[!g_mode]); }
    add_data_mods();
    add_workshop_mods();
    apply_list(e, n);
}
// Switches the window between the versions; their ticked lists are kept apart (or one list for both).
static void set_mode(int m) {
    if (m == g_mode || (m && !g_have64)) return;
    g_snap_n[g_mode] = ticked_entries(g_snap[g_mode], MAX_MODS);
    g_mode = m;
    if (!g_same_list) apply_list(g_snap[m], g_snap_n[m] < 0 ? 0 : g_snap_n[m]);
    settings_put("mode", m ? "64" : "32");
}

// ---- Presets: TotalWarModManager\presets\<name>.txt (the export format). preset=<name> in launcher.ini is the one in use.
static void preset_path(const char *name, char *out, size_t size) {
    if (name) snprintf(out, size, "%s\\presets\\%s.txt", g_home, name);
    else snprintf(out, size, "%s\\presets", g_home);
}
#define MAX_PRESETS 64
static char g_presets[MAX_PRESETS][64];
static int g_npresets;
static char g_preset[64];
static ListEntry g_preset_list[MAX_MODS];
static int g_preset_n = -1;
static int by_text(const void *a, const void *b) { return _stricmp((const char *)a, (const char *)b); }
static void presets_scan(void) {
    char dir[MAX_PATH], pat[MAX_PATH + 8];
    preset_path(NULL, dir, sizeof dir);
    snprintf(pat, sizeof pat, "%s\\*.txt", dir);
    g_npresets = 0;
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA(pat, &f);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            size_t n = strlen(f.cFileName);
            if (n < 5 || n - 4 >= sizeof g_presets[0] || g_npresets >= MAX_PRESETS) continue;
            memcpy(g_presets[g_npresets], f.cFileName, n - 4);
            g_presets[g_npresets][n - 4] = 0;
            if (preset_name_ok(g_presets[g_npresets])) g_npresets++;
        } while (FindNextFileA(h, &f));
        FindClose(h);
    }
    qsort(g_presets, (size_t)g_npresets, sizeof g_presets[0], by_text);
}
static int preset_read(const char *name, ListEntry *out, int max) {
    char path[MAX_PATH];
    preset_path(name, path, sizeof path);
    char *t = read_text_file(path);
    if (!t) return -1;
    int n = list_parse(t, out, max, NULL);
    free(t);
    return n;
}
static int preset_write(const char *name) {
    static ListEntry e[MAX_MODS];
    char dir[MAX_PATH], path[MAX_PATH];
    preset_path(NULL, dir, sizeof dir);
    CreateDirectoryA(dir, NULL);
    preset_path(name, path, sizeof path);
    int n = ticked_entries(e, MAX_MODS);
    char *t = list_format(e, n);
    int ok = t && write_text_file(path, t);
    free(t);
    if (ok) { snprintf(g_preset, sizeof g_preset, "%s", name); memcpy(g_preset_list, e, sizeof(ListEntry) * (size_t)n); g_preset_n = n; presets_scan(); }
    return ok;
}
static int preset_changed(void) {
    if (!g_preset[0] || g_preset_n < 0) return 0;
    int t = ticked_count();
    if (t != g_preset_n) return 1;
    for (int i = 0; i < t; i++) if (_stricmp(g_mods[i].name, g_preset_list[i].name)) return 1;
    return 0;
}
static ListEntry g_undo[MAX_MODS];
static int g_undo_n = -1;
static char g_undo_preset[64];
static void undo_keep(void) { g_undo_n = ticked_entries(g_undo, MAX_MODS); snprintf(g_undo_preset, sizeof g_undo_preset, "%s", g_preset); }

// ---- Steam, through the game's own steam_api.dll (32-bit, always there)
static HMODULE g_steam_dll;
static int g_steam_state;
void *steam_api_fn(const char *name) { return g_steam_dll ? (void *)GetProcAddress(g_steam_dll, name) : NULL; }
int steam_connect_quiet(void) {   // one try, no box; the game connects by itself later
    if (g_steam_state > 0) return 1;
    if (getenv("ATTILA_TEST_NO_STEAM")) return 0;
    if (!g_steam_dll) {
        char p[MAX_PATH];
        joinp(p, sizeof p, g_game, "steam_api.dll");
        if (!(g_steam_dll = LoadLibraryA(p))) return 0;
    }
    SetEnvironmentVariableA("SteamAppId", "325610");
    SetEnvironmentVariableA("SteamGameId", "325610");
    unsigned char (*init)(void) = (unsigned char (*)(void))steam_api_fn("SteamAPI_Init");
    if (!init || !init()) return 0;
    g_steam_state = 1;
    return 1;
}

// ---- Updates before the game starts: the engine fix (32-bit DLL, 64-bit plugin), the add-on, from the Workshop. Nothing is
// loaded yet, so a newer file takes effect on this Play. update_off.txt in the game folder stops all of it (as in the fix).
#include "update.c"
static void update_before_play(void) {
    wchar_t game[MAX_PATH], ws[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    char root[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, g_game, -1, game, MAX_PATH);
    swprintf(a, MAX_PATH, L"%ls\\update_off.txt", game);
    if (GetFileAttributesW(a) != INVALID_FILE_ATTRIBUTES) { lg("update: update_off.txt found, nothing taken from the Workshop"); return; }
    workshop_root(root, sizeof root);
    MultiByteToWideChar(CP_UTF8, 0, root, -1, ws, MAX_PATH);
    for (wchar_t *c = ws; *c; c++) if (*c == L'/') *c = L'\\';
    swprintf(a, MAX_PATH, L"%ls\\attila64\\plugins", game);
    swprintf(b, MAX_PATH, L"%ls\\" WORKSHOP_ITEM L"\\perffix64.dll", ws);
    plugin64_update_from(a, b, lgw);
    swprintf(a, MAX_PATH, L"%ls\\dinput8.dll", game);   // turned off (dinput8.dll.off): left as it is
    swprintf(b, MAX_PATH, L"%ls\\" WORKSHOP_ITEM L"\\dinput8.dll", ws);
    if (GetFileAttributesW(a) != INVALID_FILE_ATTRIBUTES) update_from(a, b, file_version(a), lgw);
    swprintf(a, MAX_PATH, L"%ls\\FrameBridge", game);
    swprintf(b, MAX_PATH, L"%ls\\" ADDON_ITEM L"\\FrameBridge", ws);
    addon_update_from(a, b, lgw);
}

// ---- Steam's Play button: this launcher in CA's launcher's place. CA's file stays as launcher_ca.exe (Steam's "Verify
// files" or a CA update writes CA's launcher.exe again: the next check moves it aside again). TotalWarModManager\
// use_ca_launcher.txt = the player chose CA's launcher for good: nothing is swapped.
#include "launcher_swap.inc"

// What Play does, after the window has saved everything.
enum { GO_GAME, GO_OWN_WINDOW, GO_CA };   // the game / 64-bit Attila's own window / CA's launcher
static int g_go = -1;
static const char *g_args = "";   // our command line after the exe (Steam's own arguments): handed to CA's launcher

#include "picker_work.inc"
#include "picker_ui.inc"
#include "picker_input.inc"
#include "picker_test.inc"

static UINT monitor_dpi(HMONITOR m) {
    static HRESULT (WINAPI *get)(HMONITOR, int, UINT *, UINT *);
    static int tried;
    if (!tried) { HMODULE d = LoadLibraryA("shcore.dll"); tried = 1; if (d) *(FARPROC *)&get = GetProcAddress(d, "GetDpiForMonitor"); }
    UINT x = 96, y = 96;
    if (!get || get(m, 0, &x, &y)) { HDC dc = GetDC(NULL); x = (UINT)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(NULL, dc); }
    return x ? x : 96;
}

// 64-bit Attila's version from its exe (major.minor), 0 when not there.
static int sol_version(int *major, int *minor) {
    char p[MAX_PATH];
    joinp(p, sizeof p, g_game, "attila64\\attila64.exe");
    *major = *minor = 0;
    HMODULE v = LoadLibraryA("version.dll");
    DWORD (WINAPI *size_f)(LPCSTR, LPDWORD) = v ? (DWORD (WINAPI *)(LPCSTR, LPDWORD))(void *)GetProcAddress(v, "GetFileVersionInfoSizeA") : NULL;
    BOOL (WINAPI *info_f)(LPCSTR, DWORD, DWORD, LPVOID) = v ? (BOOL (WINAPI *)(LPCSTR, DWORD, DWORD, LPVOID))(void *)GetProcAddress(v, "GetFileVersionInfoA") : NULL;
    BOOL (WINAPI *query_f)(LPCVOID, LPCSTR, LPVOID *, PUINT) = v ? (BOOL (WINAPI *)(LPCVOID, LPCSTR, LPVOID *, PUINT))(void *)GetProcAddress(v, "VerQueryValueA") : NULL;
    DWORD h = 0, n = size_f ? size_f(p, &h) : 0;
    void *buf = n ? malloc(n) : NULL;
    VS_FIXEDFILEINFO *fi = NULL;
    UINT len = 0;
    if (buf && info_f(p, 0, n, buf) && query_f(buf, "\\", (void **)&fi, &len) && fi) { *major = HIWORD(fi->dwFileVersionMS); *minor = LOWORD(fi->dwFileVersionMS); }
    free(buf);
    return *major || *minor;
}
static void have64_check(void) {
    char exe64[MAX_PATH];
    joinp(exe64, sizeof exe64, g_game, "attila64\\attila64.exe");
    g_have64 = exists(exe64);
    int ma, mi;
    g_sol_ok = g_have64 && sol_version(&ma, &mi) && (ma < SOL_TESTED_MAJOR || (ma == SOL_TESTED_MAJOR && mi <= SOL_TESTED_MINOR));
}

// Starts a program and waits for it (Steam sees one game session from Play to the end). Returns its exit code.
static DWORD run_and_wait(const char *exe, const char *args, const char *dir, int hide_window) {
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, args ? args : "");
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof si;
    DWORD code = 1;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) { lg("could not start %s (error %lu)", exe, GetLastError()); return 2; }
    lg("started: %s", cmd);
    CloseHandle(pi.hThread);
    if (hide_window && g_win) ShowWindow(g_win, SW_HIDE);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code;
}

static int start_after_window(void) {
    char exe[MAX_PATH], dir[MAX_PATH];
    if (g_go == GO_CA) {
        joinp(exe, sizeof exe, g_game, "launcher\\launcher_ca.exe");
        joinp(dir, sizeof dir, g_game, "launcher");
        return (int)run_and_wait(exe, g_args, dir, 1);
    }
    update_before_play();
    // Steam sets these for what it starts; set here too, so the game never restarts itself through Steam (which would
    // come back to this launcher) when the launcher was started some other way
    SetEnvironmentVariableA("SteamAppId", "325610");
    SetEnvironmentVariableA("SteamGameId", "325610");
    if (g_mode) {
        joinp(exe, sizeof exe, g_game, "attila64\\attila64.exe");
        joinp(dir, sizeof dir, g_game, "attila64");
        if (g_go == GO_GAME) SetEnvironmentVariableA("ATTILA_NO_PICKER", "1");
        SetEnvironmentVariableA("ATTILA_FROM_LAUNCHER", "1");   // 64-bit Attila hands Steam's Play to this launcher, but never back
        return (int)run_and_wait(exe, "", dir, 1);
    }
    joinp(exe, sizeof exe, g_game, "Attila.exe");
    return (int)run_and_wait(exe, "used_mods.txt;", g_game, 1);
}

// This launcher updates itself before anything else: a newer copy in the engine fix's Workshop item (complete, as the fix
// checks its own) replaces this exe (renamed to .old, deleted next time) and runs with the same command line; this one waits.
static int self_update(void) {
    char me_old[MAX_PATH + 8], root[MAX_PATH], ws[MAX_PATH * 2], up[MAX_PATH];
    snprintf(me_old, sizeof me_old, "%s.old", g_exe);
    DeleteFileA(me_old);
    joinp(up, sizeof up, g_game, "update_off.txt");
    if (exists(up)) return -1;
    workshop_root(root, sizeof root);
    snprintf(ws, sizeof ws, "%s/" ENGINE_ITEM "/TotalWarModManager.exe", root);
    int theirs = launcher_version(ws);
    if (theirs <= LAUNCHER_VERSION) return -1;
    wchar_t w[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, ws, -1, w, MAX_PATH * 2);
    if (!pe_complete(w)) { lg("self-update: version %d not fully downloaded yet", theirs); return -1; }
    if (!MoveFileExA(g_exe, me_old, MOVEFILE_REPLACE_EXISTING)) return -1;
    if (!CopyFileA(ws, g_exe, FALSE) || launcher_version(g_exe) != theirs) {
        DeleteFileA(g_exe);
        MoveFileExA(me_old, g_exe, MOVEFILE_REPLACE_EXISTING);
        lg("self-update: copying version %d failed, this one runs", theirs);
        return -1;
    }
    lg("self-update: %d -> %d", LAUNCHER_VERSION, theirs);
    return (int)run_and_wait(g_exe, g_args, NULL, 0);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show) {
    DWORD t0 = GetTickCount();
    GetModuleFileNameA(NULL, g_exe, sizeof g_exe);
    g_args = cmdline ? cmdline : "";
    // install.bat / uninstall.bat (and the engine fix DLL): <this exe in the Workshop item> --install|--uninstall "<game>"
    int do_inst = !strncmp(g_args, "--install ", 10), do_uninst = !strncmp(g_args, "--uninstall ", 12);
    if (do_inst || do_uninst) {
        char game[MAX_PATH];
        snprintf(game, sizeof game, "%s", g_args + (do_inst ? 10 : 12));
        char *g = game;
        if (*g == '"') { g++; char *q = strchr(g, '"'); if (q) *q = 0; }
        snprintf(g_game, sizeof g_game, "%s", g);
        joinp(g_home, sizeof g_home, g_game, "TotalWarModManager");
        CreateDirectoryA(g_home, NULL);
        if (do_uninst) return launcher_restore_ca(g_game) ? 0 : 1;
        int r = launcher_swap(g_game, g_exe);   // install.bat: the engine fix installed (again)
        return r < 0 ? 1 : 0;
    }
    if (!find_game()) {
        MessageBoxA(NULL, "Total War Mod Manager: Attila.exe was not found next to this launcher.", "Total War Mod Manager", MB_ICONERROR);
        return 1;
    }
    SetCurrentDirectoryA(g_game);
    joinp(g_home, sizeof g_home, g_game, "TotalWarModManager");
    CreateDirectoryA(g_home, NULL);
    joinp(g_ini, sizeof g_ini, g_home, "launcher.ini");
    joinp(g_sol_ini, sizeof g_sol_ini, g_game, "attila64\\settings.txt");
    // Steam's Play or a start by hand (a double-click on this exe, a shortcut): Steam sets these for what it starts
    int by_steam = getenv("SteamClientLaunch") || getenv("SteamGameId");
    {
        char names[400] = "";
        char *env = GetEnvironmentStringsA();
        for (char *e = env; e && *e; e += strlen(e) + 1)
            if (!_strnicmp(e, "Steam", 5) && strlen(names) + 48 < sizeof names) { strncat(names, e, strcspn(e, "=")); strcat(names, " "); }
        if (env) FreeEnvironmentStringsA(env);
        lg("start: %s; Steam's variables: %s", by_steam ? "by Steam" : "by hand", names[0] ? names : "none");
    }
    // 64-bit Attila started from Steam's Play by its own Launch Options line hands over to this launcher with --from-attila64:
    // this start shows 64-bit. --switch-from-attila64: the "Total War Mod Manager" button in 64-bit Attila's own window: this
    // window, in 64-bit, and Play stays with it ("sol_window_always" goes off).
    int from64 = 0, switch64 = 0;
    if (!strncmp(g_args, "--switch-from-attila64", 22)) { g_args += 22; switch64 = 1; }
    else if (!strncmp(g_args, "--from-attila64", 15)) { g_args += 15; from64 = 1; }
    while (*g_args == ' ') g_args++;
    int shown = switch64 || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;   // Shift held while pressing Play: this window
    // heapfix.ini launcher=2: Steam's Play opens CA's launcher. Started by hand (or with Shift), this window opens and
    // offers to switch back.
    int choice = launcher_choice();
    launcher_choice_note();
    if (choice == 2 && by_steam && !shown && !from64 && !getenv("ATTILA_PICKER_TEST")) {
        char ca[MAX_PATH];
        joinp(ca, sizeof ca, g_game, "launcher\\launcher_ca.exe");
        if (exists(ca)) { lg("CA's launcher (heapfix.ini: launcher=2)"); g_go = GO_CA; return start_after_window(); }
    }
    if (!getenv("ATTILA_LAUNCHER_NO_UPDATE")) { int r = self_update(); if (r >= 0) return r; }   // tests switch it off

    char v[96];
    have64_check();
    g_mode = g_have64 && ((settings_get("mode", v, sizeof v) && !strcmp(v, "64")) || from64 || switch64);
    if (switch64) settings_put("sol_window_always", "0");
    g_same_list = settings_get("same_list", v, sizeof v) && v[0] == '1';
    // sol_window_always=1 (64-bit Attila's installer can write it, for players who want its own window): no window here,
    // straight to it (after the updates, which keep the engine fix current either way)
    g_sol_always = settings_get("sol_window_always", v, sizeof v) && v[0] == '1';
    if (!shown && !getenv("ATTILA_PICKER_TEST") && g_have64 && g_mode && g_sol_always) {
        lg("64-bit Attila's own window, straight away (sol_window_always=1)");
        g_go = GO_OWN_WINDOW;
        return start_after_window();
    }
    load_all();
    gdiplus_start();
    presets_scan();
    g_theme = TH_DARK;
    if (settings_get("theme", v, sizeof v)) for (int i = 0; i < N_THEMES; i++) if (!_stricmp(v, g_theme_keys[i])) g_theme = i;
    if (g_theme > TH_DARK) g_theme = TH_DARK;   // the picture themes are 64-bit Attila's pictures: not in this launcher
    host_read_settings();
    if (settings_get("preset", v, sizeof v) && v[0] && preset_name_ok(v)) {
        int n = preset_read(v, g_preset_list, MAX_MODS);
        if (n >= 0) { snprintf(g_preset, sizeof g_preset, "%s", v); g_preset_n = n; }
    }
    if (getenv("ATTILA_PICKER_SCALE")) g_force_scale = (float)atof(getenv("ATTILA_PICKER_SCALE")) / 100.0f;

    HMODULE u32 = GetModuleHandleA("user32.dll");
    void *(WINAPI *set_ctx)(void *) = u32 ? (void *(WINAPI *)(void *))GetProcAddress(u32, "SetThreadDpiAwarenessContext") : NULL;
    void *old_ctx = set_ctx ? set_ctx((void *)(intptr_t)-4) : NULL;   // per monitor (v2): sharp at any scaling
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi2 = {sizeof mi2};
    GetMonitorInfoA(mon, &mi2);
    g_dpi = old_ctx ? (int)monitor_dpi(mon) : 96;
    g_scale = g_force_scale > 0 ? g_force_scale : g_dpi / 96.0f;

    WNDCLASSW wc = {0};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = picker_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.lpszClassName = L"TotalWarModManager";
    RegisterClassW(&wc);
    int cw = 1180, ch = 780, maxed = 0;
    if (settings_get("window", v, sizeof v)) sscanf(v, "%dx%d", &cw, &ch);
    if (settings_get("window_max", v, sizeof v)) maxed = v[0] == '1';
    if (cw < 940) cw = 940;
    if (ch < 640) ch = 640;
    RECT r = {0, 0, S((float)cw), S((float)ch)};
    adjust_rect(&r);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    RECT wa = mi2.rcWork;
    if (ww > wa.right - wa.left) ww = wa.right - wa.left;
    if (wh > wa.bottom - wa.top) wh = wa.bottom - wa.top;
    HWND w = CreateWindowExW(0, L"TotalWarModManager", L"Total War Mod Manager", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh, NULL, NULL, inst, NULL);
    if (!w) return 1;
    ShowWindow(w, maxed ? SW_SHOWMAXIMIZED : getenv("ATTILA_PICKER_TEST") ? SW_SHOWNOACTIVATE : SW_SHOW);
    if (!getenv("ATTILA_PICKER_TEST")) SetForegroundWindow(w);
    SetFocus(w);
    UpdateWindow(w);
    g_open_ms = GetTickCount() - t0;
    if (choice == 2) ask_open(ASK_BACK);   // Steam's Play opens CA's launcher: offer to switch back
    else if (!settings_get("welcomed", v, sizeof v)) {   // the first start: what this is, and where CA's launcher is (once)
        ask_open(ASK_WELCOME);
        settings_put("welcomed", "1");
    }
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    for (int i = 0; i < g_nmods; i++) free_thumb(&g_mods[i]);
    if (g_big.img) gp.dispose(g_big.img);
    free(g_big.px);
    gdiplus_stop();
    if (old_ctx) set_ctx(old_ctx);
    if (g_result != 1 || g_go < 0) return 0;
    if (getenv("ATTILA_PICKER_ONLY")) { lg("test: would start %s (%s)", g_go == GO_CA ? "CA's launcher" : g_mode ? "64-bit" : "32-bit", g_go == GO_OWN_WINDOW ? "its own window" : "-"); return 0; }
    return start_after_window();
}
