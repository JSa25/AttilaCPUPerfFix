// ---- Total War Mod Manager's own parts of the window: the two versions, their settings, and what Play starts.

// What the window shows for the version in use.
static void host_read_settings(void) {
    char v[64];
    g_set_size = settings_size_index();
    g_set_sharp = sol_get("texture_sharpness", v, sizeof v) && v[0] >= '0' && v[0] <= '4' && !v[1] ? v[0] - '0' + 1 : 0;
    g_set_shadow = sol_int("shadow_refresh", 1, 1, 4) - 1;
    g_set_fps = sol_flag("show_fps", 0);
    for (int a = 0; a < N_ADDONS; a++) g_addon_disk[a] = addon_on_disk(a);
    if (g_mode) {
        g_perf = sol_flag("perf_fix", 1);
        g_flags = sol_flag("flag_wave", 1);
        g_up = sol_get("upscaling", v, sizeof v) ? v[0] != '0' : addon_upscale_on();
    } else {
        char on[MAX_PATH];
        joinp(on, sizeof on, g_game, "dinput8.dll");
        g_perf = exists(on);   // off = dinput8.dll.off
        g_flags = 1;
        g_up = addon_upscale_on();
    }
    if (!g_addon_disk[AD_UPSCALE]) g_up = 0;
    g_dx11_note = g_mode && sol_get("graphics", v, sizeof v) && !_stricmp(v, "opengl_fallback");
    g_crashed = last_game_crashed();
    g_crash_closed = 0;
}

static void ask_open(int kind);   // the questions: picker_ui.inc / below

static void host_switch_mode(int m) {
    if (m == g_mode) return;
    if (m && !g_have64) { ask_open(ASK_64); return; }   // not installed: offer to subscribe and install it
    set_mode(m);
    host_read_settings();
    g_io.want_version = 1;   // the engine fix's version for this version of the game
    SetEvent(g_io.wake);
    g_sel = g_nmods ? 0 : -1;
    g_scroll = 0;
    g_filter = 0;
    changed();
    if (m && !g_sol_ok) ui_toast(1, "This 64-bit Attila is newer than this launcher knows: Play opens its own window, with the ticks made here.");
}

// ---- "Game files": the game's own files for the version in use. 32-bit: %APPDATA%\The Creative Assembly\Attila; 64-bit
// Attila keeps its own copy in %USERPROFILE%\attila64_home\.Creative Assembly\Attila. 0 = the preferences file (opens in the
// player's editor for .txt), 1 = the scripts folder, 2 = the save folder.
static void game_files(int which) {
    static const char *const sub[3] = {"scripts\\preferences.script.txt", "scripts", "save_games"};
    static const char *const none[3] = {"There is no preferences file yet: the game writes it the first time it runs.",
                                        "There is no scripts folder yet: the game makes it the first time it runs.",
                                        "There are no saves yet for this version of the game."};
    const char *env = getenv(g_mode ? "USERPROFILE" : "APPDATA");
    char p[MAX_PATH * 2], arg[MAX_PATH * 2 + 4];
    snprintf(p, sizeof p, g_mode ? "%s\\attila64_home\\.Creative Assembly\\Attila\\%s" : "%s\\The Creative Assembly\\Attila\\%s",
             env ? env : "", sub[which < 0 || which > 2 ? 0 : which]);
    if (!env || !exists(p)) { ui_toast(1, none[which < 0 || which > 2 ? 0 : which]); return; }
    snprintf(arg, sizeof arg, "\"%s\"", p);
    open_explorer(arg);
}

