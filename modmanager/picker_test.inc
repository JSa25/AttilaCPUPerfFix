// ---- ATTILA_PICKER_TEST (tests only; redo of the start window, 2026-11-25): a script of steps run in the window, with
// pictures of it, so every state can be checked without anyone clicking. ATTILA_PICKER_TEST=@file reads the steps from a
// file (one per line), otherwise they are separated by ';'. Positions are in 100%-screen units from the client area's corner.
//   theme:dark  scale:150  size:1180x780  click:x,y  dbl:x,y  move:x,y  key:[ctrl+|shift+|alt+]name  type:text  search:text
//   filter:n  select:n  menu:preset|import|export|theme|log|set0|set1|set2  import:file  crash:1  dx11:1  toast:text
//   tip:x,y  wait:ms  shot:file.png  dump:file.txt  unsubscribe:id  play  quit
// ATTILA_PICKER_FAKE_STEAM="id:state:done:total,..." (picker_work.inc) stands in for Steam.
static char *g_script, *g_script_at;
static DWORD g_open_ms;   // from the start of mod_picker_run to the first drawing

static void shot(const char *path) {
    RECT r;
    UpdateWindow(g_win);
    GetWindowRect(g_win, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC sdc = GetDC(NULL), dc = CreateCompatibleDC(sdc);
    HBITMAP b = CreateCompatibleBitmap(sdc, w, h), o = (HBITMAP)SelectObject(dc, b);
    PrintWindow(g_win, dc, 2 /* PW_RENDERFULLCONTENT */);
    SelectObject(dc, o);
    void *img = NULL;
    static const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    WCHAR wp[MAX_PATH * 2];
    if (gp.dll && !gp.from_hbitmap(b, NULL, &img) && MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, MAX_PATH * 2)) gp.save(img, wp, &png, NULL);
    if (img) gp.dispose(img);
    DeleteObject(b);
    DeleteDC(dc);
    ReleaseDC(NULL, sdc);
}

static void dump(const char *path) {   // what is on screen, for the checks in tests\gui_check.py
    FILE *f = fopen(path, "wb");   // binary: text mode mangled lines with non-English characters (UTF-8 code page)
    if (!f) return;
    fprintf(f, "overlay %d\n", (g_menu.owner || g_sheet) ? g_overlay_from : -1);
    fprintf(f, "mode %d\n", g_mode);   // Total War Mod Manager: 0 = 32-bit, 1 = 64-bit
    {   // Total War Mod Manager: the question on screen, the list code, the 64-bit install, Steam items asked for
        static ListEntry e[MAX_MODS];
        char code[10];
        list_code(e, ticked_entries(e, MAX_MODS), code);
        fprintf(f, "ask %d\ncode %s\nhave64 %d\ninst64 %d\n", g_sheet == SH_ASK ? g_ask : -1, code, g_have64, g_inst64);
        AcquireSRWLockShared(&g_ws_lock);
        for (int i = 0; i < g_ws_n; i++) fprintf(f, "ws %llu asked=%d state=%u\n", g_ws_items[i].id, g_ws_items[i].asked, g_ws_items[i].state);
        ReleaseSRWLockShared(&g_ws_lock);
    }
    fprintf(f, "open_ms %lu\nscale %.2f\nclient %d %d\ntheme %s\npreset %s\nsteam %ld\nfilter %d\nsel %d\nsheet %d\nmenu %d\n", g_open_ms, g_scale, g_w, g_h,
            g_theme_keys[g_theme], g_preset, g_steam, g_filter, g_sel, g_sheet, g_menu.owner);
    for (int i = 0; i < g_nmods; i++)
        fprintf(f, "mod %d on=%d missing=%d ws=%llu under=%d by=%d over=%d of=%d thumb=%d %s\n", i, g_mods[i].on, g_mods[i].missing, g_mods[i].ws,
                g_mods[i].under_n, g_mods[i].under_by, g_mods[i].over_n, g_mods[i].over_of, g_mods[i].thumb_state, g_mods[i].name);
    for (int i = 0; i < g_nhits; i++)
        fprintf(f, "hit %d %d %d %ld %ld %ld %ld %s\n", g_hits[i].id, g_hits[i].arg, g_hits[i].focus, g_hits[i].r.left, g_hits[i].r.top, g_hits[i].r.right, g_hits[i].r.bottom,
                g_hits[i].tip ? g_hits[i].tip : "");
    RECT rs;
    GetWindowRect(g_search, &rs);
    MapWindowPoints(NULL, g_win, (POINT *)&rs, 2);
    fprintf(f, "edit_search %d %ld %ld %ld %ld\n", IsWindowVisible(g_search), rs.left, rs.top, rs.right, rs.bottom);
    fprintf(f, "toast %s\n", g_toast);
    for (int a = 0; a < N_ADDONS; a++) {
        unsigned s = 0;
        unsigned long long d = 0, t = 0;
        int known = ws_get(g_addon_ws[a], &s, &d, &t);
        fprintf(f, "addon %llu known=%d state=%u done=%llu total=%llu disk=%d\n", g_addon_ws[a], known, s, d, t, g_addon_disk[a]);
    }
    fclose(f);
}

