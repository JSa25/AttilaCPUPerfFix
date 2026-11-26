// ---- Mod list text: export, import, presets and the list code (start window redo, 2026-11-25).
// One text format for all of them, readable when pasted on Discord:
//   64-bit Attila mod list, code 7F3K-92D1, 12 mods (upper mods win)
//   1. 1429109380 1212compbuild_v2.pack
//   7. data my_local_tweaks.pack
// The reader also takes the launcher's own format (mod "x.pack"; as in mod_list.txt and used_mods.txt), with or without the
// numbers, and lines pasted from Discord. An imported text comes from another player: only Workshop numbers and plain pack
// file names are taken from it, never folders or paths.
// Needs <windows.h>, <stdio.h>, <stdlib.h>, <string.h>, <ctype.h> before it (picker.c, tests/picker_test.c).

typedef struct { char name[160]; unsigned long long ws; } ListEntry;   // ws: Workshop item number, 0 = not from the Workshop

// A plain pack file name: ends in .pack, no folder, nothing Windows refuses in a file name, shorter than 160 bytes.
static int pack_name_ok(const char *s) {
    size_t n = strlen(s);
    if (n < 6 || n >= 160 || _stricmp(s + n - 5, ".pack") || s[0] == ' ' || s[0] == '.') return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 32 || strchr("\\/:*?\"<>|", *p)) return 0;
    return 1;
}

// The Workshop item number in a folder path ".../workshop/content/325610/<number>", 0 when it is not such a folder.
static unsigned long long ws_from_dir(const char *dir) {
    for (const char *p = strstr(dir, "325610"); p; p = strstr(p + 1, "325610")) {
        if ((p[6] != '/' && p[6] != '\\') || (p != dir && p[-1] != '/' && p[-1] != '\\')) continue;
        unsigned long long v = 0;
        const char *q = p + 7;
        int nd = 0;
        while (*q >= '0' && *q <= '9' && nd < 20) { v = v * 10 + (unsigned)(*q++ - '0'); nd++; }
        if (nd && (!*q || *q == '/' || *q == '\\')) return v;
    }
    return 0;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
}

// One line of a list. 1 = a mod (in *e); the header line's code goes to code (may be NULL).
static int list_parse_line(char *line, ListEntry *e, char *code) {
    char *s = trim(line);
    memset(e, 0, sizeof *e);
    if (!_strnicmp(s, "Attila mod list", 15) || !_strnicmp(s, "64-bit Attila mod list", 22)) {
        const char *c = strstr(s, "code ");
        if (code && c && strlen(c + 5) >= 9 && c[9] == '-') { memcpy(code, c + 5, 9); code[9] = 0; }
        return 0;
    }
    if (!strncmp(s, "mod ", 4)) {   // launcher format
        char *a = strchr(s, '"'), *b = a ? strchr(a + 1, '"') : NULL;
        if (!b || b - a - 1 >= (int)sizeof e->name) return 0;
        memcpy(e->name, a + 1, (size_t)(b - a - 1));
        e->name[b - a - 1] = 0;
        return pack_name_ok(e->name);
    }
    // list marker: "12." "12)" "-" "*" or the bullet Discord copies
    if (*s >= '0' && *s <= '9') {
        char *p = s;
        while (*p >= '0' && *p <= '9') p++;
        if ((*p == '.' || *p == ')') && (p[1] == ' ' || p[1] == '\t')) s = trim(p + 1);
    } else if ((*s == '-' || *s == '*') && (s[1] == ' ' || s[1] == '\t')) s = trim(s + 1);
    else if (!strncmp(s, "\xE2\x80\xA2", 3)) s = trim(s + 3);
    // "<pack>  https://steamcommunity.com/sharedfiles/filedetails/?id=<number>" (the export; the link may come first, or in
    // <> as Discord copies it): the number from the link, the rest is the pack name
    char *u = strstr(s, "steamcommunity.com/");
    if (u) {
        char *id = strstr(u, "id="), *a = u, *b;
        if (id) for (char *q = id + 3; *q >= '0' && *q <= '9' && q < id + 23; q++) e->ws = e->ws * 10 + (unsigned)(*q - '0');
        while (a > s && a[-1] != ' ' && a[-1] != '\t') a--;   // the whole link, cut out
        for (b = u; *b && *b != ' ' && *b != '\t'; b++) {}
        memmove(a, b, strlen(b) + 1);
        s = trim(s);
        if (!e->ws || strlen(s) >= sizeof e->name) { e->ws = 0; return 0; }
        strcpy(e->name, s);
        return pack_name_ok(e->name);
    }
    char *nw = strstr(s, "(not on the Workshop)");
    if (nw) { *nw = 0; s = trim(s); }
    // Workshop number, or "data" / "local"; a line with only a pack name is taken too
    char *p = s;
    int nd = 0;
    if (*p >= '0' && *p <= '9') {
        while (*p >= '0' && *p <= '9') { if (nd < 20) e->ws = e->ws * 10 + (unsigned)(*p - '0'); p++; nd++; }
    } else if (!_strnicmp(p, "data", 4)) p += 4, nd = -1;
    else if (!_strnicmp(p, "local", 5)) p += 5, nd = -1;
    if (!((nd < 0 || (nd >= 6 && nd <= 20)) && (*p == ' ' || *p == '\t'))) {
        e->ws = 0;
        if (strlen(s) >= sizeof e->name) return 0;
        strcpy(e->name, s);
        return pack_name_ok(e->name);
    }
    char *name = trim(p);
    size_t n = strlen(name);
    if (n >= 2 && (name[0] == '`' || name[0] == '"') && name[n - 1] == name[0]) { name[n - 1] = 0; name++; }
    if (strlen(name) >= sizeof e->name) return 0;
    strcpy(e->name, name);
    return pack_name_ok(e->name);
}