// ---- 64-bit Attila not installed: subscribe to it (Sol Invictus's Workshop item) and run its own installer, which copies
// its files into attila64 and gets Steam's 64-bit game file. Its installer asks on the keyboard; the answers are given from
// a file (cmd's < ; a pipe through cmd would add spaces): "1" (update) when an attila64 folder is there already, then "n" to
// its question about Steam's Play button, which keeps opening this launcher. Everything else shows in the installer's own
// window (Steam's console download of the game file needs the player).
static int g_inst64;   // 1 = waiting for Steam's download of 64-bit Attila, 2 = its installer is running
static int sol_installer(char *out, size_t n) {   // its installer in its Workshop item (Install*.exe); 1 = there
    char root[MAX_PATH], pat[MAX_PATH * 2];
    workshop_root(root, sizeof root);
    snprintf(pat, sizeof pat, "%s/" SOL_ITEM "/Install*.exe", root);
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    snprintf(out, n, "%s/" SOL_ITEM "/%s", root, f.cFileName);
    for (char *c = out; *c; c++) if (*c == '/') *c = '\\';
    return 1;
}
typedef struct { char cmd[MAX_PATH * 5], dir[MAX_PATH * 2], answers[MAX_PATH]; } Inst64;
static DWORD WINAPI inst64_thread(void *arg) {
    Inst64 *j = arg;
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi = {0};
    DWORD code = 3;
    char sys[MAX_PATH], exe[MAX_PATH + 16];
    GetSystemDirectoryA(sys, sizeof sys);
    snprintf(exe, sizeof exe, "%s\\cmd.exe", sys);
    if (CreateProcessA(exe, j->cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, j->dir, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    lg("64-bit Attila's installer ended (code %lu): %s", code, j->cmd);
    DeleteFileA(j->answers);
    free(j);
    PostMessageA(g_win, WM_P_INST64, 0, (LPARAM)code);
    return 0;
}
static void inst64_run(void) {
    char exe[MAX_PATH * 2], a64[MAX_PATH];
    if (!sol_installer(exe, sizeof exe)) { g_inst64 = 0; ui_toast(2, "64-bit Attila's installer is not in its Workshop folder. Its Workshop page opens in Steam."); open_page(SOL_ITEM_ID); return; }
    joinp(a64, sizeof a64, g_game, "attila64");
    Inst64 *j = calloc(1, sizeof *j);
    if (!j) return;
    joinp(j->answers, sizeof j->answers, g_home, "installer_answers.txt");
    if (!write_text_file(j->answers, exists(a64) ? "1\r\nn\r\n" : "n\r\n")) { free(j); g_inst64 = 0; ui_toast(2, "Could not start 64-bit Attila's installer."); return; }
    snprintf(j->cmd, sizeof j->cmd, "cmd.exe /d /s /c \"\"%s\" < \"%s\"\"", exe, j->answers);
    snprintf(j->dir, sizeof j->dir, "%s", exe);
    char *q = strrchr(j->dir, '\\');
    if (q) *q = 0;
    g_inst64 = 2;
    HANDLE t = CreateThread(NULL, 0, inst64_thread, j, 0, NULL);
    if (!t) { free(j); g_inst64 = 0; return; }
    CloseHandle(t);
    ui_toast(1, "64-bit Attila's installer is open in its own window. Follow it there; this launcher switches to 64-bit when it is done.");
}
static void inst64_start(void) {
    char exe[MAX_PATH * 2];
    if (g_inst64) { ui_toast(1, "64-bit Attila is on its way: its download or its installer is still running."); return; }
    WsView v = ws_view(SOL_ITEM_ID);
    if (sol_installer(exe, sizeof exe) && (!v.known || (v.installed && !v.busy))) { inst64_run(); return; }   // Steam has it already
    g_inst64 = 1;
    subscribe(SOL_ITEM_ID);
    ui_toast(1, "Steam is downloading 64-bit Attila. Its installer opens by itself when the download is done.");
}
static void inst64_check(void) {   // Steam's state changed (on_steam)
    WsView v = ws_view(SOL_ITEM_ID);
    if (g_inst64 == 1 && v.known && v.installed && !v.busy) inst64_run();
}
static void inst64_done(int code) {
    g_inst64 = 0;
    have64_check();
    if (!g_have64) {
        ui_toast(2, code ? "64-bit Attila's installer stopped before the end. Press 64-bit to try again."
                         : "64-bit Attila is not in place yet. If its installer asked for administrator rights, finish in that window, then press 64-bit again.");
        return;
    }
    static ListEntry o[MAX_MODS];   // its list: the 32-bit one to start with, as 64-bit Attila does
    g_snap_n[1] = read_list(1, o, MAX_MODS);
    memcpy(g_snap[1], o, sizeof(ListEntry) * (size_t)g_snap_n[1]);
    host_switch_mode(1);
    ui_toast(0, "64-bit Attila is installed. Steam's Play still opens this launcher.");
}

// ---- The questions (picker_ui.inc draws them): CA's launcher this time or from now on, back from CA's launcher, and
// 64-bit Attila not installed. Answer 0 = the first button, 1 = the second, 2 = Cancel / Esc.
static void ask_answer(int kind, int a) {
    g_sheet = SH_NONE;
    SetFocus(g_win);
    if (kind == ASK_CA) {   // 0 = this time only (Enter), 1 = from now on
        if (a == 1) {
            if (launcher_choice_put(2, 0)) lg("heapfix.ini: launcher=2 (CA's launcher from now on)");
            else ui_toast(2, "Could not write heapfix.ini: CA's launcher opens this time only.");
        }
        if (a <= 1) host_play(GO_CA);
    } else if (kind == ASK_BACK) {
        if (a == 0) {
            if (launcher_choice_put(1, 0)) ui_toast(0, "Steam's Play opens Total War Mod Manager again (heapfix.ini: launcher=1).");
            else ui_toast(2, "Could not write heapfix.ini. Set launcher=1 there by hand.");
        }
    } else if (kind == ASK_64 && a == 0) inst64_start();
    InvalidateRect(g_win, NULL, FALSE);
}

static void host_same_list(int on) {
    g_same_list = on;
    settings_put("same_list", on ? "1" : "0");
    if (on) {   // the other version takes this one's list now
        g_snap_n[!g_mode] = ticked_entries(g_snap[!g_mode], MAX_MODS);
        ui_toast(0, "One mod list for 32-bit and 64-bit: both start with these mods in this order.");
    } else ui_toast(0, "32-bit and 64-bit keep their own mod lists again (both start from this one).");
    InvalidateRect(g_win, NULL, FALSE);
}

// Play (go = GO_GAME), the Launcher menu's CA's launcher (GO_CA) or 64-bit Attila's own window (GO_OWN_WINDOW): everything
// is saved, the window closes, and WinMain starts it and waits.
static void host_play(int go) {
    save_list();
    char v[16];
    if (g_mode) {   // 64-bit Attila's settings, as its own window writes them
        settings_save_size(g_set_size);
        int sharp = g_set_sharp - 1;
        snprintf(v, sizeof v, "%d", sharp);
        ini_put(g_sol_ini, "texture_sharpness", sharp < 0 || sharp > 4 ? "game" : v);
        snprintf(v, sizeof v, "%d", g_set_shadow + 1);
        ini_put(g_sol_ini, "shadow_refresh", v);
        ini_put(g_sol_ini, "show_fps", g_set_fps ? "1" : "0");
        if (g_addon_disk[AD_PERF]) {
            ini_put(g_sol_ini, "perf_fix", g_perf ? "1" : "0");
            ini_put(g_sol_ini, "flag_wave", g_flags ? "1" : "0");
            if (g_addon_disk[AD_UPSCALE]) ini_put(g_sol_ini, "upscaling", g_up ? "1" : "0");
        }
        if (go == GO_GAME && !g_sol_ok) go = GO_OWN_WINDOW;   // a newer 64-bit Attila than tested: its own window, safe
    } else {
        char on[MAX_PATH], off[MAX_PATH], fb[MAX_PATH];
        joinp(on, sizeof on, g_game, "dinput8.dll");
        joinp(off, sizeof off, g_game, "dinput8.dll.off");
        if (g_perf && !exists(on) && exists(off)) MoveFileExA(off, on, 0);
        else if (!g_perf && exists(on)) MoveFileExA(on, off, MOVEFILE_REPLACE_EXISTING);
        joinp(fb, sizeof fb, g_game, "FrameBridge");
        if (g_addon_disk[AD_UPSCALE] && exists(fb)) { joinp(fb, sizeof fb, g_game, "FrameBridge\\settings.txt"); ini_put(fb, "addon", g_up ? "on" : "off"); }
    }
    settings_put("same_list", g_same_list ? "1" : "0");
    settings_put("mode", g_mode ? "64" : "32");
    g_go = go;
    g_result = 1;
    DestroyWindow(g_win);
}