static int vk_of(const char *s) {
    static const struct { const char *n; int vk; } k[] = {{"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT}, {"space", VK_SPACE},
        {"enter", VK_RETURN}, {"esc", VK_ESCAPE}, {"tab", VK_TAB}, {"home", VK_HOME}, {"end", VK_END}, {"pgup", VK_PRIOR}, {"pgdn", VK_NEXT}, {"f5", VK_F5}};
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!_stricmp(s, k[i].n)) return k[i].vk;
    return strlen(s) == 1 ? toupper((unsigned char)s[0]) : 0;
}

static void test_step(void) {
    if (!g_script) {
        const char *t = getenv("ATTILA_PICKER_TEST");
        if (!t) { KillTimer(g_win, T_SCRIPT); return; }
        if (t[0] == '@') g_script = read_text_file(t + 1);
        else if ((g_script = malloc(strlen(t) + 1))) { strcpy(g_script, t); for (char *c = g_script; *c; c++) if (*c == ';') *c = '\n'; }
        if (!g_script) { KillTimer(g_win, T_SCRIPT); return; }
        g_script_at = g_script;
    }
    KillTimer(g_win, T_SCRIPT);
    while (g_script_at && *g_script_at) {
        char *line_start = g_script_at;
        char line[1024];
        size_t n = strcspn(g_script_at, "\r\n");
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), g_script_at);
        g_script_at += n;
        while (*g_script_at == '\r' || *g_script_at == '\n') g_script_at++;
        char *a = strchr(line, ':'), *cmd = trim(line);
        if (a) *a++ = 0;
        if (!cmd[0] || cmd[0] == '#') continue;
        int x = 0, y = 0;
        if (a && sscanf(a, "%d,%d", &x, &y) == 2) { x = S((float)x); y = S((float)y); }
        if (!strcmp(cmd, "theme")) { for (int i = 0; i < N_THEMES; i++) if (!_stricmp(a, g_theme_keys[i])) set_theme(i); }
        else if (!strcmp(cmd, "scale")) {
            RECT r;
            GetClientRect(g_win, &r);
            float dw = r.right / g_scale, dh = r.bottom / g_scale;
            g_force_scale = (float)atof(a) / 100.0f;
            set_scale_from_dpi(g_dpi);
            for (int i = 0; i < g_nmods; i++) if (g_mods[i].thumb_state >= 2) g_mods[i].thumb_state = 0;
            RECT w = {0, 0, S(dw), S(dh)};
            adjust_rect(&w);
            SetWindowPos(g_win, NULL, 0, 0, w.right - w.left, w.bottom - w.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (!strcmp(cmd, "size")) {
            int cw = 0, ch = 0;
            if (sscanf(a, "%dx%d", &cw, &ch) == 2) {
                RECT w = {0, 0, S((float)cw), S((float)ch)};
                adjust_rect(&w);
                MONITORINFO mi = {sizeof mi};   // all of it on one screen, or the picture has holes
                GetMonitorInfoA(MonitorFromWindow(g_win, MONITOR_DEFAULTTONEAREST), &mi);
                SetWindowPos(g_win, NULL, mi.rcWork.left, mi.rcWork.top, w.right - w.left, w.bottom - w.top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        } else if (!strcmp(cmd, "click") || !strcmp(cmd, "dbl")) {
            UpdateWindow(g_win);
            mouse_move(x, y, 0);
            mouse_down(x, y, cmd[0] == 'd');
            mouse_up(x, y);
        } else if (!strcmp(cmd, "move")) { UpdateWindow(g_win); mouse_move(x, y, 0); }
        else if (!strcmp(cmd, "drag")) {   // drag:x,y,dy (dy in 100% units)
            int dy = 0;
            sscanf(a, "%*d,%*d,%d", &dy);
            UpdateWindow(g_win);
            mouse_move(x, y, 0);
            mouse_down(x, y, 0);
            for (int k = 1; k <= 8; k++) { mouse_move(x, y + S((float)dy) * k / 8, MK_LBUTTON); UpdateWindow(g_win); }
            mouse_up(x, y + S((float)dy));
        } else if (!strcmp(cmd, "tip")) { UpdateWindow(g_win); mouse_move(x, y, 0); g_tip_ready = 1; InvalidateRect(g_win, NULL, FALSE); }
        else if (!strcmp(cmd, "key")) {
            g_fake_mods = 0;
            char *k = a;
            for (;;) {
                if (!_strnicmp(k, "ctrl+", 5)) { g_fake_mods |= 1; k += 5; }
                else if (!_strnicmp(k, "shift+", 6)) { g_fake_mods |= 2; k += 6; }
                else if (!_strnicmp(k, "alt+", 4)) { g_fake_mods |= 4; k += 4; }
                else break;
            }
            HWND f = GetFocus();
            int vk = vk_of(k);
            g_kbd = g_kbd || vk == VK_TAB;
            if (f == g_search || f == g_name_edit) SendMessageW(f, WM_KEYDOWN, (WPARAM)vk, 0);
            else key_down(vk);
            g_fake_mods = 0;
        } else if (!strcmp(cmd, "type")) {
            int nw;
            WCHAR *w = wtext(a, &nw);
            HWND f = GetFocus() == g_name_edit ? g_name_edit : g_search;
            for (int i = 0; i < nw; i++) SendMessageW(f, WM_CHAR, w[i], 0);
        } else if (!strcmp(cmd, "search")) { int nw; SetWindowTextW(g_search, wtext(a, &nw)); }
        else if (!strcmp(cmd, "filter")) activate(W_FILTER, atoi(a));
        else if (!strcmp(cmd, "select")) select_mod(atoi(a), 1);
        else if (!strcmp(cmd, "menu")) {
            static const char *const names[] = {"preset", "import", "export", "theme", "log", "set0", "set1", "set2"};
            static const int ids[] = {W_PRESET, W_IMPORT, W_EXPORT, W_THEME, W_LOG, W_SETTING, W_SETTING, W_SETTING};
            UpdateWindow(g_win);
            for (int i = 0; i < 8; i++) if (!_stricmp(a, names[i])) activate(ids[i], i >= 5 ? i - 5 : 0);
        } else if (!strcmp(cmd, "import")) open_import(read_text_file(a), "From a test file");
        else if (!strcmp(cmd, "crash")) { g_crashed = atoi(a); g_crash_closed = 0; }
        else if (!strcmp(cmd, "mode")) host_switch_mode(atoi(a));        // Total War Mod Manager: 0 = 32-bit, 1 = 64-bit
        else if (!strcmp(cmd, "samelist")) host_same_list(atoi(a));     // one list for both versions
        else if (!strcmp(cmd, "go")) { host_play(atoi(a)); return; }    // 0 Play, 1 64-bit Attila's own window, 2 CA's launcher
        else if (!strcmp(cmd, "dx11")) g_dx11_note = atoi(a);
        else if (!strcmp(cmd, "toast")) ui_toast(0, a);
        else if (!strcmp(cmd, "unsubscribe")) {
            unsigned long long id = strtoull(a, NULL, 10);
            void *(*get_ugc)(void) = (void *(*)(void))steam_api_fn("SteamAPI_SteamUGC_v017");
            unsigned long long (*unsub)(void *, unsigned long long) = (unsigned long long (*)(void *, unsigned long long))steam_api_fn("SteamAPI_ISteamUGC_UnsubscribeItem");
            if (g_steam > 0 && get_ugc && unsub) unsub(get_ugc(), id);
        } else if (!strcmp(cmd, "act")) {   // act:id,arg presses that control (ids: the W_ list in picker_ui.inc)
            int id = 0, arg = 0;
            if (sscanf(a, "%d,%d", &id, &arg) == 2) activate(id, arg);
        }
        else if (!strcmp(cmd, "waitsteam")) {   // waitsteam:id,seconds: until Steam has the item installed and not busy (or time is up)
            static DWORD since;
            unsigned long long id = strtoull(a, NULL, 10);
            const char *c = strchr(a, ',');
            DWORD limit = (DWORD)(c ? atoi(c + 1) : 600) * 1000;
            WsView v = ws_view(id);
            if (!since) since = GetTickCount();
            if (!(v.known && v.installed && !v.busy) && GetTickCount() - since < limit) {
                g_script_at = line_start;   // the same step again in half a second
                SetTimer(g_win, T_SCRIPT, 500, NULL);
                return;
            }
            since = 0;
        } else if (!strcmp(cmd, "wait")) { SetTimer(g_win, T_SCRIPT, (UINT)atoi(a), NULL); InvalidateRect(g_win, NULL, FALSE); return; }
        else if (!strcmp(cmd, "shot")) { InvalidateRect(g_win, NULL, FALSE); shot(a); }
        else if (!strcmp(cmd, "dump")) { InvalidateRect(g_win, NULL, FALSE); UpdateWindow(g_win); dump(a); }
        else if (!strcmp(cmd, "play")) { play(); return; }
        else if (!strcmp(cmd, "quit")) { DestroyWindow(g_win); return; }
        InvalidateRect(g_win, NULL, FALSE);
        UpdateWindow(g_win);
    }
    SetTimer(g_win, T_SCRIPT, 100, NULL);   // the end of the script: wait (a later quit or the person closes it)
    free(g_script);
    g_script = NULL;
    g_script_at = NULL;
    KillTimer(g_win, T_SCRIPT);
}
