// Portions of this file (the start of the window procedure (picker_proc)) come from Sol Invictus's 64-bit Attila (attila64/loader/picker.c) and are under its MIT license: see SOL_INVICTUS_MIT.txt.
// ---- The start window: what clicks and keys do, results from the background threads, and the window itself (redo, 2026-11-25).

static const Hit *find_hit(int id, int arg) {
    for (int i = g_nhits - 1; i >= 0; i--) if (g_hits[i].id == id && g_hits[i].arg == arg) return &g_hits[i];
    return NULL;
}
static const Hit *hit_test(POINT p) {
    for (int i = g_nhits - 1; i >= ((g_menu.owner || g_sheet) ? g_overlay_from : 0); i--)
        if (g_hits[i].id != W_TIP && PtInRect(&g_hits[i].r, p)) return &g_hits[i];
    return NULL;
}

static void title_colors(void) {   // the title bar in the theme's colours (dark mode on Windows 10, colours on Windows 11)
    static HRESULT (WINAPI *set)(HWND, DWORD, const void *, DWORD);
    static int tried;
    if (!tried) { HMODULE d = LoadLibraryA("dwmapi.dll"); tried = 1; if (d) *(FARPROC *)&set = GetProcAddress(d, "DwmSetWindowAttribute"); }
    if (!set || !g_win) return;
    BOOL dark = PAL->dark;
    COLORREF cap = cref(PAL->art ? PAL->panel : PAL->bg), txt = cref(PAL->dim);
    set(g_win, 20, &dark, sizeof dark);   // DWMWA_USE_IMMERSIVE_DARK_MODE
    set(g_win, 35, &cap, sizeof cap);     // DWMWA_CAPTION_COLOR
    set(g_win, 36, &txt, sizeof txt);     // DWMWA_TEXT_COLOR
}
static void set_theme(int t) {
    if (t < 0 || t >= N_THEMES) return;
    g_theme = t;
    if (g_field_brush) DeleteObject(g_field_brush);
    g_field_brush = CreateSolidBrush(cref(PAL->field));
    g_field_rgb = PAL->field;
    title_colors();
    if (g_search) InvalidateRect(g_search, NULL, TRUE);
    InvalidateRect(g_win, NULL, FALSE);
}

static void select_mod(int i, int scroll) {
    g_sel = i;
    if (scroll && i >= 0) ensure_visible(i);
    InvalidateRect(g_win, NULL, FALSE);
}
static void tick(int i) {   // the tick box of row i; the selection stays with the mod
    if (i < 0 || i >= g_nmods) return;
    char keep[160] = "";
    if (g_sel >= 0 && g_sel < g_nmods && g_sel != i) snprintf(keep, sizeof keep, "%s", g_mods[g_sel].name);
    int to = set_tick(i, !g_mods[i].on);
    Mod *k = keep[0] ? find_mod(keep) : NULL;
    g_sel = keep[0] ? (k ? (int)(k - g_mods) : -1) : to;   // the selection stays with its mod
    changed();
}
static void move_sel(int by) {
    int t = ticked_count();
    if (g_sel < 0 || !g_mods[g_sel].on || !reorder_ok()) return;
    int to = g_sel + by;
    if (to < 0 || to >= t) return;
    move_row(g_sel, to);
    g_sel = to;
    changed();
    ensure_visible(g_sel);
}
static void set_all(int on) {
    undo_keep();
    if (on) { for (int i = 0; i < g_nmods; i++) g_mods[i].on = 1; }
    else for (int i = g_nmods - 1; i >= 0; i--) if (g_mods[i].on) set_tick(i, 0);
    sort_rest();
    if (on) {   // the newly ticked ones after the old load order, A to Z
        int t = 0;
        for (int i = 0; i < g_undo_n; i++) { Mod *m = find_mod(g_undo[i].name); if (m) move_row((int)(m - g_mods), t++); }
    }
    g_sel = g_nmods ? 0 : -1;
    changed();
    ui_toast(0, on ? "Every mod is ticked." : "Every mod is unticked: the game starts without mods.");
    g_toast_undo = 1;
}

static void do_undo(void) {
    if (g_undo_n < 0) { ui_toast(1, "Nothing to undo."); return; }
    static ListEntry now[MAX_MODS];
    char preset[64];
    int n = ticked_entries(now, MAX_MODS);
    snprintf(preset, sizeof preset, "%s", g_preset);
    apply_list(g_undo, g_undo_n);
    snprintf(g_preset, sizeof g_preset, "%s", g_undo_preset);
    if (g_preset[0]) { int k = preset_read(g_preset, g_preset_list, MAX_MODS); g_preset_n = k; if (k < 0) g_preset[0] = 0; }
    memcpy(g_undo, now, sizeof(ListEntry) * (size_t)n);   // undo again = redo
    g_undo_n = n;
    snprintf(g_undo_preset, sizeof g_undo_preset, "%s", preset);
    g_sel = g_nmods ? 0 : -1;
    changed();
    ui_toast(0, "Back to the list before. Ctrl+Z again goes forward.");
    g_toast_undo = 1;
}

static void switch_preset(const char *name) {
    static ListEntry e[MAX_MODS];
    int n = preset_read(name, e, MAX_MODS);
    if (n < 0) { ui_toast(2, "Could not read that preset."); return; }
    undo_keep();
    int miss = apply_list(e, n);
    snprintf(g_preset, sizeof g_preset, "%s", name);
    memcpy(g_preset_list, e, sizeof(ListEntry) * (size_t)n);
    g_preset_n = n;
    g_sel = g_nmods ? 0 : -1;
    g_scroll = 0;
    changed();
    char msg[200];
    if (miss) snprintf(msg, sizeof msg, "Preset \"%s\" in use: %d mod%s, %d of them not on this PC.", name, n, n == 1 ? "" : "s", miss);
    else snprintf(msg, sizeof msg, "Preset \"%s\" in use: %d mod%s.", name, n, n == 1 ? "" : "s");
    ui_toast(0, msg);
    g_toast_undo = 1;
}