// Reads a mod list from text. Returns the number of mods (a pack named twice counts once); code gets the list code from
// the header line when there is one ("" when not; may be NULL).
static int list_parse(const char *text, ListEntry *out, int max, char *code) {
    int n = 0;
    if (code) code[0] = 0;
    for (const char *line = text; line && *line;) {
        size_t len = strcspn(line, "\r\n");
        char buf[600];
        ListEntry e;
        if (len < sizeof buf) {
            memcpy(buf, line, len);
            buf[len] = 0;
            if (list_parse_line(buf, &e, code) && n < max) {
                int dup = 0;
                for (int i = 0; i < n && !dup; i++) dup = !_stricmp(out[i].name, e.name);
                if (!dup) out[n++] = e;
            }
        }
        line += len;
        while (*line == '\r' || *line == '\n') line++;
    }
    return n;
}

// 8 letters from the pack names in their order and their versions (list_version: size and pack header; 0 for a pack not on
// this PC): two players with the same code have the same mods, in the same order, in the same versions. Upper and lower
// case the same.
static void list_code(const ListEntry *e, int n, char out[10]) {
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) {
        for (const unsigned char *p = (const unsigned char *)e[i].name; *p; p++) { h ^= (unsigned char)tolower(*p); h *= 1099511628211ULL; }
        h ^= '\n';
        h *= 1099511628211ULL;
        unsigned long long v = list_version(e[i].name);
        for (int b = 0; b < 8; b++) { h ^= (v >> (8 * b)) & 255; h *= 1099511628211ULL; }
    }
    static const char abc[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";   // no I, L, O, U: nothing to misread when said aloud
    for (int i = 0, k = 0; i < 9; i++) out[i] = i == 4 ? '-' : abc[(h >> (59 - 5 * k++)) & 31];
    out[9] = 0;
}

// The list as text (the caller frees it), CRLF line ends.
static char *list_format(const ListEntry *e, int n) {
    char code[10];
    list_code(e, n, code);
    size_t cap = 256 + (size_t)n * 300, len = 0;
    char *t = malloc(cap);
    if (!t) return NULL;
    len += (size_t)snprintf(t + len, cap - len, "Attila mod list, code %s, %d mod%s (upper mods win). Total War Mod Manager's Import subscribes to the links.\r\n",
                            code, n, n == 1 ? "" : "s");
    for (int i = 0; i < n; i++) {   // the Workshop link of each mod (Discord shows them as links)
        if (e[i].ws) len += (size_t)snprintf(t + len, cap - len, "%d. %s  https://steamcommunity.com/sharedfiles/filedetails/?id=%llu\r\n", i + 1, e[i].name, e[i].ws);
        else len += (size_t)snprintf(t + len, cap - len, "%d. %s  (not on the Workshop)\r\n", i + 1, e[i].name);
    }
    return t;
}

// Whole file as text (the caller frees it); NULL when it cannot be read. Files bigger than 4 MB are not mod lists.
static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *t = malloc((4u << 20) + 1);
    size_t n = t ? fread(t, 1, 4u << 20, f) : 0;
    fclose(f);
    if (t) t[n] = 0;
    return t;
}

// Writes the text to path through a temporary file, so a failed write never leaves half a file. 1 = written.
static int write_text_file(const char *path, const char *text) {
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    fputs(text, f);
    int ok = !ferror(f);
    if (fclose(f)) ok = 0;
    if (ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) return 1;
    DeleteFileA(tmp);
    return 0;
}

// A preset name the player typed, usable as a file name: 1 to 60 bytes, letters (any language), digits, spaces and
// - _ ( ) , . ' & + !; no leading or trailing space or dot; not a name Windows keeps for devices.
static int preset_name_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 60 || s[0] == ' ' || s[0] == '.' || s[n - 1] == ' ' || s[n - 1] == '.') return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!(*p >= 0x80 || isalnum(*p) || strchr(" -_(),.'&+!", *p))) return 0;
    static const char *const dev[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
                                      "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (size_t i = 0; i < sizeof dev / sizeof dev[0]; i++) {
        size_t d = strlen(dev[i]);
        if (!_strnicmp(s, dev[i], d) && (!s[d] || s[d] == '.')) return 0;
    }
    return 1;
}
