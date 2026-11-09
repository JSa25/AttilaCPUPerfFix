// Script extender: loose Lua scripts and a Lua console, through the game's own Lua 5.1 (statically in
// empire.retail.dll, lua_Number is float). Opt-in: nothing happens unless the game folder has a lua_ext\ folder.
// The game keeps one shared state (+0x291DE00, made on first use, closed when the last script environment goes) and
// a list of script environments (+0x1DA4230 first node, +0x1DA4234 the end marker; node: +4 next, +8 environment,
// +0xC root state). Each environment's first field is a Lua thread whose globals table is the environment's own,
// falling back to the shared globals (FUN_11626190); the campaign's scripting.lua runs in one (FUN_10779090).
// Every frame, on the main thread at the start of the game's frame function (no Lua running then):
//  - an environment seen for 2 frames in a row gets ext_print() and runs every lua_ext\*.lua, in name order;
//  - lines typed in the console (Ctrl+` toggles its window) run in the selected environment, the newest by default.
// Output goes to the console and to lua_ext\lua_ext.log.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "gamerva.h"

typedef struct lua_State lua_State;
typedef int (*lua_CFunction)(lua_State *L);
#define LUA_GLOBALSINDEX (-10002)
#define LUA_MULTRET (-1)

static int (*l_gettop)(lua_State *L);
static void (*l_settop)(lua_State *L, int idx);
static void (*l_pushvalue)(lua_State *L, int idx);
static void (*l_pushstring)(lua_State *L, const char *s);
static void (*l_getfield)(lua_State *L, int idx, const char *k);
static void (*l_setfield)(lua_State *L, int idx, const char *k);
static void (*l_pushcclosure)(lua_State *L, lua_CFunction f, int n);
static int (*l_pcall)(lua_State *L, int nargs, int nresults, int errfunc);
static const char *(*l_tolstring)(lua_State *L, int idx, size_t *len);
static int (*l_loadbuffer)(lua_State *L, const char *buf, size_t n, const char *name);

// Lua 5.1 lua_State: status byte at +6, top at +8 (TValue: value, then type at +4; LUA_TNIL 0)
#define L_STATUS(L) (((unsigned char *)(L))[6])
#define TOP_TYPE(L) (((int *)*(char **)((char *)(L) + 8))[-1])

static const struct { unsigned rva; unsigned char n, b[10]; void **fn; } API[] = {
    {0x12C67B0, 10, {0x8B, 0x4C, 0x24, 0x04, 0x8B, 0x41, 0x08, 0x2B, 0x41, 0x0C}, (void **)&l_gettop},
    {0x12C7210, 8, {0x8B, 0x4C, 0x24, 0x08, 0x8B, 0x44, 0x24, 0x04}, (void **)&l_settop},
    {0x12C6D80, 0, {0}, (void **)&l_pushvalue},   // shortcut: no byte check, called only after the others matched
    {0x12C6D20, 8, {0x8B, 0x54, 0x24, 0x08, 0x85, 0xD2, 0x75, 0x0D}, (void **)&l_pushstring},
    {0x12C66D0, 10, {0x83, 0xEC, 0x08, 0x53, 0x56, 0x8B, 0x74, 0x24, 0x14, 0x57}, (void **)&l_getfield},
    {0x12C70D0, 10, {0x83, 0xEC, 0x08, 0x53, 0x56, 0x8B, 0x74, 0x24, 0x14, 0x57}, (void **)&l_setfield},
    {0x12C6BA0, 10, {0x55, 0x56, 0x8B, 0x74, 0x24, 0x0C, 0x57, 0x8B, 0x4E, 0x10}, (void **)&l_pushcclosure},
    {0x12C6B00, 10, {0x8B, 0x44, 0x24, 0x10, 0x83, 0xEC, 0x08, 0x53, 0x56, 0x57}, (void **)&l_pcall},
    {0x12C7380, 10, {0x56, 0xFF, 0x74, 0x24, 0x0C, 0x8B, 0x74, 0x24, 0x0C, 0x56}, (void **)&l_tolstring},
    {0x12C7E20, 10, {0x83, 0xEC, 0x08, 0x8B, 0x44, 0x24, 0x10, 0xFF, 0x74, 0x24}, (void **)&l_loadbuffer},
};
#define RVA_STATE 0x291DE00u      // the shared lua_State*, read by FUN_11626190 (+0x1626190: sub esp,0xC; push esi; mov esi,[..])
#define RVA_ENVLIST 0x1DA4230u    // first node; read by FUN_116238a0 (+0x16238A0: mov edx,[..])