static void subscribe(unsigned long long ws) {
    if (!ws) return;
    g_last_asked = ws;
    ws_subscribe(ws);
    InvalidateRect(g_win, NULL, FALSE);
}
static void subscribe_missing(const ListEntry *e, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        Mod *m = find_mod(e[i].name);
        if ((!m || m->missing) && e[i].ws && !ws_view(e[i].ws).subscribed) { subscribe(e[i].ws); k++; }
    }
    if (k) ui_toast(1, k == 1 ? "Steam is downloading 1 mod. It is ticked in its place when it arrives." : "Steam is downloading the mods. Each is ticked in its place when it arrives.");
}

static void name_sheet(int for_import) {
    g_sheet = SH_NAME;
    g_name_for = for_import;
    g_name_err[0] = 0;
    g_name_overwrite = 0;
    SetWindowTextW(g_name_edit, L"");
    InvalidateRect(g_win, NULL, FALSE);
    UpdateWindow(g_win);   // places the edit box
    SetFocus(g_name_edit);
}
static void name_save(void) {
    WCHAR w[128];
    char name[256];
    GetWindowTextW(g_name_edit, w, 128);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, name, sizeof name, NULL, NULL);
    char *n = trim(name);
    if (!preset_name_ok(n)) {
        snprintf(g_name_err, sizeof g_name_err, n[0] ? "That name cannot be a file name. Use letters, numbers, spaces and - _ ( ) , . ' & + !" : "Type a name first.");
        g_name_overwrite = 0;
        InvalidateRect(g_win, NULL, FALSE);
        return;
    }
    char path[MAX_PATH];
    preset_path(n, path, sizeof path);
    if (!g_name_overwrite && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        snprintf(g_name_err, sizeof g_name_err, "There is a preset called \"%s\" already. Press Replace it to save over it, or type another name.", n);
        g_name_overwrite = 1;
        InvalidateRect(g_win, NULL, FALSE);
        return;
    }
    if (g_name_for) { undo_keep(); apply_list(g_imp, g_imp_n); subscribe_missing(g_imp, g_imp_n); }
    if (!preset_write(n)) { snprintf(g_name_err, sizeof g_name_err, "Could not save it (is the TotalWarModManager folder read-only?)."); InvalidateRect(g_win, NULL, FALSE); return; }
    g_sheet = SH_NONE;
    SetFocus(g_win);
    g_sel = g_nmods ? 0 : -1;
    changed();
    char msg[120];
    snprintf(msg, sizeof msg, "Saved as preset \"%s\".", n);
    ui_toast(0, msg);
    g_toast_undo = g_name_for;
}

static void open_import(char *text, const char *from) {
    g_imp_n = text ? list_parse(text, g_imp, MAX_MODS, g_imp_code) : 0;
    free(text);
    if (!g_imp_n) { ui_toast(2, pool("No mod list found in %s. A list has one line per mod: its Workshop number (or \"data\") and the pack's name.", from)); return; }
    snprintf(g_imp_from, sizeof g_imp_from, "%s", from);
    for (int i = 0; i < g_imp_n; i++) ws_watch(g_imp[i].ws);
    g_sheet = SH_IMPORT;
    g_sheet_scroll = 0;
    menu_close();
    SetFocus(g_win);
    InvalidateRect(g_win, NULL, FALSE);
}
static void import_use(void) {
    undo_keep();
    int miss = apply_list(g_imp, g_imp_n);
    g_preset[0] = 0;
    g_sheet = SH_NONE;
    g_sel = g_nmods ? 0 : -1;
    g_scroll = 0;
    changed();
    ui_toast(0, miss ? pool("The imported list is in use: %d mods, %d of them not on this PC yet.", g_imp_n, miss) : pool("The imported list is in use: %d mods.", g_imp_n));
    g_toast_undo = 1;
    subscribe_missing(g_imp, g_imp_n);   // the missing ones from the Workshop: Steam downloads them, each is ticked when it arrives
}

typedef BOOL (WINAPI *FileDlg)(OPENFILENAMEW *);
static int file_dialog(int save, char *out, size_t size) {   // 1 = a file was chosen (UTF-8 path in out)
    static FileDlg open_fn, save_fn;
    static int tried;
    if (!tried) {
        HMODULE d = LoadLibraryA("comdlg32.dll");
        tried = 1;
        if (d) { open_fn = (FileDlg)(void *)GetProcAddress(d, "GetOpenFileNameW"); save_fn = (FileDlg)(void *)GetProcAddress(d, "GetSaveFileNameW"); }
    }
    WCHAR file[MAX_PATH] = L"";
    if (save) wcscpy(file, L"Attila mod list.txt");
    OPENFILENAMEW o = {0};
    o.lStructSize = sizeof o;
    o.hwndOwner = g_win;
    o.lpstrFilter = L"Mod lists (*.txt)\0*.txt\0All files\0*.*\0";
    o.lpstrFile = file;
    o.nMaxFile = MAX_PATH;
    o.lpstrDefExt = L"txt";
    o.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    FileDlg f = save ? save_fn : open_fn;
    if (!f || !f(&o)) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, file, -1, out, (int)size, NULL, NULL) > 0;
}

static void export_list(int how) {   // 0 clipboard, 1 file, 2 the code only
    static ListEntry e[MAX_MODS];
    int n = ticked_entries(e, MAX_MODS);
    if (how == 2) {
        char code[10];
        list_code(e, n, code);
        copy_text(g_win, code);
        ui_toast(0, pool("List code %s copied.", code));
        return;
    }
    char *t = list_format(e, n);
    if (!t) return;
    if (how == 0) { copy_text(g_win, t); ui_toast(0, pool("The list of %d mod%s is copied. Paste it (Ctrl+V) for your friends: they press Import.", n, n == 1 ? "" : "s")); }
    else {
        char path[MAX_PATH * 3];
        if (file_dialog(1, path, sizeof path)) { int ok = write_text_file(path, t); ui_toast(ok ? 0 : 2, ok ? "The list is saved to the file." : "Could not write that file."); }
    }
    free(t);
}

static void host_play(int go);   // host_ui.inc
static void play(void) { host_play(GO_GAME); }

#include "host_ui.inc"
static void set_ui_size(int pct);
static void menu_choose(int i) {
    if (i < 0 || i >= g_menu.n || g_menu.value[i] < -100) return;
    int v = g_menu.value[i], owner = g_menu.owner;
    menu_close();
    switch (owner) {
    case M_PRESET:
        if (v == 1000) name_sheet(0);
        else if (v == 1001) { g_preset[0] = 0; ui_toast(0, "No preset in use. The ticks stay as they are."); }
        else if (v == 1002) host_same_list(!g_same_list);
        else if (v >= 0 && v < g_npresets) switch_preset(g_presets[v]);
        break;
    case M_IMPORT:
        if (v == 0) open_import(clipboard_text(g_win), "Pasted from the clipboard");
        else if (v == 1) { char p[MAX_PATH * 3]; if (file_dialog(0, p, sizeof p)) { const char *b = strrchr(p, '\\'); open_import(read_text_file(p), pool("From %s", b ? b + 1 : p)); } }
        else open_import(read_text_file("used_mods.txt"), "Normal Attila's mod list");   // the working folder is the game's
        break;
    case M_EXPORT: export_list(v); break;
    case M_THEME: if (v >= 1000) set_ui_size(v - 1000); else set_theme(v); break;   // the Settings menu
    case M_LOG: if (v) show_log_file(g_win); else copy_last_log(g_win); break;
    case M_FILES: game_files(v); break;
    case M_SET0: g_set_size = v; break;
    case M_SET1: g_set_sharp = v; break;
    case M_SET2: g_set_shadow = v; break;
    }
    InvalidateRect(g_win, NULL, FALSE);
}
static void open_menu_at(int owner, int id, int arg) {
    const Hit *h = find_hit(id, arg);
    if (g_menu.owner == owner) { menu_close(); return; }
    if (h) menu_open(owner, h->r.left, h->r.top, h->r.right - h->r.left, h->r.bottom - h->r.top);
}

static void activate(int id, int arg) {
    switch (id) {
    case W_PRESET: open_menu_at(M_PRESET, id, arg); break;
    case W_MODE: host_switch_mode(arg); break;
    case W_LAUNCHER:   // 64-bit: its own window, this start only; 32-bit: CA's launcher, it asks this time or from now on
        if (g_mode) { host_play(GO_OWN_WINDOW); return; }
        ask_open(ASK_CA);
        break;
    case W_DISCORD: open_explorer("\"" DISCORD_URL "\""); break;
    case W_SAVE:
        if (!g_preset[0]) name_sheet(0);
        else if (preset_write(g_preset)) ui_toast(0, pool("Saved to the preset \"%s\".", g_preset));
        else ui_toast(2, "Could not save the preset.");
        break;
    case W_SAVEAS: name_sheet(0); break;
    case W_DELETE: if (g_preset[0]) { g_sheet = SH_CONFIRM; SetFocus(g_win); } break;
    case W_IMPORT: open_menu_at(M_IMPORT, id, arg); break;
    case W_EXPORT: open_menu_at(M_EXPORT, id, arg); break;
    case W_CODE: export_list(2); break;
    case W_NOTE: {
        int k = arg / 8, b = arg % 8;
        if (k == 0 && b == 0) { settings_put("graphics", "dx11"); g_dx11_note = 0; ui_toast(0, "The game uses DirectX 11 again from this Play."); }
        else if ((k == 0 && b == 1) || (k == 1 && b == 0)) copy_last_log(g_win);
        else if (k == 1 && b == 1) show_log_file(g_win);
        else if (k == 1 && b == 7) g_crash_closed = 1;
        else if (k == 2 && b == 0) { static ListEntry e[MAX_MODS]; subscribe_missing(e, ticked_entries(e, MAX_MODS)); }
        else if (k == 2 && b == 1) { g_filter = 3; g_scroll = 0; view_build(); }
        else if (k == 3 && b == 0) do_undo();
        else if (k == 3 && b == 7) { g_toast[0] = 0; KillTimer(g_win, T_TOAST); }
        break;
    }
    case W_SEARCH: SetFocus(g_search); break;
    case W_SEARCH_CLEAR: SetWindowTextW(g_search, L""); SetFocus(g_search); break;
    case W_FILTER: g_filter = arg; g_scroll = 0; view_build(); break;
    case W_CHECK: tick(arg); break;
    case W_ROW_SUB: if (arg >= 0 && arg < g_nmods) subscribe(g_mods[arg].ws); break;
    case W_ROW_PAGE: if (arg >= 0 && arg < g_nmods) open_page(g_mods[arg].ws); break;
    case W_SECT: set_all(arg == 1); break;
    case W_SEL_PAGE: if (g_sel >= 0) open_page(g_mods[g_sel].ws); break;
    case W_SEL_FOLDER:
        if (g_sel >= 0) {
            char p[MAX_PATH * 3], full[MAX_PATH * 2];
            snprintf(p, sizeof p, "%s/%s", g_mods[g_sel].dir[0] ? g_mods[g_sel].dir : "data", g_mods[g_sel].name);
            if (GetFullPathNameA(p, sizeof full, full, NULL)) { snprintf(p, sizeof p, "/select,\"%s\"", full); open_explorer(p); }
        }
        break;
    case W_SEL_UP: move_sel(-1); break;
    case W_SEL_DOWN: move_sel(1); break;
    case W_SEL_SUB: if (g_sel >= 0) subscribe(g_mods[g_sel].ws); break;
    case W_ADDON_TOG:
        if (arg == 0) g_perf = !g_perf;
        else if (arg == 1) g_flags = !g_flags;
        else g_up = !g_up;
        break;
    case W_ADDON_SUB: subscribe(g_addon_ws[arg]); break;
    case W_ADDON_PAGE: open_page(g_addon_ws[arg]); break;
    case W_SETTING: open_menu_at(M_SET0 + arg, id, arg); break;
    case W_FPS: g_set_fps = !g_set_fps; break;
    case W_THEME: open_menu_at(M_THEME, id, arg); break;
    case W_LOG: open_menu_at(M_LOG, id, arg); break;
    case W_FILES: open_menu_at(M_FILES, id, arg); break;
    case W_QUIT: DestroyWindow(g_win); return;
    case W_PLAY: play(); return;
    case W_MENU: if (arg < 0) menu_close(); else menu_choose(arg); break;
    case W_SHEET:
        if (arg < 0) {   // a click beside the sheet does nothing, except on the welcome note (it closes it)
            if (g_sheet == SH_ASK && g_ask == ASK_WELCOME) { g_sheet = SH_NONE; SetFocus(g_win); }
            break;
        }
        if (g_sheet == SH_IMPORT) {
            if (arg == 0) import_use();
            else if (arg == 1) name_sheet(1);
            else if (arg == 2) g_sheet = SH_NONE;
            else subscribe_missing(g_imp, g_imp_n);
        } else if (g_sheet == SH_NAME) {
            if (arg == 0) name_save();
            else { g_sheet = SH_NONE; SetFocus(g_win); }
        } else if (g_sheet == SH_CONFIRM) {
            if (arg == 0) {
                char path[MAX_PATH];
                preset_path(g_preset, path, sizeof path);
                if (DeleteFileA(path)) { ui_toast(0, pool("The preset \"%s\" is deleted. The ticks stay as they are.", g_preset)); g_preset[0] = 0; presets_scan(); }
                else ui_toast(2, "Could not delete the preset.");
            }
            g_sheet = SH_NONE;
        } else if (g_sheet == SH_ASK) { ask_answer(g_ask, arg); return; }   // may close the window (CA's launcher)
        break;
    case W_SHEET_SUB:
        if (arg >= 0 && arg < g_imp_n) { if (ws_view(g_imp[arg].ws).known) subscribe(g_imp[arg].ws); else open_page(g_imp[arg].ws); }
        break;
    case W_EMPTY:
        if (arg == 0) open_explorer("steam://url/SteamWorkshopPage/325610");
        else { g_filter = 0; SetWindowTextW(g_search, L""); }
        break;
    }
    InvalidateRect(g_win, NULL, FALSE);
}