static void **g_first;            // &first node
static char *g_end;               // the list's end marker
static wchar_t g_dir[MAX_PATH];   // <game>\lua_ext
static FILE *g_log;

#define MAXENV 32
static struct { void *env; lua_State *T; unsigned id, frames, loaded; } g_env[MAXENV];
static unsigned g_nenv, g_next_id = 1, g_sel;   // g_sel: the console's environment id, 0 = the newest

static void out(const char *s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD w;
    if (GetConsoleWindow() && h && h != INVALID_HANDLE_VALUE) {
        WriteConsoleA(h, s, (DWORD)strlen(s), &w, NULL);
        WriteConsoleA(h, "\r\n", 2, &w, NULL);
    }
    if (g_log) { fputs(s, g_log); fputc('\n', g_log); fflush(g_log); }
}

static void outf(const char *fmt, ...) {
    char b[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    out(b);
}

// tostring() of the value at idx (absolute), through the environment's own tostring
// Every Lua call that can raise an error runs inside l_pcall: an error with no pcall around it makes Lua 5.1 panic and
// exit() - the game would close with no record (round 2 R2-4: a global lookup can run an __index metamethod).
static int tostr_fn(lua_State *L) {   // (value) -> tostring(value) or the error message, on top either way
    l_getfield(L, LUA_GLOBALSINDEX, "tostring");
    l_pushvalue(L, 1);
    l_pcall(L, 1, 1, 0);
    return 1;
}
static const char *to_text(lua_State *L, int idx) {
    l_pushcclosure(L, tostr_fn, 0);
    l_pushvalue(L, idx);
    if (l_pcall(L, 1, 1, 0)) return "(tostring failed)";
    const char *s = l_tolstring(L, -1, NULL);
    return s ? s : "(tostring gave no string)";
}

// ext_print(...): like print, into the console and lua_ext.log
static int ext_print(lua_State *L) {
    char line[2048];
    int n = l_gettop(L), len = 0;
    line[0] = 0;
    for (int i = 1; i <= n && len < (int)sizeof line - 1; i++) {
        len += snprintf(line + len, sizeof line - len, i > 1 ? "\t%s" : "%s", to_text(L, i));
        l_settop(L, n);
    }
    out(line);
    return 0;
}

// Runs code in T; prints what it returns. A line that compiles as an expression is returned ("1+1" prints 2).
static void run(lua_State *T, const char *code, size_t len, const char *name, int show) {
    int top = l_gettop(T);
    static char buf[16384];
    int r = 1;
    if (show && len < sizeof buf - 40) {
        int k = snprintf(buf, sizeof buf, "local print=ext_print return ");
        memcpy(buf + k, code, len);
        r = l_loadbuffer(T, buf, k + len, name);
        if (r) {
            l_settop(T, top);
            k = snprintf(buf, sizeof buf, "local print=ext_print ");
            memcpy(buf + k, code, len);
            r = l_loadbuffer(T, buf, k + len, name);
        }
    } else
        r = l_loadbuffer(T, code, len, name);
    if (!r) r = l_pcall(T, 0, show ? LUA_MULTRET : 0, 0);
    if (r) {
        const char *e = l_tolstring(T, -1, NULL);
        outf("error: %s", e ? e : "(no message)");
    } else if (show) {
        int n = l_gettop(T) - top;
        for (int i = 1; i <= n && i <= 16; i++) {
            outf("%s", to_text(T, top + i));
            l_settop(T, top + n);
        }
    }
    l_settop(T, top);
}

static void load_scripts(lua_State *T) {
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*.lua", g_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\%ls", g_dir, fd.cFileName);
        FILE *f = _wfopen(path, L"rb");
        if (!f) continue;
        static char text[1 << 20];
        size_t n = fread(text, 1, sizeof text, f);
        fclose(f);
        char name[MAX_PATH + 1];
        snprintf(name, sizeof name, "@lua_ext/%ls", fd.cFileName);
        if (n == sizeof text) { outf("%s: over 1 MB, not run", name + 1); continue; }
        outf("running %s", name + 1);
        run(T, text, n, name, 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int g_has;
static int has_fn(lua_State *L) {   // (name): sets g_has
    const char *k = l_tolstring(L, 1, NULL);
    l_getfield(L, LUA_GLOBALSINDEX, k ? k : "");
    g_has = TOP_TYPE(L) != 0;
    return 0;
}
static int has_global(lua_State *T, const char *k) {
    int top = l_gettop(T);
    g_has = 0;
    l_pushcclosure(T, has_fn, 0);
    l_pushstring(T, k);
    if (l_pcall(T, 1, 0, 0)) g_has = 0;
    l_settop(T, top);
    return g_has;
}
static int setup_fn(lua_State *L) {   // ext_print into the environment's globals
    l_pushcclosure(L, ext_print, 0);
    l_setfield(L, LUA_GLOBALSINDEX, "ext_print");
    return 0;
}

static void list_envs(void) {
    if (!g_nenv) { out("no script environment (the main menu has none; load a campaign or battle)"); return; }
    for (unsigned i = 0; i < g_nenv; i++) {
        lua_State *T = g_env[i].T;
        int sel = g_sel ? g_env[i].id == g_sel : i == g_nenv - 1;
        outf("%c env %u: %s%s%s", sel ? '*' : ' ', g_env[i].id, has_global(T, "scripting") ? "scripting " : "",
             has_global(T, "events") ? "events " : "", has_global(T, "bm") ? "bm " : "");
    }
}

// The environment list as it is now; new ones are kept with frames = 0
static void refresh_envs(void) {
    void *seen[MAXENV];
    unsigned n = 0;
    for (char *p = *g_first; p && p != g_end && n < MAXENV; p = *(char **)(p + 4)) seen[n++] = *(void **)(p + 8);
    unsigned k = 0;
    for (unsigned i = 0; i < g_nenv; i++) {   // keep the ones still listed with the same thread
        unsigned j = 0;
        while (j < n && (seen[j] != g_env[i].env || *(lua_State **)seen[j] != g_env[i].T)) j++;
        if (j < n) g_env[k++] = g_env[i];
        else if (g_env[i].loaded) outf("env %u closed", g_env[i].id);
    }
    g_nenv = k;
    for (unsigned j = 0; j < n; j++) {
        unsigned i = 0;
        while (i < g_nenv && g_env[i].env != seen[j]) i++;
        if (i == g_nenv && g_nenv < MAXENV && *(lua_State **)seen[j])
            g_env[g_nenv++] = (typeof(g_env[0])){seen[j], *(lua_State **)seen[j], g_next_id++, 0, 0};
    }
}

static lua_State *selected(void) {
    for (unsigned i = g_nenv; i--;)
        if ((!g_sel || g_env[i].id == g_sel) && g_env[i].loaded) return g_env[i].T;
    return NULL;
}

// ---- the console window ----
#define QN 64
static char g_q[QN][1024];
static volatile LONG g_qhead, g_qtail;   // the reader thread adds at head, the main thread takes from tail
static CRITICAL_SECTION g_qlock;
static HWND g_con;

static DWORD WINAPI reader(void *x) {
    (void)x;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char line[1024];
    DWORD got;
    while (ReadConsoleA(in, line, sizeof line - 1, &got, NULL)) {
        while (got && (line[got - 1] == '\n' || line[got - 1] == '\r')) got--;
        line[got] = 0;
        if (!got) continue;
        EnterCriticalSection(&g_qlock);
        if (g_qhead - g_qtail < QN) strcpy(g_q[g_qhead++ % QN], line);
        LeaveCriticalSection(&g_qlock);
    }
    return 0;
}

static BOOL WINAPI console_ctrl(DWORD ev) { return ev == CTRL_C_EVENT || ev == CTRL_BREAK_EVENT; }
static void toggle_console(void) {
    if (!g_con) {
        if (!AllocConsole()) return;
        g_con = GetConsoleWindow();
        SetConsoleTitleA("Attila Lua console - Ctrl+` hides it");
        SetConsoleCtrlHandler(NULL, TRUE);   // Ctrl+C must not end the game
        SetConsoleCtrlHandler(console_ctrl, TRUE);   // nor Ctrl+Break (round 2 R2-5)
        DeleteMenu(GetSystemMenu(g_con, FALSE), SC_CLOSE, MF_BYCOMMAND);   // closing a console window ends its process
        out("Lua console. Lines run in the selected script environment; '=' is not needed, \"1+1\" prints 2.\r\n"
            "/envs lists the environments, /env N selects one (/env 0: the newest), /reload runs lua_ext\\*.lua again.\r\n"
            "Hide it with Ctrl+` - closing this window from the taskbar closes the game too.");
        CloseHandle(CreateThread(NULL, 0, reader, NULL, 0, NULL));
        return;
    }
    ShowWindow(g_con, IsWindowVisible(g_con) ? SW_HIDE : SW_SHOW);
    if (IsWindowVisible(g_con)) SetForegroundWindow(g_con);
}

static int our_window(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return w && (pid == GetCurrentProcessId() || w == g_con);
}

static void command(char *line) {
    outf("> %s", line);
    if (line[0] == '/') {
        if (!strcmp(line, "/envs")) list_envs();
        else if (!strncmp(line, "/env ", 5)) { g_sel = (unsigned)atoi(line + 5); list_envs(); }
        else if (!strcmp(line, "/reload")) {
            lua_State *T = selected();
            if (T) load_scripts(T);
            else out("no script environment");
        } else out("commands: /envs, /env N, /reload");
        return;
    }
    lua_State *T = selected();
    if (!T) { out(g_sel ? "that environment is gone (/env 0 selects the newest)" : "no script environment yet (load a campaign or battle)"); return; }
    if (L_STATUS(T)) { out("that environment is busy (suspended), try again"); return; }
    run(T, line, strlen(line), "=console", 1);
}

static int g_key_down;
void scriptext_frame(void) {
    int down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_OEM_3) & 0x8000);
    if (down && !g_key_down && our_window(GetForegroundWindow())) toggle_console();
    g_key_down = down;
    refresh_envs();
    for (unsigned i = 0; i < g_nenv; i++) {
        if (g_env[i].loaded || ++g_env[i].frames < 2 || L_STATUS(g_env[i].T)) continue;
        lua_State *T = g_env[i].T;
        g_env[i].loaded = 1;
        int top = l_gettop(T);
        l_pushcclosure(T, setup_fn, 0);
        if (l_pcall(T, 0, 0, 0)) outf("env %u: ext_print could not be set: %s", g_env[i].id, to_text(T, l_gettop(T)));
        l_settop(T, top);
        outf("env %u opened%s", g_env[i].id, has_global(T, "scripting") ? " (campaign scripting)" : "");
        load_scripts(T);
    }
    while (g_qtail != g_qhead) {
        char line[1024];
        EnterCriticalSection(&g_qlock);
        strcpy(line, g_q[g_qtail % QN]);
        g_qtail++;
        LeaveCriticalSection(&g_qlock);
        command(line);
    }
}