// ---- keyboard
static int view_index_of(int mod) { for (int i = 0; i < g_nview; i++) if (g_view[i].kind == 2 && g_view[i].mod == mod) return i; return -1; }
static void list_key(int vk) {   // the list has the focus
    int first = -1, last = -1;
    for (int i = 0; i < g_nview; i++) if (g_view[i].kind == 2) { if (first < 0) first = i; last = i; }
    if (first < 0) return;
    int vi = view_index_of(g_sel), page = (g_rows_r.bottom - g_rows_r.top) / S(48), target;
    if (vk == VK_HOME) target = first;
    else if (vk == VK_END) target = last;
    else if (vi < 0) target = first;   // nothing selected yet: any key starts at the top
    else {
        int step = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : vk == VK_PRIOR ? -(page > 1 ? page : 1) : (page > 1 ? page : 1), dir = step > 0 ? 1 : -1;
        target = vi + step;
        if (target < first) target = first;
        if (target > last) target = last;
        while (g_view[target].kind != 2) target += dir;   // a section header: the next mod that way (first..last are mods)
    }
    select_mod(g_view[target].mod, 1);
}
static void focus_move(int back) {
    int chain[400], args[400], n = 0, from = (g_menu.owner || g_sheet) ? g_overlay_from : 0;
    for (int i = from; i < g_nhits && n < 400; i++) if (g_hits[i].focus) { chain[n] = g_hits[i].id; args[n++] = g_hits[i].arg; }
    if (!n) return;
    int cur = -1;
    int fid = GetFocus() == g_search ? W_SEARCH : g_focus_id, farg = GetFocus() == g_search ? 0 : g_focus_arg;
    for (int i = 0; i < n; i++) if (chain[i] == fid && args[i] == farg) cur = i;
    int next = cur < 0 ? (back ? n - 1 : 0) : (cur + (back ? n - 1 : 1)) % n;
    g_focus_id = chain[next];
    g_focus_arg = args[next];
    g_kbd = 1;
    if (g_focus_id == W_SEARCH) SetFocus(g_search);
    else SetFocus(g_win);
    InvalidateRect(g_win, NULL, FALSE);
}
static void focus_list(void) {
    g_focus_id = W_LIST; g_focus_arg = 0;
    SetFocus(g_win);
    if (g_sel < 0 || view_index_of(g_sel) < 0) list_key(VK_HOME);
    InvalidateRect(g_win, NULL, FALSE);
}

static int g_fake_mods;   // tests: 1 ctrl, 2 shift, 4 alt held
static int key_down(int vk) {   // 1 = used
    int ctrl = GetKeyState(VK_CONTROL) < 0 || (g_fake_mods & 1), shift = GetKeyState(VK_SHIFT) < 0 || (g_fake_mods & 2), alt = GetKeyState(VK_MENU) < 0 || (g_fake_mods & 4);
    if (g_menu.owner) {
        int n = g_menu.n, h = g_menu.hover;
        if (vk == VK_ESCAPE) { menu_close(); return 1; }
        if (vk == VK_UP || vk == VK_DOWN) {
            for (int k = 0; k < n; k++) { h = (h + (vk == VK_DOWN ? 1 : n - 1) + (h < 0 && vk == VK_UP ? 1 : 0)) % n; if (g_menu.value[h] >= -100) break; }
            g_menu.hover = h;
            InvalidateRect(g_win, NULL, FALSE);
            return 1;
        }
        if (vk == VK_RETURN || vk == VK_SPACE) { menu_choose(g_menu.hover); return 1; }
        return 1;
    }
    if (g_sheet) {
        if (vk == VK_ESCAPE) { g_sheet = SH_NONE; SetFocus(g_win); InvalidateRect(g_win, NULL, FALSE); return 1; }
        if (vk == VK_TAB) { focus_move(shift); return 1; }
        if (vk == VK_RETURN || vk == VK_SPACE) {
            if (g_kbd && g_focus_id == W_SHEET) activate(W_SHEET, g_focus_arg);
            else if (g_kbd && g_focus_id == W_SHEET_SUB) activate(W_SHEET_SUB, g_focus_arg);
            else if (vk == VK_RETURN) activate(W_SHEET, 0);
            return 1;
        }
        if (g_sheet == SH_IMPORT && (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT)) {
            g_sheet_scroll += (vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : vk == VK_PRIOR ? -6 : 6) * S(44);
            InvalidateRect(g_win, NULL, FALSE);
            return 1;
        }
        return 1;
    }
    if (ctrl && vk == 'F') { SetFocus(g_search); SendMessageW(g_search, EM_SETSEL, 0, -1); return 1; }
    if (ctrl && vk == 'C') { export_list(0); return 1; }
    if (ctrl && vk == 'V') { open_import(clipboard_text(g_win), "Pasted from the clipboard"); return 1; }
    if (ctrl && vk == 'S') { activate(shift ? W_SAVEAS : W_SAVE, 0); return 1; }
    if (ctrl && vk == 'Z') { do_undo(); return 1; }
    if (vk == VK_F5) { ui_toast(1, "Close and open the window to look for new mods."); return 1; }
    if (vk == VK_TAB) { focus_move(shift); return 1; }
    if (vk == VK_ESCAPE) { if (g_search_text[0]) SetWindowTextW(g_search, L""); g_kbd = 0; InvalidateRect(g_win, NULL, FALSE); return 1; }
    if (alt && (vk == VK_UP || vk == VK_DOWN)) { move_sel(vk == VK_UP ? -1 : 1); return 1; }
    if (g_focus_id == W_LIST || !g_kbd) {
        if (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END) { g_focus_id = W_LIST; list_key(vk); return 1; }
        if (vk == VK_SPACE && g_sel >= 0) { tick(g_sel); ensure_visible(g_sel); return 1; }
        if (vk == VK_RETURN) { play(); return 1; }
    }
    if (g_kbd && (vk == VK_RETURN || vk == VK_SPACE)) {
        if (g_focus_id == W_ROW || g_focus_id == W_LIST) { if (vk == VK_RETURN) play(); return 1; }
        activate(g_focus_id, g_focus_arg);
        return 1;
    }
    return 0;
}

static LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        int shift = GetKeyState(VK_SHIFT) < 0;
        if (h == g_name_edit) {
            if (wp == VK_RETURN) { name_save(); return 0; }
            if (wp == VK_ESCAPE) { g_sheet = SH_NONE; SetFocus(g_win); InvalidateRect(g_win, NULL, FALSE); return 0; }
            if (wp == VK_TAB) { g_focus_id = W_SHEET; g_focus_arg = 0; g_kbd = 1; SetFocus(g_win); InvalidateRect(g_win, NULL, FALSE); return 0; }
        } else {
            if (wp == VK_ESCAPE) { if (GetWindowTextLengthW(h)) SetWindowTextW(h, L""); else focus_list(); return 0; }
            if (wp == VK_DOWN || wp == VK_RETURN) { g_kbd = 1; focus_list(); if (wp == VK_RETURN && g_view_h) {} return 0; }
            if (wp == VK_TAB) { g_focus_id = W_SEARCH; g_focus_arg = 0; focus_move(shift); return 0; }
        }
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) return 0;   // no beep
    if (msg == WM_PAINT && h == g_search && !GetWindowTextLengthW(h)) {   // the grey hint while it is empty
        LRESULT r = CallWindowProcW(g_edit_old, h, msg, wp, lp);
        HDC dc = GetDC(h);
        RECT rc;
        GetClientRect(h, &rc);
        HFONT o = (HFONT)SelectObject(dc, g_font[F_BODY]);
        SetTextColor(dc, cref(PAL->faint));
        SetBkMode(dc, TRANSPARENT);
        rc.left += 2;
        int n;
        WCHAR *t = wtext(g_nmods ? pool("Search %d mods by name or Workshop number", g_nmods) : "Search", &n);
        DrawTextW(dc, t, n, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, o);
        ReleaseDC(h, dc);
        return r;
    }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) InvalidateRect(g_win, &g_search_r, FALSE);
    return CallWindowProcW(g_edit_old, h, msg, wp, lp);
}

// ---- results from the background threads
static void thumb_dropped(const char *name, int size) {   // io_thumb's queue was full: this ask is gone, ask again later
    if (size == g_big.size && !_stricmp(name, g_big.name) && g_big.state == 1) g_big.name[0] = 0;
    Mod *m = find_mod(name);
    if (m && m->thumb_state == 1) m->thumb_state = 0;
}
static void on_thumb(ThumbDone *d) {
    if (d->size == g_big.size && !_stricmp(d->name, g_big.name)) {
        if (g_big.img) gp.dispose(g_big.img);
        free(g_big.px);
        g_big.img = NULL;
        g_big.px = d->px;
        g_big.state = d->px ? 2 : 3;
        if (d->px && gp.bitmap_scan0(d->size, d->size, d->size * 4, 0xE200B, d->px, &g_big.img)) g_big.img = NULL;
    } else {
        Mod *m = find_mod(d->name);
        if (m && m->thumb_state == 1) {
            free_thumb(m);
            m->thumb_px = d->px;
            m->thumb_size = d->size;
            m->thumb_state = d->px ? 2 : 3;
            if (d->px && gp.bitmap_scan0(d->size, d->size, d->size * 4, 0xE200B, d->px, &m->thumb)) m->thumb = NULL;
            if (!m->thumb && d->px) m->thumb_state = 3;
        } else free(d->px);
    }
    free(d);
    InvalidateRect(g_win, NULL, FALSE);
}
static void on_clash(ClashDone *d) {
    if (d->gen == g_clash_gen) {
        for (int i = 0; i < g_nmods; i++) g_mods[i].under_n = g_mods[i].over_n = 0, g_mods[i].under_by = g_mods[i].over_of = -1;
        for (int k = 0; k < d->k; k++) {
            Mod *m = find_mod(d->names[k]);
            if (!m) continue;
            m->under_n = d->c[k].under_n;
            m->over_n = d->c[k].over_n;
            Mod *a = d->c[k].under_by >= 0 ? find_mod(d->names[d->c[k].under_by]) : NULL, *b = d->c[k].over_of >= 0 ? find_mod(d->names[d->c[k].over_of]) : NULL;
            m->under_by = a ? (int)(a - g_mods) : -1;
            m->over_of = b ? (int)(b - g_mods) : -1;
        }
        InvalidateRect(g_win, NULL, FALSE);
    }
    free(d->names); free(d->c); free(d);
}
static void on_steam(int no_steam, int code) {
    if (no_steam && g_inst64 == 1) g_inst64 = 0;   // 64-bit Attila's subscribe did not go through (the toast below says why)
    if (no_steam == 2) {   // Steam refused or dropped a subscribe
        if (code == 3)
            ui_toast(2, "Steam is offline (not connected to Steam's servers), so it cannot subscribe. In Steam: Steam menu > Go Online, or restart Steam; then press Subscribe again.");
        else {
            char m[200];
            snprintf(m, sizeof m, "Steam did not subscribe (its answer: error %d). The Workshop page opens in Steam instead.", code);
            ui_toast(2, m);
            if (g_last_asked) open_page(g_last_asked);
        }
        InvalidateRect(g_win, NULL, FALSE);
        return;
    }
    if (no_steam) {
        ui_toast(2, "Steam is not running, so nothing could be subscribed. Its Workshop page opens in Steam instead; or start Steam and press Subscribe again.");
        if (g_last_asked) open_page(g_last_asked);
    }
    int any = 0;
    for (int i = 0; i < g_nmods; i++) {   // finished downloads: the pack joins the list in its place
        if (!g_mods[i].missing || !g_mods[i].ws) continue;
        WsView v = ws_view(g_mods[i].ws);
        if (v.installed && !v.busy) { rescan_item(g_mods[i].ws); any = 1; }
    }
    for (int a = 0; a < N_ADDONS; a++) {
        WsView v = ws_view(g_addon_ws[a]);
        if (v.installed && !v.busy && !g_addon_disk[a]) { g_addon_disk[a] = addon_on_disk(a); if (a == AD_PERF) { g_io.want_version = 1; SetEvent(g_io.wake); } }
    }
    if (any) changed();
    inst64_check();   // 64-bit Attila downloaded: its installer runs
    InvalidateRect(g_win, NULL, FALSE);
}

// ---- mouse
static int g_scroll_drag = -1;   // the scroll bar's thumb is held: mouse y where it was taken
static int g_scroll_from;
static void drag_to(int y) {
    int t = ticked_count();
    int first = view_index_of(0);
    if (g_drag < 0 || first < 0 || t < 1) return;
    int rel = y - g_rows_r.top + g_scroll - g_view[first].y;
    int target = rel < 0 ? 0 : rel / S(48);
    if (target >= t) target = t - 1;
    if (g_mods[target].missing && target != g_drag) {}   // missing rows move like the others
    if (target != g_drag) { move_row(g_drag, target); g_drag = g_sel = target; view_build(); InvalidateRect(g_win, NULL, FALSE); }
}
static void mouse_move(int x, int y, int buttons) {
    g_mouse.x = x; g_mouse.y = y;
    if (!g_tracking) { TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, g_win, 0}; TrackMouseEvent(&t); g_tracking = 1; }
    if (g_scroll_drag >= 0) {
        int area = g_rows_r.bottom - g_rows_r.top, track = area - S(12), th = track * area / (g_view_h ? g_view_h : 1);
        if (th < S(30)) th = S(30);
        if (track > th) g_scroll = g_scroll_from + (y - g_scroll_drag) * (g_view_h - area) / (track - th);
        clamp_scroll();
        InvalidateRect(g_win, NULL, FALSE);
        return;
    }
    if (g_drag_armed && (buttons & MK_LBUTTON) && abs(y - g_drag_y0) > S(5) && reorder_ok() && g_drag_armed - 1 >= 0 && g_drag_armed - 1 < g_nmods && !g_mods[g_drag_armed - 1].missing) {
        int i = g_drag_armed - 1;
        g_drag_armed = 0;
        if (!g_mods[i].on) i = set_tick(i, 1);   // dragging a mod up from the others puts it in the load order
        g_drag = g_sel = i;
        view_build();
        SetTimer(g_win, T_DRAG, 40, NULL);
        SetCursor(LoadCursor(NULL, IDC_SIZENS));
    }
    if (g_drag >= 0) { SetCursor(LoadCursor(NULL, IDC_SIZENS)); drag_to(y); return; }
    const Hit *h = hit_test(g_mouse);
    int id = h ? h->id : W_NONE, arg = h ? h->arg : 0;
    const char *tip = NULL;
    for (int i = g_nhits - 1; i >= 0; i--) if (PtInRect(&g_hits[i].r, g_mouse)) { tip = g_hits[i].tip; if (tip || g_hits[i].id != W_TIP) break; }
    static const char *last_tip;
    if (id != g_hover_id || arg != g_hover_arg || tip != last_tip) {
        g_hover_id = id; g_hover_arg = arg;
        last_tip = tip;
        g_tip_ready = 0;
        if (tip) SetTimer(g_win, T_TIP, 550, NULL);
        if (g_menu.owner && id == W_MENU && arg >= 0) g_menu.hover = arg;
        InvalidateRect(g_win, NULL, FALSE);
    }
}
static void mouse_down(int x, int y, int dbl) {
    POINT p = {x, y};
    const Hit *h = hit_test(p);
    g_tip_ready = 0;
    if (GetFocus() != g_win && !(h && h->id == W_SEARCH)) SetFocus(g_win);
    g_kbd = 0;
    if (!h) return;
    if (g_menu.owner && h->id != W_MENU) { menu_close(); }
    g_press_id = h->id; g_press_arg = h->arg;
    SetCapture(g_win);
    if (h->id == W_ROW) {
        select_mod(h->arg, 0);
        g_focus_id = W_LIST;
        if (dbl) { tick(h->arg); g_press_id = W_NONE; }
        else { g_drag_armed = h->arg + 1; g_drag_y0 = y; }
    } else if (h->id == W_CHECK) select_mod(h->arg, 0);
    else if (h->id == W_SCROLL) {
        int area = g_rows_r.bottom - g_rows_r.top, track = area - S(12), th = track * area / (g_view_h ? g_view_h : 1);
        if (th < S(30)) th = S(30);
        int ty = g_rows_r.top + S(6) + (g_view_h > area ? (track - th) * g_scroll / (g_view_h - area) : 0);
        if (y < ty) g_scroll -= area;
        else if (y > ty + th) g_scroll += area;
        else { g_scroll_drag = y; g_scroll_from = g_scroll; }
        clamp_scroll();
    } else if (h->id == W_LIST) g_focus_id = W_LIST;
    InvalidateRect(g_win, NULL, FALSE);
}
static void mouse_up(int x, int y) {
    POINT p = {x, y};
    ReleaseCapture();
    g_scroll_drag = -1;
    g_drag_armed = 0;
    if (g_drag >= 0) { g_drag = -1; KillTimer(g_win, T_DRAG); changed(); g_press_id = W_NONE; return; }
    const Hit *h = hit_test(p);
    int id = g_press_id, arg = g_press_arg;
    g_press_id = W_NONE;
    if (h && h->id == id && h->arg == arg && id != W_ROW && id != W_LIST && id != W_SCROLL && id != W_TIP) activate(id, arg);
    InvalidateRect(g_win, NULL, FALSE);
}