// b: the game DLL's image, va: the address it was relocated to (b itself in the game). Sets the API pointers.
static int find_lua(unsigned char *b, DWORD va) {
    unsigned st = game_rva(b, RVA_STATE), el = game_rva(b, RVA_ENVLIST);
    int ok = st && el && !memcmp(b + 0x1626190, "\x83\xEC\x0C\x56\x8B\x35", 6) && *(DWORD *)(b + 0x1626196) == va + st
             && !memcmp(b + 0x16238A0, "\x8B\x15", 2) && *(DWORD *)(b + 0x16238A2) == va + el;
    for (unsigned i = 0; ok && i < sizeof API / sizeof *API; i++) {
        unsigned r = game_rva(b, API[i].rva);
        ok = r && !memcmp(b + r, API[i].b, API[i].n);
        *API[i].fn = b + r;
    }
    return ok;
}

// dir: the game folder. Returns 1 when on (lua_ext\ exists and the game's Lua is where expected).
int install_scriptext(HMODULE m, const wchar_t *dir, void (*log)(const char *fmt, ...)) {
    swprintf(g_dir, MAX_PATH, L"%ls\\lua_ext", dir);
    DWORD a = GetFileAttributesW(g_dir);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    unsigned char *b = (unsigned char *)m;
    unsigned el = game_rva(m, RVA_ENVLIST);
    if (!find_lua(b, (DWORD)(UINT_PTR)b)) {
        log("script extender: lua_ext folder found, but this game build's Lua is not where expected - off");
        return 0;
    }
    g_first = (void **)(b + el);
    g_end = (char *)(b + el + 4);
    InitializeCriticalSection(&g_qlock);
    wchar_t lp[MAX_PATH];
    swprintf(lp, MAX_PATH, L"%ls\\lua_ext.log", g_dir);
    g_log = _wfopen(lp, L"w");
    log("script extender on: lua_ext\\*.lua run in each script environment, Ctrl+` opens the Lua console (lua_ext\\lua_ext.log)");
    return 1;
}