// ---- the window
static void make_back_buffer(void) {
    if (g_bb) { SelectObject(g_bb, g_bb_old); DeleteObject(g_bb_bmp); DeleteDC(g_bb); g_bb = NULL; }
    if (g_w <= 0 || g_h <= 0) return;
    HDC dc = GetDC(g_win);
    g_bb = CreateCompatibleDC(dc);
    g_bb_bmp = CreateCompatibleBitmap(dc, g_w, g_h);
    g_bb_old = (HBITMAP)SelectObject(g_bb, g_bb_bmp);
    ReleaseDC(g_win, dc);
}
// The biggest scale whose whole layout (940 x 640 at 100%, plus the frame) fits on this monitor.
static float ui_fit(int dpi, HMONITOR mon) {
    MONITORINFO mi = {sizeof mi};
    if (!mon || !GetMonitorInfoA(mon, &mi)) return 100.0f;
    float fw = (mi.rcWork.right - mi.rcWork.left - 16 * dpi / 96.0f) / 940.0f, fh = (mi.rcWork.bottom - mi.rcWork.top - 40 * dpi / 96.0f) / 640.0f;
    return fw < fh ? fw : fh;
}
static float ui_scale(int dpi, HMONITOR mon) {   // Windows' scaling x the interface size, never bigger than the screen
    if (g_force_scale > 0) return g_force_scale;
    float s = dpi / 96.0f * g_ui_pct / 100.0f, fit = ui_fit(dpi, mon), win = dpi / 96.0f;
    if (s > fit) s = fit > win ? fit : win;   // a smaller screen than when it was chosen: as big as fits (not below Windows' own)
    return s;
}
static void set_scale_from_dpi(int dpi) {
    g_dpi = dpi ? dpi : 96;
    g_scale = ui_scale(g_dpi, g_win ? MonitorFromWindow(g_win, MONITOR_DEFAULTTONEAREST) : NULL);
    fonts_make();
    if (g_search) SendMessageW(g_search, WM_SETFONT, (WPARAM)g_font[F_BODY], TRUE);
    if (g_name_edit) SendMessageW(g_name_edit, WM_SETFONT, (WPARAM)g_font[F_BODY], TRUE);
    view_build();
}
static void adjust_rect(RECT *r) {   // client size -> window size, with the frame of this monitor's scaling
    static BOOL (WINAPI *adjust)(RECT *, DWORD, BOOL, DWORD, UINT);
    static int tried;
    if (!tried) { tried = 1; *(FARPROC *)&adjust = GetProcAddress(GetModuleHandleA("user32.dll"), "AdjustWindowRectExForDpi"); }
    if (adjust) adjust(r, WS_OVERLAPPEDWINDOW, FALSE, 0, (UINT)g_dpi);
    else AdjustWindowRectEx(r, WS_OVERLAPPEDWINDOW, FALSE, 0);
}
static void min_window_size(SIZE *out) {   // never smaller than this layout needs
    RECT r = {0, 0, S(940), S(640)};
    adjust_rect(&r);
    out->cx = r.right - r.left;
    out->cy = r.bottom - r.top;
}
static void set_ui_size(int pct) {   // Settings > Interface size: everything bigger or smaller, the window with it (kept on the screen)
    RECT c;
    GetClientRect(g_win, &c);
    float dw = c.right / g_scale, dh = c.bottom / g_scale;
    char v[8];
    g_ui_pct = pct;
    snprintf(v, sizeof v, "%d", pct);
    settings_put("ui_size", v);
    set_scale_from_dpi(g_dpi);
    if (IsZoomed(g_win)) { layout(); clamp_scroll(); InvalidateRect(g_win, NULL, FALSE); return; }
    MONITORINFO mi = {sizeof mi};
    RECT w = {0, 0, S(dw), S(dh)}, now;
    adjust_rect(&w);
    GetWindowRect(g_win, &now);
    GetMonitorInfoA(MonitorFromWindow(g_win, MONITOR_DEFAULTTONEAREST), &mi);
    int ww = w.right - w.left, wh = w.bottom - w.top, aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
    if (ww > aw) ww = aw;
    if (wh > ah) wh = ah;
    int x = (now.left + now.right - ww) / 2, y = (now.top + now.bottom - wh) / 2;   // the same middle
    if (x + ww > mi.rcWork.right) x = mi.rcWork.right - ww;
    if (y + wh > mi.rcWork.bottom) y = mi.rcWork.bottom - wh;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    SetWindowPos(g_win, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_win, NULL, FALSE);
}

static void test_step(void);   // picker_test.inc

static LRESULT CALLBACK picker_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    static int scripted = -1;   // ATTILA_PICKER_TEST: the script moves the mouse, the person's own mouse is left out
    if (scripted < 0) scripted = getenv("ATTILA_PICKER_TEST") != NULL;
    if (scripted && ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSELEAVE)) return 0;
    g_step_msg = msg;   // for the crash line
    switch (msg) {
    case WM_CREATE: {
        g_win = w;
        g_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, w, (HMENU)1, GetModuleHandleA(NULL), NULL);
        g_name_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, w, (HMENU)2, GetModuleHandleA(NULL), NULL);
        g_edit_old = (WNDPROC)SetWindowLongPtrW(g_search, GWLP_WNDPROC, (LONG_PTR)edit_proc);
        SetWindowLongPtrW(g_name_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
        SendMessageW(g_search, EM_SETLIMITTEXT, 60, 0);
        SendMessageW(g_name_edit, EM_SETLIMITTEXT, 60, 0);
        SendMessageW(g_search, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SendMessageW(g_name_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        set_scale_from_dpi(g_dpi);
        set_theme(g_theme);
        g_sel = g_nmods ? 0 : -1;
        work_start();
        changed();
        if (getenv("ATTILA_PICKER_AUTO")) PostMessageA(w, WM_P_AUTO, 0, 0);
        if (getenv("ATTILA_PICKER_TEST")) SetTimer(w, T_SCRIPT, 300, NULL);
        return 0;
    }
    case WM_P_AUTO:   // ATTILA_PICKER_AUTO=1: untick the first, move the last to the top, Play; =2: Play with the list as it is
        if (g_nmods > 1 && strcmp(getenv("ATTILA_PICKER_AUTO") ? getenv("ATTILA_PICKER_AUTO") : "", "2")) {
            if (g_mods[0].on) set_tick(0, 0);
            int i = set_tick(g_nmods - 1, 1);
            move_row(i, 0);
        }
        play();
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        paint(dc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) return 0;
        g_w = LOWORD(lp); g_h = HIWORD(lp);
        make_back_buffer();
        layout();
        clamp_scroll();
        InvalidateRect(w, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        SIZE z;
        min_window_size(&z);
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = z.cx;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = z.cy;
        return 0;
    }
    case 0x02E0: {   // WM_DPICHANGED: another monitor or another scaling
        set_scale_from_dpi(HIWORD(wp));
        const RECT *r = (const RECT *)lp;
        SetWindowPos(w, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        for (int i = 0; i < g_nmods; i++) if (g_mods[i].thumb_state == 3) g_mods[i].thumb_state = 0;
        InvalidateRect(w, NULL, FALSE);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        SetTextColor((HDC)wp, cref(PAL->text));
        SetBkColor((HDC)wp, cref(PAL->field));
        return (LRESULT)g_field_brush;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 1 && HIWORD(wp) == EN_CHANGE) {
            WCHAR t[80];
            GetWindowTextW(g_search, t, 80);
            WideCharToMultiByte(CP_UTF8, 0, t, -1, g_search_text, sizeof g_search_text, NULL, NULL);
            g_scroll = 0;
            view_build();
            InvalidateRect(w, NULL, FALSE);
        } else if (LOWORD(wp) == 2 && HIWORD(wp) == EN_CHANGE && (g_name_err[0] || g_name_overwrite)) {
            g_name_err[0] = 0; g_name_overwrite = 0;
            InvalidateRect(w, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE: mouse_move((short)LOWORD(lp), (short)HIWORD(lp), (int)wp); return 0;
    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hover_id) { g_hover_id = W_NONE; g_tip_ready = 0; InvalidateRect(w, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: mouse_down((short)LOWORD(lp), (short)HIWORD(lp), 0); return 0;
    case WM_LBUTTONDBLCLK: mouse_down((short)LOWORD(lp), (short)HIWORD(lp), 1); return 0;
    case WM_LBUTTONUP: mouse_up((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_CAPTURECHANGED:
        if (g_drag >= 0) { g_drag = -1; KillTimer(w, T_DRAG); changed(); }
        g_scroll_drag = -1;
        return 0;
    case WM_MOUSEWHEEL: {
        int d = -GET_WHEEL_DELTA_WPARAM(wp) * S(48) * 2 / WHEEL_DELTA;
        g_tip_ready = 0;
        if (g_sheet == SH_IMPORT) g_sheet_scroll += d;
        else if (!g_menu.owner && !g_sheet) { g_scroll += d; clamp_scroll(); }
        InvalidateRect(w, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (key_down((int)wp)) { g_tip_ready = 0; return 0; }
        break;
    case WM_SYSCHAR: if (wp == VK_UP || wp == VK_DOWN) return 0; break;
    case WM_CHAR:   // typing in the list starts a search
        if (wp >= 32 && wp != 127 && !g_sheet && !g_menu.owner && GetKeyState(VK_CONTROL) >= 0 && wp != ' ') {
            SetFocus(g_search);
            SendMessageW(g_search, WM_CHAR, wp, lp);
        }
        return 0;
    case WM_TIMER:
        if (wp == T_TOAST) { KillTimer(w, T_TOAST); g_toast[0] = 0; InvalidateRect(w, NULL, FALSE); }
        else if (wp == T_TIP) { KillTimer(w, T_TIP); g_tip_ready = 1; InvalidateRect(w, NULL, FALSE); }
        else if (wp == T_DRAG && g_drag >= 0) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(w, &p);
            if (p.y < g_rows_r.top + S(24)) g_scroll -= S(14);
            else if (p.y > g_rows_r.bottom - S(24)) g_scroll += S(14);
            else return 0;
            clamp_scroll();
            drag_to(p.y);
            InvalidateRect(w, NULL, FALSE);
        } else if (wp == T_SCRIPT) test_step();
        else if (wp == T_QUICK) { KillTimer(w, T_QUICK); crash_test_late(); quick_done(); }   // up for 10 seconds: this start went well
        return 0;
    case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE && g_menu.owner) menu_close(); break;
    case WM_SETFOCUS: InvalidateRect(w, NULL, FALSE); return 0;
    case WM_KILLFOCUS: InvalidateRect(w, NULL, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (g_drag >= 0)) { SetCursor(LoadCursor(NULL, IDC_SIZENS)); return TRUE; }
        if (LOWORD(lp) == HTCLIENT) {
            const Hit *h = hit_test(g_mouse);
            int link = h && h->id != W_LIST && h->id != W_ROW && h->id != W_SHEET && h->id != W_MENU && h->id != W_SEARCH;
            SetCursor(LoadCursor(NULL, h && h->id == W_SEARCH ? IDC_IBEAM : link ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_P_STEAM: on_steam((int)wp, (int)lp); return 0;
    case WM_P_THUMB: on_thumb((ThumbDone *)lp); return 0;
    case WM_P_CLASH: on_clash((ClashDone *)lp); return 0;
    case WM_P_INST64: inst64_done((int)lp); return 0;
    case WM_P_INFO: g_perf_version = (int)lp; InvalidateRect(w, NULL, FALSE); return 0;
    case WM_CLOSE: DestroyWindow(w); return 0;
    case WM_DESTROY: {
        quick_done();   // closed or Play: a normal end
        g_step = "closing the window";
        settings_put("theme", g_theme_keys[g_theme]);
        settings_put("preset", g_preset);
        WINDOWPLACEMENT wpl = {sizeof wpl};
        if (GetWindowPlacement(w, &wpl) && !getenv("ATTILA_PICKER_TEST")) {
            char v[32];
            RECT r = wpl.rcNormalPosition;
            snprintf(v, sizeof v, "%dx%d", (int)((r.right - r.left) / g_scale), (int)((r.bottom - r.top) / g_scale));
            settings_put("window", v);
            settings_put("window_max", wpl.showCmd == SW_SHOWMAXIMIZED ? "1" : "0");
        }
        work_stop();
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(w, msg, wp, lp);
}
