// Portions of this file (the mod-picture lookup (picture_path)) come from Sol Invictus's 64-bit Attila (attila64/loader/picker.c) and are under its MIT license: see SOL_INVICTUS_MIT.txt.
// ---- The start window (redo, 2026-11-25). One window, drawn by itself into a back buffer (no flicker), sized for the
// monitor's scaling. Two real edit boxes (search, a preset's name); everything else is drawn here and hit-tested from the
// rectangles the last drawing registered. Every size below is in 100%-screen units and goes through S().

enum {   // what was clicked; arg tells which one
    W_NONE, W_PRESET, W_SAVE, W_SAVEAS, W_DELETE, W_IMPORT, W_EXPORT, W_CODE, W_NOTE, W_SEARCH, W_SEARCH_CLEAR, W_FILTER,
    W_LIST, W_ROW, W_CHECK, W_ROW_SUB, W_ROW_PAGE, W_SECT, W_SCROLL, W_SEL_PAGE, W_SEL_FOLDER, W_SEL_UP, W_SEL_DOWN, W_SEL_SUB,
    W_ADDON_TOG, W_ADDON_SUB, W_ADDON_PAGE, W_SETTING, W_FPS, W_THEME, W_LOG, W_QUIT, W_PLAY, W_MENU, W_SHEET, W_SHEET_SUB,
    W_SHEET_SCROLL, W_EMPTY, W_TIP, W_NAME_EDIT, W_MODE, W_LAUNCHER, W_DISCORD, W_FILES
};
enum { WM_P_AUTO = WM_APP + 30, WM_P_INST64 };
#define DISCORD_URL "https://discord.gg/cSAJeNUWZ3"   // JGdaCaesar's Discord: bug reports
enum { T_TOAST = 1, T_DRAG, T_TIP, T_SCRIPT, T_STEAM_POLL };

typedef struct { RECT r; int id, arg, focus; const char *tip; } Hit;
static Hit g_hits[1200];
static int g_nhits, g_overlay_from;   // hits from g_overlay_from on belong to the open menu or sheet
static char g_pool[65536];            // tooltip texts of this drawing
static int g_pool_at;
static const char *pool(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int room = (int)sizeof g_pool - g_pool_at;
    int n = room > 1 ? vsnprintf(g_pool + g_pool_at, (size_t)room, fmt, ap) : -1;
    va_end(ap);
    if (n < 0 || n >= room) return "";
    const char *s = g_pool + g_pool_at;
    g_pool_at += n + 1;
    return s;
}

static int g_w, g_h;                    // client size, pixels
static int g_dpi = 96;
static float g_force_scale;             // ATTILA_PICKER_SCALE (tests)
static HDC g_bb;                        // back buffer
static HBITMAP g_bb_bmp, g_bb_old;
static HDC g_art;                       // the theme's picture at the window's size
static HBITMAP g_art_bmp, g_art_old;
static int g_art_w, g_art_h, g_art_theme = -1;
static void *g_art_img;                 // the theme's picture file (GDI+)
static int g_art_img_theme = -1;
static HWND g_search, g_name_edit;
static WNDPROC g_edit_old;
static HBRUSH g_field_brush;
static unsigned g_field_rgb = 0xFFFFFFFF;
static char g_search_text[128];
static int g_filter;                    // 0 all, 1 ticked, 2 not ticked, 3 missing
static int g_sel = -1;                  // selected row of g_mods
static int g_scroll;                    // list scroll, pixels
static int g_hover_id, g_hover_arg, g_press_id, g_press_arg, g_tracking;
static int g_focus_id = W_LIST, g_focus_arg, g_kbd;
static int g_set_size, g_set_sharp, g_set_shadow, g_set_fps, g_perf, g_flags, g_up;
static int g_dx11_note, g_crashed, g_crash_closed;
static int g_addon_disk[N_ADDONS];
static char g_toast[300];
static int g_toast_kind, g_toast_undo;
static int g_drag = -1, g_drag_y0, g_drag_armed, g_drag_x0;
static POINT g_mouse;
static int g_tip_ready;
static RECT g_rows_r, g_list_r, g_side_r, g_search_r, g_name_r, g_sheet_rows_r;
static int g_sheet_scroll;
static unsigned long long g_last_asked;

// big picture of the selected mod
static struct { char name[160]; void *img; BYTE *px; int size, state; } g_big;

// view: the rows the list shows now
typedef struct { int kind, mod, y, h; } ViewRow;   // kind 0 = load order header, 1 = not ticked header, 2 = a mod
static ViewRow g_view[MAX_MODS + 4];
static int g_nview, g_view_h, g_nmatch_t, g_nmatch_r;

// menus
enum { M_PRESET = 1, M_IMPORT, M_EXPORT, M_THEME, M_LOG, M_SET0, M_SET1, M_SET2, M_FILES };
static struct { int owner, n, hover, check; RECT anchor; char text[MAX_PRESETS + 8][96]; int value[MAX_PRESETS + 8], line[MAX_PRESETS + 8]; } g_menu;

// sheets
enum { SH_NONE, SH_IMPORT, SH_NAME, SH_CONFIRM, SH_ASK };
static int g_sheet;
// SH_ASK: CA's launcher this time or from now on / back from CA's launcher (heapfix.ini launcher=2) / 64-bit not installed
enum { ASK_CA, ASK_BACK, ASK_64, ASK_WELCOME };   // ASK_WELCOME: the first start (OK, Esc or a click beside it closes it)
static int g_ask;
static void ask_open(int kind) { g_ask = kind; g_sheet = SH_ASK; g_focus_id = 0; if (g_win) { SetFocus(g_win); InvalidateRect(g_win, NULL, FALSE); } }
static ListEntry g_imp[MAX_MODS];
static int g_imp_n;
static char g_imp_code[10], g_imp_from[80];
static int g_name_for;                  // 0 = save the ticked list, 1 = save the import as a preset
static char g_name_err[200];
static int g_name_overwrite;

static int reorder_ok(void) { return !g_search_text[0] && (g_filter == 0 || g_filter == 1); }
static int missing_count(void) { int n = 0; for (int i = 0; i < g_nmods && g_mods[i].on; i++) n += g_mods[i].missing; return n; }

static void ui_toast(int kind, const char *msg) {   // kind: 0 done, 1 info, 2 warning; shown for 8 seconds
    snprintf(g_toast, sizeof g_toast, "%s", msg);
    g_toast_kind = kind;
    g_toast_undo = 0;
    if (g_win) { SetTimer(g_win, T_TOAST, 8000, NULL); InvalidateRect(g_win, NULL, FALSE); }
}

static void copy_text(HWND w, const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    HGLOBAL mem = n > 0 ? GlobalAlloc(GMEM_MOVEABLE, (size_t)n * 2) : NULL;
    int ok = 0;
    if (mem) {
        WCHAR *p = GlobalLock(mem);
        if (p) { MultiByteToWideChar(CP_UTF8, 0, utf8, -1, p, n); GlobalUnlock(mem); }
        if (p && OpenClipboard(w)) {
            EmptyClipboard();
            ok = SetClipboardData(CF_UNICODETEXT, mem) != NULL;   // the clipboard owns the memory once this succeeds
            CloseClipboard();
        }
        if (!ok) GlobalFree(mem);
    }
    if (!ok) ui_toast(2, "Could not use the clipboard. Try again.");
}
static char *clipboard_text(HWND w) {   // UTF-8, the caller frees it; NULL = no text
    char *out = NULL;
    if (!OpenClipboard(w)) return NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *p = h ? GlobalLock(h) : NULL;
    if (p) {
        int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, NULL, 0, NULL, NULL);
        if (n > 0 && n < (4 << 20) && (out = malloc((size_t)n))) WideCharToMultiByte(CP_UTF8, 0, p, -1, out, n, NULL, NULL);
        GlobalUnlock(h);
    }
    CloseClipboard();
    return out;
}

static void open_page(unsigned long long ws) {   // the item's Workshop page in the Steam client
    char a[96];
    snprintf(a, sizeof a, "steam://url/CommunityFilePage/%llu", ws);
    open_explorer(a);
}

static void fmt_size(unsigned long long b, char *out, size_t n) {
    if (b >= 1000000000ULL) snprintf(out, n, "%.1f GB", b / 1e9);
    else if (b >= 10000000ULL) snprintf(out, n, "%.0f MB", b / 1e6);
    else if (b >= 1000000ULL) snprintf(out, n, "%.1f MB", b / 1e6);
    else snprintf(out, n, "%.0f KB", b / 1e3 < 1 ? 1 : b / 1e3);
}
static void fmt_date(FILETIME t, char *out, size_t n) {
    static const char *const mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    FILETIME l;
    SYSTEMTIME s;
    if (!FileTimeToLocalFileTime(&t, &l) || !FileTimeToSystemTime(&l, &s) || s.wMonth < 1 || s.wMonth > 12) { out[0] = 0; return; }
    snprintf(out, n, "%u %s %u", s.wDay, mon[s.wMonth - 1], s.wYear);
}

// The picture next to a mod's pack (<name>.png / .jpg / .jpeg), or the first picture in its Workshop folder.
static int picture_path(const Mod *m, char *out, size_t size) {
    static const char *const exts[] = {".png", ".jpg", ".jpeg"};
    if (m->missing) return 0;
    const char *dir = m->dir[0] ? m->dir : "data";
    char base[160];
    snprintf(base, sizeof base, "%s", m->name);
    char *dot = strrchr(base, '.');
    if (dot && !_stricmp(dot, ".pack")) *dot = 0;
    for (int i = 0; i < 3; i++) {
        snprintf(out, size, "%s/%s%s", dir, base, exts[i]);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
    }
    if (!m->ws) return 0;   // only a Workshop item's own folder holds just that mod's files
    for (int i = 0; i < 3; i++) {
        char pat[MAX_PATH * 2];
        snprintf(pat, sizeof pat, "%s/*%s", dir, exts[i]);
        WIN32_FIND_DATAA f;
        HANDLE h = FindFirstFileA(pat, &f);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        snprintf(out, size, "%s/%s", dir, f.cFileName);
        return 1;
    }
    return 0;
}
static void want_thumb(Mod *m, int size) {   // asks the file thread for the picture; it arrives as WM_P_THUMB
    char p[MAX_PATH * 2] = "";
    if (!picture_path(m, p, sizeof p)) p[0] = 0;
    io_thumb(m->name, p, size);
}

// ---- view
static int matches(const Mod *m) {
    if (!g_search_text[0]) return 1;
    char ws[24];
    snprintf(ws, sizeof ws, "%llu", m->ws);
    for (const char *h = m->name; *h; h++) if (!_strnicmp(h, g_search_text, strlen(g_search_text))) return 1;
    return m->ws && strstr(ws, g_search_text) != NULL;
}
static void view_build(void) {
    int y = 0, t = ticked_count();
    g_nview = 0;
    g_nmatch_t = g_nmatch_r = 0;
    for (int pass = 0; pass < 2 && g_nmods; pass++) {
        if ((pass == 0 && g_filter == 2) || (pass == 1 && (g_filter == 1 || g_filter == 3))) continue;
        int from = pass ? t : 0, to = pass ? g_nmods : t, head = g_nview, any = 0;
        if (from == to && pass == 1) continue;
        g_view[g_nview++] = (ViewRow){pass, -1, y, S(34)};
        y += S(34);
        for (int i = from; i < to; i++) {
            if (!matches(&g_mods[i]) || (g_filter == 3 && !g_mods[i].missing)) continue;
            g_view[g_nview++] = (ViewRow){2, i, y, S(48)};
            y += S(48);
            any++;
        }
        if (pass) g_nmatch_r = any; else g_nmatch_t = any;
        if (!any && (g_search_text[0] || g_filter)) { g_nview = head; y = g_view[head].y; }   // a header with nothing under it goes
        else if (!any && pass == 0) { g_view[g_nview++] = (ViewRow){3, -1, y, S(44)}; y += S(44); }   // "nothing ticked"
    }
    g_view_h = y + S(8);
}
static void ensure_visible(int mod) {
    for (int i = 0; i < g_nview; i++)
        if (g_view[i].kind == 2 && g_view[i].mod == mod) {
            int area = g_rows_r.bottom - g_rows_r.top;
            if (g_view[i].y < g_scroll + S(34) && i > 0 && g_view[i - 1].kind < 2) g_scroll = g_view[i - 1].y;
            else if (g_view[i].y < g_scroll) g_scroll = g_view[i].y;
            else if (g_view[i].y + g_view[i].h > g_scroll + area) g_scroll = g_view[i].y + g_view[i].h - area;
        }
}
static void clamp_scroll(void) {
    int area = g_rows_r.bottom - g_rows_r.top, top = g_view_h - area;
    if (g_scroll > top) g_scroll = top;
    if (g_scroll < 0) g_scroll = 0;
}
static void changed(void) {   // the list changed: rows, hints and drawing
    for (int i = 0; i < g_nmods; i++) g_mods[i].under_n = g_mods[i].over_n = -1;   // old row numbers are wrong now; the new count follows in ms
    view_build();
    clamp_scroll();
    io_clash_request();
    for (int i = 0; i < g_nmods; i++) if (g_mods[i].on) ws_watch(g_mods[i].ws);
    for (int a = 0; a < N_ADDONS; a++) ws_watch(g_addon_ws[a]);   // the add-on card shows Steam's state from the start
    if (!g_have64) ws_watch(SOL_ITEM_ID);   // 64-bit Attila: whether Steam has it already, for its install
    InvalidateRect(g_win, NULL, FALSE);
}

// ---- hits
static void hit(int x, int y, int w, int h, int id, int arg, int focus, const char *tip) {
    if (g_nhits >= (int)(sizeof g_hits / sizeof g_hits[0])) return;
    Hit *t = &g_hits[g_nhits++];
    SetRect(&t->r, x, y, x + w, y + h);
    t->id = id; t->arg = arg; t->focus = focus; t->tip = tip;
}
static int is_hover(int id, int arg) { return g_hover_id == id && g_hover_arg == arg; }
static int is_press(int id, int arg) { return g_press_id == id && g_press_arg == arg && g_hover_id == id && g_hover_arg == arg; }
static int is_focus(int id, int arg) { return g_kbd && g_focus_id == id && g_focus_arg == arg && GetFocus() == g_win; }
static void focus_ring(int x, int y, int w, int h, float r) { stroke_rr((float)x - S(2), (float)y - S(2), (float)w + S(4), (float)h + S(4), r + S(2), argb(PAL->accent, 255), (float)S(2)); }

// ---- controls
enum { B_NORMAL, B_GHOST, B_ACCENT, B_PLAY, B_OFF = 16, B_ICON_RIGHT = 32 };
static int button(int x, int y, int w, int h, const char *label, WCHAR ic, int id, int arg, int style, const char *tip) {
    int off = style & B_OFF, iright = style & B_ICON_RIGHT;
    style &= 15;
    int font = style == B_PLAY ? F_PLAY : h < S(30) ? F_SMALL : F_BODY;
    int tw = label[0] ? text_w(label, font) : 0, iw = ic ? S(style == B_PLAY ? 16 : 14) : 0, gap = ic && label[0] ? S(7) : 0;
    if (w <= 0) w = tw + iw + gap + S(h < S(30) ? 18 : 24);
    int hov = !off && is_hover(id, arg), pr = !off && is_press(id, arg);
    unsigned bg = PAL->field, edge = PAL->line2, fg = off ? PAL->faint : PAL->text;
    int a = 255;
    if (style == B_GHOST) { a = hov ? 255 : 0; bg = hov ? mix(PAL->panel, PAL->text, 18) : PAL->panel; edge = hov ? PAL->line2 : PAL->line; }
    else if (style == B_ACCENT) { bg = mix(PAL->panel, PAL->accent, hov ? 60 : 34); edge = PAL->accent; fg = off ? PAL->faint : PAL->accent_text; }
    else if (style == B_PLAY) { bg = hov ? mix(PAL->play, 0xFFFFFF, 22) : PAL->play; edge = mix(PAL->play, 0, 90); fg = 0xFFFFFF; }
    else if (hov) bg = mix(PAL->field, PAL->text, 14);
    if (pr) bg = mix(bg, PAL->dark ? 0 : 0xFFFFFF, 60);
    float r = (float)S(6);
    if (a) fill_rr((float)x, (float)y, (float)w, (float)h, r, argb(bg, a == 255 ? (style == B_GHOST ? 255 : PAL->panel_alpha < 255 ? 235 : 255) : a));
    if (style != B_GHOST || hov) stroke_rr((float)x, (float)y, (float)w, (float)h, r, argb(edge, 255), style == B_PLAY ? (float)S(1) : (float)S(1));
    if (style == B_PLAY) stroke_rr((float)x - S(3), (float)y - S(3), (float)w + S(6), (float)h + S(6), r + S(3), argb(PAL->accent, hov ? 200 : 140), (float)S(2));
    int cx = x + (w - tw - iw - gap) / 2;
    if (ic) icon(iright ? cx + tw + gap : cx, y, iw, h, ic, style == B_PLAY ? F_ICON_L : F_ICON_S, fg);
    if (label[0]) text(iright ? cx : cx + iw + gap, y + (pr ? 1 : 0), tw + 2, h, label, font, fg, DT_LEFT);
    if (!off) hit(x, y, w, h, id, arg, 1, tip);
    if (!off && is_focus(id, arg)) focus_ring(x, y, w, h, r);
    return w;
}
static int dropdown(int x, int y, int w, int h, const char *label, const char *sub, int id, int arg, const char *tip) {
    int hov = is_hover(id, arg);
    fill_rr((float)x, (float)y, (float)w, (float)h, (float)S(6), argb(hov ? mix(PAL->field, PAL->text, 12) : PAL->field, 255));
    stroke_rr((float)x, (float)y, (float)w, (float)h, (float)S(6), argb(g_menu.owner && g_menu.anchor.left == x && g_menu.anchor.top == y ? PAL->accent : PAL->line2, 255), (float)S(1));
    int lw = text_w(label, h < S(30) ? F_SMALL : F_BOLD);
    int room = w - S(40);
    text(x + S(11), y, room, h, label, h < S(30) ? F_BODY : F_BOLD, PAL->text, DT_LEFT);
    if (sub && sub[0] && lw + S(18) < room) text(x + S(11) + lw + S(8), y, room - lw - S(8), h, sub, F_SMALL, PAL->faint, DT_LEFT);
    icon(x + w - S(28), y, S(20), h, IC_DOWN, F_ICON_S, PAL->dim);
    hit(x, y, w, h, id, arg, 1, tip);
    if (is_focus(id, arg)) focus_ring(x, y, w, h, (float)S(6));
    return w;
}
static void toggle(int x, int y, int on, int off, int id, int arg, const char *tip) {
    int w = S(36), h = S(20);
    unsigned track = on ? PAL->accent : PAL->dark ? 0x4A4C55 : 0xBDB8AF;
    fill_rr((float)x, (float)y, (float)w, (float)h, (float)h / 2, argb(track, off ? 90 : 255));
    float kx = on ? (float)(x + w - h / 2) : (float)(x + h / 2);
    fill_circle(kx, (float)y + h / 2.0f, (float)S(7), argb(on ? (PAL->dark ? 0x1B1407 : 0xFFFFFF) : 0xFFFFFF, off ? 140 : 255));
    if (!off) hit(x - S(4), y - S(6), w + S(8), h + S(12), id, arg, 1, tip);
    if (!off && is_focus(id, arg)) focus_ring(x, y, w, h, (float)h / 2);
}
static int pill(int x_right, int cy, const char *s, int st, const char *tip) {   // right-aligned at x_right; returns its width
    int w = text_w(s, F_SMALL) + S(16), h = S(22), x = x_right - w;
    unsigned c = st == ST_MUTE ? PAL->dim : st_col(st);
    fill_rr((float)x, (float)(cy - h / 2), (float)w, (float)h, (float)h / 2, argb(c, PAL->dark ? 40 : 30));
    text(x, cy - h / 2, w, h, s, F_SMALL, c, DT_CENTER);
    if (tip) hit(x, cy - h / 2, w, h, W_TIP, 0, 0, tip);
    return w;
}
static void panel(int x, int y, int w, int h, float r) {
    fill_rr((float)x, (float)y, (float)w, (float)h, r, argb(PAL->panel, PAL->panel_alpha));
    stroke_rr((float)x, (float)y, (float)w, (float)h, r, argb(PAL->line, 255), (float)S(1));
}
static void bar(int x, int y, int w, int h, double frac, unsigned rgb) {
    fill_rr((float)x, (float)y, (float)w, (float)h, (float)h / 2, argb(PAL->line, 255));
    if (frac > 0) fill_rr((float)x, (float)y, (float)(w * (frac > 1 ? 1 : frac)), (float)h, (float)h / 2, argb(rgb, 255));
}
static void card_head(int x, int y, int w, const char *title, const char *right) {
    text(x, y, w, S(20), title, F_HEAD, PAL->dim, DT_LEFT);
    if (right) text(x, y, w, S(20), right, F_SMALL, PAL->faint, DT_RIGHT);
}

// Steam's word on an item for a row: what to show and whether a Subscribe button belongs there.
typedef struct { int known, subscribed, busy, installed; double frac; char text[64]; int st; } WsView;
static WsView ws_view(unsigned long long ws) {
    WsView v = {0};
    unsigned s = 0;
    unsigned long long d = 0, t = 0;
    if (!ws || !ws_get(ws, &s, &d, &t)) return v;
    v.known = 1;
    v.subscribed = (s & WS_SUBSCRIBED) != 0;
    v.installed = (s & WS_INSTALLED) != 0;
    v.busy = (s & (WS_DOWNLOADING | WS_PENDING)) != 0 || (v.subscribed && !v.installed);
    v.frac = t ? (double)d / t : 0;
    if (s & WS_DOWNLOADING) { snprintf(v.text, sizeof v.text, t ? "Downloading %d%%" : "Downloading", (int)(v.frac * 100)); v.st = ST_INFO; }
    else if (v.busy) { snprintf(v.text, sizeof v.text, "Waiting for Steam"); v.st = ST_INFO; }
    return v;
}

// ---- layout of the frame
static int notes_list(int *kinds) {   // which bands show: 0 OpenGL, 1 crash, 2 missing mods (short messages float: draw_toast)
    int n = 0;
    if (g_dx11_note) kinds[n++] = 0;
    if (g_crashed && !g_crash_closed) kinds[n++] = 1;
    if (missing_count()) kinds[n++] = 2;
    return n;
}
static void layout(void) {
    int kinds[4], nn = notes_list(kinds);
    int top = S(10) + S(34) + S(26) + nn * S(46);   // the top bar and the line under the list code
    int side_w = S(372), gap = S(12), m = S(16);
    int foot = S(68);
    SetRect(&g_side_r, g_w - m - side_w, top, g_w - m, g_h - foot);
    SetRect(&g_list_r, m, top, g_side_r.left - gap, g_h - foot);
    SetRect(&g_search_r, g_list_r.left + S(12), g_list_r.top + S(12), 0, g_list_r.top + S(12) + S(34));
    SetRect(&g_rows_r, g_list_r.left + S(1), g_list_r.top + S(58), g_list_r.right - S(1), g_list_r.bottom - S(1));
}

// ---- drawing
static void draw_background(void) {
    const Pal *p = PAL;
    if (p->art && gp.dll) {
        if (g_art_img_theme != g_theme) {
            if (g_art_img) gp.dispose(g_art_img);
            g_art_img = NULL;
            g_art_img_theme = g_theme;
            char path[MAX_PATH * 2];
            WCHAR w[MAX_PATH * 2];
            GetModuleFileNameA(NULL, path, MAX_PATH);
            char *s = strrchr(path, '\\');
            snprintf(s ? s + 1 : path, MAX_PATH, "themes\\%s", p->art);
            if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH * 2) || gp.load(w, &g_art_img)) g_art_img = NULL;
        }
        if (g_art_img && (g_art_theme != g_theme || g_art_w != g_w || g_art_h != g_h)) {   // the picture at this size, made once
            if (g_art) { SelectObject(g_art, g_art_old); DeleteObject(g_art_bmp); DeleteDC(g_art); }
            g_art = CreateCompatibleDC(g_bb);
            g_art_bmp = CreateCompatibleBitmap(g_bb, g_w, g_h);
            g_art_old = (HBITMAP)SelectObject(g_art, g_art_bmp);
            HDC keep_dc = g_dc;
            void *keep_g = g_g, *g = NULL;
            g_dc = g_art;
            if (!gp.from_hdc(g_art, &g)) {
                g_g = g;
                fill_rect(0, 0, g_w, g_h, p->bg);
                picture_cover(g_art_img, 0, 0, (float)g_w, (float)g_h, 0.5f);
                fill_rr(0, 0, (float)g_w, (float)g_h, 0, argb(0, 70));   // a little darker, so the panels stand out
                gp.del_graphics(g);
            }
            g_dc = keep_dc;
            g_g = keep_g;
            g_art_theme = g_theme; g_art_w = g_w; g_art_h = g_h;
        }
        if (g_art_img && g_art) { BitBlt(g_dc, 0, 0, g_w, g_h, g_art, 0, 0, SRCCOPY); return; }
    }
    fill_rect(0, 0, g_w, g_h, p->bg);
}

static void draw_top(void) {
    if (PAL->art) {
        fill_rr(0, 0, (float)g_w, (float)S(54), 0, argb(PAL->panel, PAL->panel_alpha - 30));
        fill_rect(0, S(54), g_w, S(1), PAL->line);
    }
    int x = S(16), y = S(10), h = S(34);
    {   // 32-bit / 64-bit: which game Play starts, and whose list is shown
        static const char *const lab[2] = {"32-bit", "64-bit"};
        int sw = S(76), tw2 = 2 * sw;
        fill_rr((float)x, (float)y, (float)tw2, (float)h, (float)S(6), argb(PAL->field, 255));
        stroke_rr((float)x, (float)y, (float)tw2, (float)h, (float)S(6), argb(PAL->line2, 255), (float)S(1));
        for (int i = 0; i < 2; i++) {
            int sx = x + i * sw, on = g_mode == i, off = i == 1 && !g_have64;
            if (on) fill_rr((float)sx + S(3), (float)y + S(3), (float)sw - S(6), (float)h - S(6), (float)S(4), argb(PAL->accent, 255));
            else if (is_hover(W_MODE, i)) fill_rr((float)sx + S(3), (float)y + S(3), (float)sw - S(6), (float)h - S(6), (float)S(4), argb(PAL->text, 16));
            text(sx, y, sw, h, lab[i], F_BOLD, on ? (PAL->dark ? 0x1B1407 : 0xFFFFFF) : off ? PAL->faint : PAL->dim, DT_CENTER);
            hit(sx, y, sw, h, W_MODE, i, 1, i ? (g_have64 ? (g_same_list ? "64-bit Attila (Sol Invictus), with the same mod list as 32-bit" : "64-bit Attila (Sol Invictus), with its own mod list")
                                                           : "64-bit Attila is not installed: click to open its Workshop page")
                                              : (g_same_list ? "Total War: ATTILA as it is, with the same mod list as 64-bit" : "Total War: ATTILA as it is, with its own mod list"));
            if (is_focus(W_MODE, i)) focus_ring(sx, y, sw, h, (float)S(6));
        }
        x += tw2 + S(16);
    }
    text(x, y, S(60), h, "Preset", F_SMALL, PAL->dim, DT_LEFT);
    x += text_w("Preset", F_SMALL) + S(10);
    char sub[48] = "";
    int t = ticked_count();
    snprintf(sub, sizeof sub, "%d mod%s%s", t, t == 1 ? "" : "s", preset_changed() ? "  \xE2\x97\x8F changed" : "");
    dropdown(x, y, S(236), h, g_preset[0] ? g_preset : "No preset", sub, W_PRESET, 0,
             "Saved mod lists for both versions: pick one to use it. Ctrl+Z goes back.");
    x += S(236) + S(8);
    x += button(x, y, 0, h, "Save", IC_SAVE, W_SAVE, 0, B_NORMAL, g_preset[0] ? pool("Save the ticks and order to \"%s\" (Ctrl+S)", g_preset) : "Save the ticks and order as a new preset (Ctrl+S)") + S(6);
    x += button(x, y, 0, h, "Save as\xE2\x80\xA6", 0, W_SAVEAS, 0, B_GHOST, "Save the ticks and order as a new preset") + S(6);
    x += button(x, y, 0, h, "Delete", 0, W_DELETE, 0, B_GHOST | (g_preset[0] ? 0 : B_OFF), "Delete this preset (the mods stay)") + S(14);
    fill_rect(x - S(8), y + S(7), S(1), h - S(14), PAL->line2);
    x += button(x, y, 0, h, "Import", IC_DOWNLOAD, W_IMPORT, 0, B_NORMAL, "Use a mod list from a friend: paste it (Ctrl+V) or open a file") + S(6);
    x += button(x, y, 0, h, "Export", IC_UPLOAD, W_EXPORT, 0, B_NORMAL, "Copy the ticked list to send to friends (Ctrl+C)");
    // list code, right
    static ListEntry e[MAX_MODS];
    char code[10];
    list_code(e, ticked_entries(e, MAX_MODS), code);
    int cw = text_w(code, F_MONO) + text_w("List code", F_SMALL) + S(30), cx = g_w - S(16) - cw;
    if (cx > x + S(16)) {
        int hov = is_hover(W_CODE, 0);
        fill_rr((float)cx, (float)y, (float)cw, (float)h, (float)S(6), argb(hov ? PAL->field : PAL->panel, PAL->panel_alpha));
        stroke_rr((float)cx, (float)y, (float)cw, (float)h, (float)S(6), argb(hov ? PAL->accent : PAL->line2, 255), (float)S(1));
        text(cx + S(10), y, cw, h, "List code", F_SMALL, PAL->dim, DT_LEFT);
        text(cx, y, cw - S(10), h, code, F_MONO, PAL->accent_text, DT_RIGHT);
        hit(cx, y, cw, h, W_CODE, 0, 1, "Players with the same code have the same mods, in the same order and the same versions. Click to copy it.");
        if (is_focus(W_CODE, 0)) focus_ring(cx, y, cw, h, (float)S(6));
    }
    // what the code means, under it (right-aligned: it fits whatever the window's width)
    text(S(16), y + h + S(3), g_w - S(32) - S(2), S(16), "Same list code as a friend = the same mods, order and versions", F_SMALL, PAL->faint, DT_RIGHT);
}

static void draw_notes(void) {
    int kinds[4], nn = notes_list(kinds), y = S(10) + S(34) + S(26);
    for (int i = 0; i < nn; i++, y += S(46)) {
        int k = kinds[i], x = S(16), w = g_w - S(32), h = S(38);
        int st = k == 0 ? ST_INFO : k == 1 ? ST_WARN : k == 2 ? ST_BAD : g_toast_kind == 2 ? ST_WARN : g_toast_kind == 1 ? ST_INFO : ST_OK;
        unsigned c = st_col(st);
        fill_rr((float)x, (float)y, (float)w, (float)h, (float)S(7), argb(PAL->panel, PAL->panel_alpha));
        fill_rr((float)x, (float)y, (float)w, (float)h, (float)S(7), argb(c, PAL->dark ? 34 : 26));
        stroke_rr((float)x, (float)y, (float)w, (float)h, (float)S(7), argb(c, 110), (float)S(1));
        icon(x + S(8), y, S(24), h, k == 3 && g_toast_kind == 0 ? IC_CHECK : k == 0 || (k == 3 && g_toast_kind == 1) ? IC_INFO : IC_WARN, F_ICON, c);
        // buttons from the right
        int bx = x + w - S(8), bh = S(28), by = y + (h - bh) / 2;
#define NB(label, arg, style) (bx -= text_w(label, F_SMALL) + S(18), button(bx, by, 0, bh, label, 0, W_NOTE, k * 8 + (arg), style, NULL), bx -= S(6))
        const char *msg = "";
        char buf[300];
        if (k == 0) {
            NB("Copy last log", 1, B_GHOST);
            NB("Use DirectX 11 again", 0, B_ACCENT);
            msg = "DirectX 11 did not start last time, so this PC uses OpenGL now. If something else stopped the game (no Steam?), use DirectX 11 again.";
        } else if (k == 1) {
            bx -= S(28);
            button(bx, by, S(28), bh, "", IC_CLOSE, W_NOTE, 8 + 7, B_GHOST, "Hide this note");
            bx -= S(6);
            NB("Show log file", 1, B_GHOST);
            NB("Copy last log", 0, B_NORMAL);
            msg = "The last game crashed. Please send its log on the Discord.";
        } else if (k == 2) {
            int m = missing_count(), subs = 0;
            for (int j = 0; j < g_nmods && g_mods[j].on; j++) if (g_mods[j].missing && g_mods[j].ws && !ws_view(g_mods[j].ws).subscribed) subs++;
            if (g_filter != 3) NB("Show them", 1, B_GHOST);
            if (subs) { char l[48]; snprintf(l, sizeof l, "Subscribe to %d", subs); NB(l, 0, B_ACCENT); }
            snprintf(buf, sizeof buf, "%d ticked mod%s %s not on this PC. %s left out when you press Play.", m, m == 1 ? "" : "s", m == 1 ? "is" : "are", m == 1 ? "It is" : "They are");
            msg = pool("%s", buf);   // the tooltip keeps this pointer after drawing: not the stack buffer
        } else {
            bx -= S(28);
            button(bx, by, S(28), bh, "", IC_CLOSE, W_NOTE, 24 + 7, B_GHOST, NULL);
            bx -= S(6);
            if (g_toast_undo) NB("Undo", 0, B_NORMAL);
            msg = g_toast;
        }
#undef NB
        text(x + S(38), y, bx - x - S(44), h, msg, F_BODY, PAL->text, DT_LEFT);
        hit(x + S(38), y, bx - x - S(44), h, W_TIP, 0, 0, text_w(msg, F_BODY) > bx - x - S(44) ? msg : NULL);
    }
}

// A short message (saved, copied, undo) floating over the bottom of the list for 8 seconds; it never moves the layout.
static void draw_toast(void) {
    if (!g_toast[0] || g_sheet) return;
    int st = g_toast_kind == 2 ? ST_WARN : g_toast_kind == 1 ? ST_INFO : ST_OK;
    int undo_w = g_toast_undo ? text_w("Undo", F_SMALL) + S(18) + S(6) : 0;
    int lw = g_list_r.right - g_list_r.left, maxw = lw - S(32);
    int w = S(40) + text_w(g_toast, F_BODY) + S(14) + undo_w + S(28) + S(10);
    if (w > maxw) w = maxw;
    int h = S(44), x = g_list_r.left + (lw - w) / 2, y = g_list_r.bottom - S(16) - h;
    unsigned c = st_col(st);
    fill_rr((float)x + S(1), (float)y + S(5), (float)w, (float)h, (float)S(9), argb(0, 80));
    fill_rr((float)x, (float)y, (float)w, (float)h, (float)S(9), argb(PAL->dark ? mix(PAL->panel, 0xFFFFFF, 16) : 0xFFFFFF, 255));
    stroke_rr((float)x, (float)y, (float)w, (float)h, (float)S(9), argb(c, 150), (float)S(1));
    icon(x + S(10), y, S(24), h, st == ST_OK ? IC_CHECK : st == ST_INFO ? IC_INFO : IC_WARN, F_ICON, c);
    int bx = x + w - S(8) - S(28), bh = S(28), by = y + (h - bh) / 2;
    button(bx, by, S(28), bh, "", IC_CLOSE, W_NOTE, 24 + 7, B_GHOST, "Hide");
    if (g_toast_undo) { bx -= undo_w; button(bx, by, 0, bh, "Undo", 0, W_NOTE, 24, B_NORMAL, "Ctrl+Z"); }
    text(x + S(40), y, bx - x - S(48), h, g_toast, F_BODY, PAL->text, DT_LEFT);
    hit(x + S(40), y, bx - x - S(48), h, W_TIP, 0, 0, text_w(g_toast, F_BODY) > bx - x - S(48) ? g_toast : NULL);
}

static void draw_row(const ViewRow *v, int x, int y, int w) {
    Mod *m = &g_mods[v->mod];
    int i = v->mod, h = v->h, sel = i == g_sel, hov = is_hover(W_ROW, i) || is_hover(W_CHECK, i) || is_hover(W_ROW_SUB, i) || is_hover(W_ROW_PAGE, i);
    if (sel) {
        fill_rr((float)x + S(4), (float)y + S(1), (float)w - S(8), (float)h - S(2), (float)S(6), argb(PAL->sel, 255));
        fill_rr((float)x + S(4), (float)y + S(8), (float)S(3), (float)h - S(16), (float)S(1.5f), argb(PAL->accent, 255));
    } else if (hov || g_drag == i) fill_rr((float)x + S(4), (float)y + S(1), (float)w - S(8), (float)h - S(2), (float)S(6), argb(PAL->text, PAL->dark ? 12 : 14));
    hit(x, y, w, h, W_ROW, i, 0, NULL);
    int cy = y + h / 2;
    if (m->on) {
        if (reorder_ok() && !m->missing) for (int a = 0; a < 3; a++) for (int b = 0; b < 2; b++)
            fill_circle((float)(x + S(14) + b * S(5)), (float)(cy - S(5) + a * S(5)), (float)S(1.3f), argb(PAL->faint, hov || sel ? 255 : 150));
        char num[12];
        snprintf(num, sizeof num, "%d", i + 1);
        text(x + S(22), y, S(26), h, num, F_SMALL, PAL->faint, DT_RIGHT);
    }
    // tick box
    int bx = x + S(56), bs = S(18);
    if (m->on) {
        fill_rr((float)bx, (float)(cy - bs / 2), (float)bs, (float)bs, (float)S(4), argb(m->missing ? st_col(ST_BAD) : PAL->accent, 255));
        check_mark((float)bx, (float)(cy - bs / 2), (float)bs, argb(PAL->dark ? 0x1B1407 : 0xFFFFFF, 255), (float)S(2));
    } else stroke_rr((float)bx, (float)(cy - bs / 2), (float)bs, (float)bs, (float)S(4), argb(is_hover(W_CHECK, i) ? PAL->accent : PAL->line2, 255), (float)S(1.5f));
    hit(bx - S(8), y, bs + S(16), h, W_CHECK, i, 0, m->on ? "Untick (Space)" : "Tick: add to the end of the load order (Space)");
    // picture
    int px = x + S(86), ps = S(36), py = cy - ps / 2;
    if (m->missing) {
        fill_rr((float)px, (float)py, (float)ps, (float)ps, (float)S(6), argb(st_col(ST_BAD), 40));
        icon(px, py, ps, ps, IC_DOWNLOAD, F_ICON, st_col(ST_BAD));
    } else if (m->thumb && m->thumb_size == ps) picture_cover(m->thumb, (float)px, (float)py, (float)ps, (float)ps, (float)S(6));
    else {
        fill_rr((float)px, (float)py, (float)ps, (float)ps, (float)S(6), argb(mix(PAL->field, PAL->accent, 30), 255));
        char ini[2] = {(char)toupper((unsigned char)(isalnum((unsigned char)m->name[0]) ? m->name[0] : m->name[1] ? m->name[1] : '?')), 0};
        if (!isalnum((unsigned char)ini[0])) ini[0] = '#';
        text(px, py, ps, ps, ini, F_BOLD, PAL->dim, DT_CENTER);
        if (m->thumb_state == 0 || (m->thumb_state == 2 && m->thumb_size != ps)) { m->thumb_state = 1; want_thumb(m, ps); }
    }
    // right side: state, conflicts, buttons
    int rx = x + w - S(14);
    WsView sv = ws_view(m->ws);
    const char *sub_extra = NULL;
    char tmp[160];
    if (m->missing) {
        if (!m->ws) pill(rx, cy, "No download", ST_WARN, "No Workshop number is known for this pack, so it cannot be downloaded here. Get the file from whoever has it, or untick it.");
        else if (sv.busy) {
            int pw = pill(rx, cy, sv.text, sv.st, NULL);
            if (sv.frac > 0) bar(rx - pw, cy + S(14), pw, S(3), sv.frac, st_col(ST_INFO));
        } else if (sv.known && sv.installed) pill(rx, cy, "No such pack in the item", ST_BAD, "Steam has the Workshop item, but it has no pack of this name (the mod may have renamed it)");
        else if (sv.known && !sv.subscribed) button(rx - text_w("Subscribe", F_SMALL) - S(18), cy - S(14), 0, S(28), "Subscribe", 0, W_ROW_SUB, i, B_ACCENT, "Subscribe on the Steam Workshop; it is ticked here when Steam has downloaded it");
        else button(rx - text_w("Workshop page", F_SMALL) - S(18), cy - S(14), 0, S(28), "Workshop page", 0, W_ROW_PAGE, i, B_GHOST, "Open its Workshop page in Steam");
        sub_extra = m->ws ? (snprintf(tmp, sizeof tmp, "Not on this PC \xC2\xB7 Workshop %llu", m->ws), tmp) : "Not on this PC \xC2\xB7 no Workshop number";
    } else {
        int pw = 0;
        if (sv.known && (sv.busy || (sv.subscribed && (m->ws && sv.frac > 0)))) {
            char t2[64];
            snprintf(t2, sizeof t2, "Update: %s", sv.text);
            pw = pill(rx, cy, t2, ST_INFO, "Steam is downloading a new version of this mod");
            rx -= pw + S(6);
        }
        if (m->on && m->under_n > 0 && m->under_by >= 0 && m->under_by < g_nmods) {
            snprintf(tmp, sizeof tmp, "%d file%s replaced by #%d", m->under_n, m->under_n == 1 ? "" : "s", m->under_by + 1);
            pill(rx, cy, tmp, ST_WARN, pool("%d file%s of this mod %s replaced by mods above it, most by #%d %s. Upper mods win: move this one up if its version should win.",
                                            m->under_n, m->under_n == 1 ? "" : "s", m->under_n == 1 ? "is" : "are", m->under_by + 1, g_mods[m->under_by].name));
        } else if (m->on && m->over_n > 0 && m->over_of >= 0 && m->over_of < g_nmods) {
            snprintf(tmp, sizeof tmp, "replaces %d file%s of #%d", m->over_n, m->over_n == 1 ? "" : "s", m->over_of + 1);
            pill(rx, cy, tmp, ST_MUTE, pool("This mod replaces %d file%s of mods below it, most of #%d %s.", m->over_n, m->over_n == 1 ? "" : "s", m->over_of + 1, g_mods[m->over_of].name));
        }
    }
    // name and the line under it
    int tx = x + S(134), tw = w - S(134) - S(150);
    text(tx, cy - S(19), tw, S(20), m->name, F_BOLD, m->missing ? PAL->dim : PAL->text, DT_LEFT);
    char line[200], sz[32] = "";
    if (sub_extra) snprintf(line, sizeof line, "%s", sub_extra);
    else {
        fmt_size(m->size, sz, sizeof sz);
        if (m->ws) snprintf(line, sizeof line, "Workshop %llu \xC2\xB7 %s", m->ws, sz);
        else if (m->dir[0]) snprintf(line, sizeof line, "Own folder \xC2\xB7 %s", sz);
        else snprintf(line, sizeof line, "Game data folder \xC2\xB7 %s", sz);
    }
    text(tx, cy + S(1), tw, S(18), line, F_SMALL, m->missing ? st_col(ST_BAD) : PAL->faint, DT_LEFT);
}

static void draw_list(void) {
    RECT L = g_list_r;
    int x = L.left, y = L.top, w = L.right - L.left, h = L.bottom - L.top;
    panel(x, y, w, h, (float)S(8));
    // search box (the edit control sits inside it)
    int fx = g_search_r.left, fy = g_search_r.top, fh = g_search_r.bottom - g_search_r.top;
    // filters, right
    char lab[4][40];
    int t = ticked_count(), miss = missing_count();
    snprintf(lab[0], 40, "All  %d", g_nmods);
    snprintf(lab[1], 40, "Ticked  %d", t);
    snprintf(lab[2], 40, "Not ticked  %d", g_nmods - t);
    snprintf(lab[3], 40, "Missing  %d", miss);
    int nf = miss || g_filter == 3 ? 4 : 3, segw[4], total = 0;
    for (int i = 0; i < nf; i++) total += segw[i] = text_w(lab[i], F_SMALL) + S(22);
    int sx = x + w - S(12) - total;
    if (sx < fx + S(200)) { nf = 0; sx = x + w - S(12); }   // too narrow: no filters
    fill_rr((float)sx, (float)fy, (float)total, (float)fh, (float)S(6), argb(PAL->field, 255));
    stroke_rr((float)sx, (float)fy, (float)total, (float)fh, (float)S(6), argb(PAL->line2, 255), (float)S(1));
    for (int i = 0, cx = sx; i < nf; cx += segw[i++]) {
        int on = g_filter == i;
        if (on) fill_rr((float)cx + S(3), (float)fy + S(3), (float)segw[i] - S(6), (float)fh - S(6), (float)S(4), argb(PAL->sel, 255));
        else if (is_hover(W_FILTER, i)) fill_rr((float)cx + S(3), (float)fy + S(3), (float)segw[i] - S(6), (float)fh - S(6), (float)S(4), argb(PAL->text, 14));
        text(cx, fy, segw[i], fh, lab[i], F_SMALL, on ? PAL->text : i == 3 ? st_col(ST_BAD) : PAL->dim, DT_CENTER);
        hit(cx, fy, segw[i], fh, W_FILTER, i, 1, i == 3 ? "Ticked mods that are not on this PC" : NULL);
        if (is_focus(W_FILTER, i)) focus_ring(cx, fy, segw[i], fh, (float)S(6));
    }
    g_search_r.right = (nf ? sx : x + w - S(12)) - S(10);
    int fw = g_search_r.right - fx;
    int ef = GetFocus() == g_search;
    fill_rr((float)fx, (float)fy, (float)fw, (float)fh, (float)S(6), argb(PAL->field, 255));
    stroke_rr((float)fx, (float)fy, (float)fw, (float)fh, (float)S(6), argb(ef ? PAL->accent : PAL->line2, 255), ef ? (float)S(1.5f) : (float)S(1));
    icon(fx + S(6), fy, S(22), fh, IC_SEARCH, F_ICON_S, PAL->faint);
    if (g_search_text[0]) button(fx + fw - S(30), fy + S(4), S(26), fh - S(8), "", IC_CLOSE, W_SEARCH_CLEAR, 0, B_GHOST, "Clear the search (Esc)");
    else text(fx, fy, fw - S(10), fh, "Ctrl+F", F_SMALL, PAL->faint, DT_RIGHT);
    if (g_sheet || g_menu.owner)   // the edit box hides under menus and sheets: its text (or hint) drawn in its place
        text(fx + S(30), fy, fw - S(70), fh, g_search_text[0] ? g_search_text : pool("Search %d mods by name or Workshop number", g_nmods), F_BODY, g_search_text[0] ? PAL->text : PAL->faint, DT_LEFT);
    hit(fx, fy, fw, fh, W_SEARCH, 0, 1, NULL);
    // rows
    RECT R = g_rows_r;
    fill_rect(R.left + S(8), R.top - S(1), R.right - R.left - S(16), S(1), PAL->line);
    int area = R.bottom - R.top;
    SaveDC(g_dc);
    IntersectClipRect(g_dc, R.left, R.top, R.right, R.bottom - S(4));
    gp.clip_rect(g_g, R.left, R.top, R.right - R.left, R.bottom - R.top - S(4), 0);
    int rw = R.right - R.left - S(10);
    hit(R.left, R.top, R.right - R.left, area, W_LIST, 0, 1, NULL);
    for (int i = 0; i < g_nview; i++) {
        ViewRow *v = &g_view[i];
        int vy = R.top + v->y - g_scroll;
        if (vy + v->h < R.top || vy > R.bottom) continue;
        if (v->kind == 2) { draw_row(v, R.left + S(4), vy, rw - S(4)); continue; }
        if (v->kind == 3) { text(R.left + S(26), vy, rw, v->h, "Nothing ticked: the game starts without mods. Tick mods below, or pick a preset.", F_BODY, PAL->faint, DT_LEFT); continue; }
        char head[200];
        const char *cap = v->kind == 0 ? "LOAD ORDER" : "NOT TICKED";
        int cw = text_w(cap, F_HEAD);
        text(R.left + S(18), vy + S(8), cw + 4, S(24), cap, F_HEAD, PAL->dim, DT_LEFT);
        if (v->kind == 0) {
            if (g_search_text[0] || g_filter) snprintf(head, sizeof head, "\xC2\xB7  %d of %d", g_nmatch_t, t);
            else snprintf(head, sizeof head, "\xC2\xB7  %d ticked  \xC2\xB7  upper mods win when two change the same file  \xC2\xB7  drag or Alt+\xE2\x86\x91\xE2\x86\x93", t);
        } else {
            if (g_search_text[0]) snprintf(head, sizeof head, "\xC2\xB7  %d of %d", g_nmatch_r, g_nmods - t);
            else snprintf(head, sizeof head, "\xC2\xB7  %d  \xC2\xB7  A to Z", g_nmods - t);
        }
        const char *bl = v->kind == 0 ? "Tick none" : "Tick all";
        int bw = text_w(bl, F_SMALL) + S(18);
        text(R.left + S(18) + cw + S(8), vy + S(8), rw - cw - bw - S(40), S(24), head, F_SMALL, PAL->faint, DT_LEFT);
        if ((v->kind == 0 && t) || (v->kind == 1 && t < g_nmods)) button(R.left + rw - bw - S(6), vy + S(6), bw, S(26), bl, 0, W_SECT, v->kind, B_GHOST, NULL);
    }
    if (!g_nview || (!g_nmatch_t && !g_nmatch_r && (g_search_text[0] || g_filter))) {
        int cy = R.top + area / 2 - S(50);
        if (!g_nmods) {
            text(R.left, cy, R.right - R.left, S(26), "No mods found", F_TITLE, PAL->text, DT_CENTER);
            text(R.left + S(40), cy + S(30), R.right - R.left - S(80), S(44), "Subscribe to mods on the Steam Workshop, or put .pack files in the game's data folder. Playing without mods works as it is.", F_BODY, PAL->dim, DT_CENTER | DT_WORDBREAK);
            int bw = text_w("Open the Attila Workshop", F_BODY) + S(24);
            button(R.left + (R.right - R.left - bw) / 2, cy + S(86), bw, S(34), "Open the Attila Workshop", 0, W_EMPTY, 0, B_ACCENT, NULL);
        } else {
            text(R.left, cy, R.right - R.left, S(26), pool(g_search_text[0] ? "No mod matches \"%s\"" : "Nothing here", g_search_text), F_TITLE, PAL->text, DT_CENTER);
            int bw = text_w("Show all mods", F_BODY) + S(24);
            button(R.left + (R.right - R.left - bw) / 2, cy + S(40), bw, S(34), "Show all mods", 0, W_EMPTY, 1, B_NORMAL, NULL);
        }
    }
    RestoreDC(g_dc, -1);
    gp.reset_clip(g_g);
    // scroll bar
    if (g_view_h > area) {
        int track = area - S(12), th = track * area / g_view_h;
        if (th < S(30)) th = S(30);
        int ty = R.top + S(6) + (track - th) * g_scroll / (g_view_h - area);
        int hov = is_hover(W_SCROLL, 0) || g_press_id == W_SCROLL, bw = hov ? S(8) : S(5);
        fill_rr((float)(R.right - S(6) - bw), (float)ty, (float)bw, (float)th, (float)bw / 2, argb(PAL->faint, hov ? 200 : 120));
        hit(R.right - S(16), R.top, S(16), area, W_SCROLL, 0, 0, NULL);
    }
    if (is_focus(W_LIST, 0)) focus_ring(R.left + S(3), R.top + S(3), R.right - R.left - S(6), area - S(6), (float)S(6));
}

static void draw_side(void) {
    RECT Sd = g_side_r;
    int x = Sd.left, w = Sd.right - Sd.left, y = Sd.top, avail = Sd.bottom - Sd.top;
    // row heights shrink when the window is short (small window, several notes); the selected-mod card comes and goes
    // tries: full rows with the card, smaller rows with the card, smallest rows with it, then without it
    static const int RA[3] = {46, 40, 36}, RG[3] = {38, 34, 30};
    int ra = 46, rg = 38, gap = S(12), k;
    static const int rows32[2] = {0, 2}, rows64[4] = {0, 1, 2, 3};   // add-on rows: 32-bit has no flags or 4K pictures
    const int *rows = g_mode ? rows64 : rows32;
    int na = g_mode ? 4 : 2, ng = g_mode ? 4 : 0, ncards = ng ? 2 : 1;   // the game card: 64-bit Attila's settings only
#define SIDE_NEED(k) S((float)(40 + (na + 1) * RA[k] + 10 + (ng ? 40 + ng * RG[k] + 4 : 0)))   // + 1: the Discord row
    for (k = 0; k < 3; k++) if (avail - SIDE_NEED(k) - ncards * gap >= S(124)) break;
    if (k == 3) for (k = 0; k < 2; k++) if (avail - SIDE_NEED(k) - (ncards - 1) * gap >= 0) break;
#undef SIDE_NEED
    ra = RA[k]; rg = RG[k];
    int addons_h = S((float)(40 + (na + 1) * ra + 10)), game_h = ng ? S((float)(40 + ng * rg + 4)) : 0;
    int sel_h = avail - addons_h - game_h - ncards * gap;
    int big = sel_h >= S(190);
    if (sel_h > S(190)) sel_h = S(190);
    if (sel_h >= S(124) && g_sel >= 0 && g_sel < g_nmods) {
        Mod *m = &g_mods[g_sel];
        panel(x, y, w, sel_h, (float)S(8));
        char right[48];
        if (m->on) snprintf(right, sizeof right, "#%d of %d", g_sel + 1, ticked_count()); else snprintf(right, sizeof right, "not ticked");
        card_head(x + S(14), y + S(12), w - S(28), "SELECTED MOD", right);
        int ps = big ? S(104) : 0, px = x + S(14), py = y + S(40);
        if (big) {
            if (_stricmp(g_big.name, m->name) || g_big.size != ps) {   // ask for the bigger picture
                if (g_big.img) gp.dispose(g_big.img);
                free(g_big.px);
                memset(&g_big, 0, sizeof g_big);
                snprintf(g_big.name, sizeof g_big.name, "%s", m->name);
                g_big.size = ps;
                g_big.state = 1;
                char p[MAX_PATH * 2] = "";
                if (!picture_path(m, p, sizeof p)) p[0] = 0;
                io_thumb(m->name, p, ps);
            }
            if (g_big.img) picture_cover(g_big.img, (float)px, (float)py, (float)ps, (float)ps, (float)S(7));
            else {
                fill_rr((float)px, (float)py, (float)ps, (float)ps, (float)S(7), argb(PAL->field, 255));
                text(px, py, ps, ps, g_big.state == 3 || m->missing ? "No picture" : "", F_SMALL, PAL->faint, DT_CENTER);
            }
        }
        int tx = px + ps + (big ? S(14) : 0), tw = x + w - S(14) - tx, ty = py - S(2);
        int by = y + sel_h - S(42);   // the buttons' row; the text lines above stop where it starts
        int bx = big ? tx : x + S(14);
        RECT nr = {tx, ty, tx + tw, ty + (big ? S(40) : S(20))};
        HFONT o = (HFONT)SelectObject(g_dc, g_font[F_BOLD]);
        SetTextColor(g_dc, cref(PAL->text));
        int nl;
        WCHAR *wn = wtext(m->name, &nl);
        RECT calc = nr;
        DrawTextW(g_dc, wn, nl, &calc, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX | DT_EDITCONTROL);
        if (calc.bottom > nr.bottom) calc.bottom = nr.bottom;
        nr.bottom = calc.bottom;
        DrawTextW(g_dc, wn, nl, &nr, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
        SelectObject(g_dc, o);
        ty = nr.bottom + S(3);
        char l1[96], sz[32], dt[32];
        if (m->missing) snprintf(l1, sizeof l1, m->ws ? "Not on this PC \xC2\xB7 Workshop %llu" : "Not on this PC", m->ws);
        else if (m->ws) snprintf(l1, sizeof l1, "Workshop %llu", m->ws);
        else snprintf(l1, sizeof l1, m->dir[0] ? "Own folder" : "Game data folder");
        if (!m->missing && !big) {   // short card: number and size on one line
            fmt_size(m->size, sz, sizeof sz);
            snprintf(l1 + strlen(l1), sizeof l1 - strlen(l1), " \xC2\xB7 %s", sz);
        }
        text(tx, ty, tw, S(18), l1, F_SMALL, m->missing ? st_col(ST_BAD) : PAL->dim, DT_LEFT);
        ty += S(18);
        if (!m->missing && big) {
            fmt_size(m->size, sz, sizeof sz);
            fmt_date(m->time, dt, sizeof dt);
            text(tx, ty, tw, S(18), pool("%s \xC2\xB7 changed %s", sz, dt), F_SMALL, PAL->dim, DT_LEFT);
            ty += S(18);
        }
        if (!m->missing && ty + S(18) <= by - S(2)) {
            if (m->on && (m->under_n > 0 || m->over_n > 0)) {
                const char *c = m->under_n > 0 ? pool("%d of its files replaced by #%d", m->under_n, m->under_by + 1) : pool("Replaces %d files of #%d", m->over_n, m->over_of + 1);
                text(tx, ty, tw, S(18), c, F_SMALL, m->under_n > 0 ? st_col(ST_WARN) : PAL->dim, DT_LEFT);
            } else if (m->on && m->under_n == 0 && m->over_n == 0) text(tx, ty, tw, S(18), "No file shared with other ticked mods", F_SMALL, PAL->faint, DT_LEFT);
        }
        if (m->ws) bx += button(bx, by, 0, S(28), "Workshop page", IC_GLOBE, W_SEL_PAGE, 0, B_NORMAL, "Open its page in Steam") + S(6);
        if (!m->missing) bx += button(bx, by, 0, S(28), "Folder", IC_FOLDER, W_SEL_FOLDER, 0, B_GHOST, "Show the pack in its folder") + S(6);
        if (m->missing && m->ws && ws_view(m->ws).known && !ws_view(m->ws).subscribed) bx += button(bx, by, 0, S(28), "Subscribe", 0, W_SEL_SUB, 0, B_ACCENT, NULL) + S(6);
        if (m->on && reorder_ok() && bx + S(64) < x + w - S(10)) {
            bx += button(bx, by, S(30), S(28), "", IC_UP, W_SEL_UP, 0, g_sel > 0 ? B_GHOST : B_GHOST | B_OFF, "Move up: wins over the mods below (Alt+\xE2\x86\x91)") + S(4);
            button(bx, by, S(30), S(28), "", IC_DOWN, W_SEL_DOWN, 0, g_sel + 1 < ticked_count() ? B_GHOST : B_GHOST | B_OFF, "Move down (Alt+\xE2\x86\x93)");
        }
        y += sel_h + gap;
    } else if (sel_h >= S(124)) {
        panel(x, y, w, sel_h, (float)S(8));
        card_head(x + S(14), y + S(12), w - S(28), "SELECTED MOD", NULL);
        text(x + S(14), y + S(40), w - S(28), S(40), g_nmods ? "Click a mod to see its picture and details." : "No mods yet.", F_BODY, PAL->faint, DT_LEFT | DT_WORDBREAK);
        y += sel_h + gap;
    }

    // add-ons
    panel(x, y, w, addons_h, (float)S(8));
    card_head(x + S(14), y + S(12), w - S(28), "ADD-ONS", "by JGdaCaesar");
    int ry = y + S(40);
    for (int ri = 0; ri < na; ri++) {
        int r = rows[ri];
        int a = r == 0 ? AD_PERF : r == 2 ? AD_UPSCALE : r == 3 ? AD_SHARP : -1;   // r == 1: waving flags, part of the engine fix
        int sub = r == 1, rh = S((float)ra), cy = ry + rh / 2;
        if (ri) fill_rect(x + S(14) + (sub ? S(46) : 0), ry, w - S(28) - (sub ? S(46) : 0), S(1), mix(PAL->panel, PAL->line, 200));
        int inst = r == 1 ? g_addon_disk[AD_PERF] : g_addon_disk[a];
        int on = r == 0 ? g_perf : r == 1 ? g_flags : r == 2 ? g_up : 1;
        int can = r == 0 ? inst : r == 1 ? inst && g_perf : r == 2 ? inst && g_addon_disk[AD_PERF] && g_perf : 0;
        int tx = x + S(14) + (sub ? S(28) : 0);
        if (r < 3) toggle(tx, cy - S(10), on && (r == 0 ? inst : can), !can, W_ADDON_TOG, r,
                         r == 0 ? "Load the engine performance fix" : r == 1 ? "Banners wave as cloth (Medieval Kingdoms 1212AD)" : "DLSS or FSR upscaling and frame generation; F8 in the game for its settings");
        static const char *const subs[4] = {"Faster battles, map and menus", "Only with 1212AD ticked", "DLSS or FSR \xC2\xB7 F8 in the game", "On by itself on 4K screens at 150% and more"};
        int nx = tx + S(46), rx = x + w - S(14);
        // state, right
        int pw = 0;
        if (r != 1) {
            WsView v = ws_view(g_addon_ws[a]);
            if (v.busy) {
                pw = pill(rx, cy, v.frac > 0 ? pool("%d%%", (int)(v.frac * 100)) : v.text, v.st, v.text);   // short: the name keeps its room
                if (v.frac > 0) bar(nx, cy + S(20), rx - pw - nx - S(10), S(3), v.frac, st_col(ST_INFO));
            } else if (!inst && v.known && v.subscribed) {
                pw = pill(rx, cy, "Subscribed", ST_INFO, "Steam has it; it is used from the next Play");
            } else if (!inst || (v.known && !v.subscribed)) {
                if (v.known || g_steam != -1) pw = button(rx - text_w("Subscribe", F_SMALL) - S(18), cy - S(14), 0, S(28), "Subscribe", 0, W_ADDON_SUB, a, B_ACCENT,
                                                          inst ? "This copy gets no updates: subscribe on the Workshop" : "Subscribe on the Steam Workshop; Steam downloads it");
                else pw = button(rx - text_w("Workshop page", F_SMALL) - S(18), cy - S(14), 0, S(28), "Workshop page", 0, W_ADDON_PAGE, a, B_GHOST, "Steam is not running: open the page in Steam");
            } else if (r == 0 && !g_perf) pw = pill(rx, cy, "Off", ST_MUTE, NULL);
            else if (r == 2 && (!g_up || !g_perf)) pw = pill(rx, cy, g_perf ? "Off" : "Needs the engine fix", ST_MUTE, NULL);
            else {
                char t[40] = "Installed";
                if (a == AD_PERF && g_perf_version > 0) snprintf(t, sizeof t, g_perf_version % 10 ? "Installed %d.%02d" : "Installed %d.%d", g_perf_version / 100, g_perf_version % 10 ? g_perf_version % 100 : (g_perf_version % 100) / 10);
                pw = pill(rx, cy, t, ST_OK, NULL);
            }
        } else if (inst && g_perf && !g_flags) pw = pill(rx, cy, "Off", ST_MUTE, NULL);
        int nw = rx - pw - S(10) - nx;
        text(nx, cy - S(18), nw, S(20), r == 1 ? "Waving flags" : g_addon_name[a], F_BOLD, (r < 3 && !can && r) ? PAL->dim : PAL->text, DT_LEFT);
        const char *sl = subs[r];
        if (r != 1 && !inst && !ws_view(g_addon_ws[a]).busy) sl = "Not installed";
        else if (r != 1 && inst && ws_view(g_addon_ws[a]).known && !ws_view(g_addon_ws[a]).subscribed) sl = "Copy without updates: not subscribed";
        text(nx, cy + S(1), nw, S(18), sl, F_SMALL, PAL->faint, DT_LEFT);
        ry += rh;
    }
    {   // bug reports: JGdaCaesar's Discord
        int rh = S((float)ra), cy = ry + rh / 2, tx = x + S(14), rx = x + w - S(14);
        fill_rect(tx, ry, w - S(28), S(1), mix(PAL->panel, PAL->line, 200));
        const char *bl = "Join the Discord";
        int bw = button(rx - text_w(bl, F_SMALL) - S(18), cy - S(14), 0, S(28), bl, 0, W_DISCORD, 0, B_NORMAL, "Opens JGdaCaesar's Discord in your browser");
        text(tx, cy - S(18), rx - bw - S(10) - tx, S(20), "Found a bug?", F_BOLD, PAL->text, DT_LEFT);
        text(tx, cy + S(1), rx - bw - S(10) - tx, S(18), "Report it on JGdaCaesar's Discord", F_SMALL, PAL->faint, DT_LEFT);
    }
    y += addons_h + gap;
    if (!ng) return;

    // game settings (64-bit Attila's settings.txt)
    panel(x, y, w, game_h, (float)S(8));
    card_head(x + S(14), y + S(12), w - S(28), "64-BIT ATTILA", g_sol_ok ? NULL : "its own window opens");
    static const char *const labels[3] = {"Interface size", "Texture sharpness", "Shadows redrawn"};
    static const char *const hints[3] = {"Auto looks like 1080p", "seen in battle", "less often = faster"};
    static const char *const shadow_texts[4] = {"Every frame", "Every 2nd frame", "Every 3rd frame", "Every 4th frame"};
    int gy = y + S(40), dw = S(172);
    for (int i = 0; i < 4; i++, gy += S((float)rg)) {
        int cy = gy + S((float)rg) / 2 - S(2);
        int hint = rg >= 34;   // short rows: the label only
        text(x + S(14), hint ? cy - S(16) : cy - S(9), w - dw - S(30), S(18), i < 3 ? labels[i] : "Show frame rate", F_BODY, PAL->text, DT_LEFT);
        if (hint) text(x + S(14), cy + S(1), w - dw - S(30), S(16), i < 3 ? hints[i] : "a small number in a corner", F_SMALL, PAL->faint, DT_LEFT);
        if (i == 0) dropdown(x + w - S(14) - dw, cy - S(15), dw, S(30), g_sizes[g_set_size].text, NULL, W_SETTING, 0, NULL);
        else if (i == 1) dropdown(x + w - S(14) - dw, cy - S(15), dw, S(30), g_sharp_texts[g_set_sharp], NULL, W_SETTING, 1, NULL);
        else if (i == 2) dropdown(x + w - S(14) - dw, cy - S(15), dw, S(30), shadow_texts[g_set_shadow], NULL, W_SETTING, 2, NULL);
        else toggle(x + w - S(14) - S(36), cy - S(10), g_set_fps, 0, W_FPS, 0, NULL);
    }
}

static void draw_footer(void) {
    int y = g_h - S(52), h = S(40), x = S(16);
    if (PAL->art) {
        fill_rr(0, (float)(g_h - S(62)), (float)g_w, (float)S(62), 0, argb(PAL->panel, PAL->panel_alpha - 30));
        fill_rect(0, g_h - S(62), g_w, S(1), PAL->line);
    }
    const char *tl = pool("Theme: %s", g_theme_names[g_theme]);
    x += dropdown(x, y + S(4), text_w(tl, F_BOLD) + S(52), S(32), tl, NULL, W_THEME, 0, "The look of this window") + S(8);
    x += button(x, y + S(4), 0, S(32), "Log", IC_DOWN, W_LOG, 0, B_GHOST | B_ICON_RIGHT, "The last game's log, to send with a bug report") + S(6);
    x += button(x, y + S(4), 0, S(32), "Game files", IC_DOWN, W_FILES, 0, B_GHOST | B_ICON_RIGHT,
                g_mode ? "64-bit Attila's preferences, scripts and saves" : "Attila's preferences, scripts and saves") + S(6);
    // the other launcher: 64-bit Attila's own window for this start only; CA's launcher this time or from now on (it asks)
    x += button(x, y + S(4), 0, S(32), g_mode ? "64-bit Attila's own window" : "CA's launcher", 0, W_LAUNCHER, 0, B_GHOST,
                g_mode ? "Start 64-bit Attila with its own window this time (the ticks made here are kept)"
                       : "Open CA's launcher instead of this one: this time, or from now on") + S(18);
    int pw = S(188), qw = text_w("Quit", F_BODY) + S(34);
    int px = g_w - S(16) - pw - S(3), qx = px - S(12) - qw;
    const char *keys = "Space  tick    Alt+\xE2\x86\x91\xE2\x86\x93  move    Ctrl+F  search    Ctrl+Z  undo    Enter  Play";
    if (text_w(keys, F_SMALL) < qx - x - S(16)) text(x, y, qx - x, h, keys, F_SMALL, PAL->faint, DT_LEFT);
    button(qx, y + S(2), qw, h - S(4), "Quit", 0, W_QUIT, 0, B_NORMAL, "Close without starting the game");
    button(px, y, pw, h, g_mode ? "Play 64-bit" : "Play 32-bit", IC_PLAY, W_PLAY, 0, B_PLAY,
           g_mode ? "Start 64-bit Attila with the ticked mods (Enter)" : "Start Total War: ATTILA with the ticked mods (Enter)");
}

static void menu_draw(void) {
    if (!g_menu.owner) return;
    int iw = S(220);
    for (int i = 0; i < g_menu.n; i++) { int tw = text_w(g_menu.text[i], F_BODY) + S(52); if (tw > iw) iw = tw; }
    if (iw < g_menu.anchor.right - g_menu.anchor.left) iw = g_menu.anchor.right - g_menu.anchor.left;
    int ih = S(32), h = S(8) + g_menu.n * ih;
    for (int i = 0; i < g_menu.n; i++) if (g_menu.line[i]) h += S(9);
    int x = g_menu.anchor.left, y = g_menu.anchor.bottom + S(4);
    if (y + h > g_h - S(8)) y = g_menu.anchor.top - S(4) - h;
    if (y < S(8)) y = S(8);
    if (x + iw > g_w - S(8)) x = g_w - S(8) - iw;
    g_overlay_from = g_nhits;
    hit(0, 0, g_w, g_h, W_MENU, -1, 0, NULL);   // a click outside closes it
    fill_rr((float)x + S(2), (float)y + S(4), (float)iw, (float)h, (float)S(8), argb(0, 60));
    fill_rr((float)x, (float)y, (float)iw, (float)h, (float)S(8), argb(PAL->dark ? mix(PAL->panel, 0xFFFFFF, 10) : 0xFFFFFF, 255));
    stroke_rr((float)x, (float)y, (float)iw, (float)h, (float)S(8), argb(PAL->line2, 255), (float)S(1));
    int cy = y + S(4);
    for (int i = 0; i < g_menu.n; i++) {
        if (g_menu.line[i]) { fill_rect(x + S(10), cy + S(4), iw - S(20), S(1), PAL->line); cy += S(9); }
        int hov = g_menu.hover == i;
        if (hov) fill_rr((float)x + S(4), (float)cy, (float)iw - S(8), (float)ih, (float)S(5), argb(PAL->sel, 255));
        if (g_menu.value[i] == g_menu.check && g_menu.check >= 0) icon(x + S(8), cy, S(22), ih, IC_CHECK, F_ICON_S, PAL->accent_text);
        text(x + S(34), cy, iw - S(44), ih, g_menu.text[i], F_BODY, g_menu.value[i] < -100 ? PAL->faint : PAL->text, DT_LEFT);
        if (g_menu.value[i] >= -100) hit(x, cy, iw, ih, W_MENU, i, 0, NULL);
        cy += ih;
    }
}
static void menu_open(int owner, int x, int y, int w, int h) {
    g_menu.owner = owner; g_menu.n = 0; g_menu.hover = -1; g_menu.check = -1;
    SetRect(&g_menu.anchor, x, y, x + w, y + h);
#define ITEM(t, v, ln) do { if (g_menu.n < (int)(sizeof g_menu.value / sizeof g_menu.value[0])) { snprintf(g_menu.text[g_menu.n], 96, "%s", t); g_menu.value[g_menu.n] = v; g_menu.line[g_menu.n++] = ln; } } while (0)
    if (owner == M_PRESET) {
        presets_scan();
        if (!g_npresets) ITEM("No presets yet: tick mods and press Save", -200, 0);
        for (int i = 0; i < g_npresets; i++) { ITEM(g_presets[i], i, 0); if (!_stricmp(g_presets[i], g_preset)) g_menu.check = i; }
        ITEM("New preset from the ticked mods\xE2\x80\xA6", 1000, 1);
        if (g_preset[0]) ITEM("Stop using a preset (keep the ticks)", 1001, 0);
        if (g_have64) ITEM(g_same_list ? "\xE2\x9C\x93 One mod list for 32-bit and 64-bit" : "One mod list for 32-bit and 64-bit", 1002, 1);
    } else if (owner == M_IMPORT) {
        ITEM("Paste a list from the clipboard   Ctrl+V", 0, 0);
        ITEM("Open a list file\xE2\x80\xA6", 1, 0);
        ITEM("Normal Attila's mod list (its launcher)", 2, 1);
    } else if (owner == M_EXPORT) {
        ITEM("Copy the list to the clipboard   Ctrl+C", 0, 0);
        ITEM("Save the list to a file\xE2\x80\xA6", 1, 0);
        ITEM("Copy only the list code", 2, 1);
    } else if (owner == M_THEME) {
        for (int i = 0; i <= TH_DARK; i++) ITEM(g_theme_names[i], i, 0);
        g_menu.check = g_theme;
    } else if (owner == M_LOG) {
        ITEM("Copy last log", 0, 0);
        ITEM("Show log file", 1, 0);
    } else if (owner == M_FILES) {   // the version in use (host_ui.inc game_files)
        ITEM("Edit preferences (preferences.script.txt)", 0, 0);
        ITEM("Open the scripts folder", 1, 0);
        ITEM("Open the save folder", 2, 0);
    } else if (owner == M_SET0) { for (int i = 0; i < N_SIZES; i++) ITEM(g_sizes[i].text, i, 0); g_menu.check = g_set_size; }
    else if (owner == M_SET1) { for (int i = 0; i < N_SHARP; i++) ITEM(g_sharp_texts[i], i, 0); g_menu.check = g_set_sharp; }
    else if (owner == M_SET2) {
        static const char *const t[4] = {"Every frame", "Every 2nd frame", "Every 3rd frame", "Every 4th frame"};
        for (int i = 0; i < 4; i++) ITEM(t[i], i, 0);
        g_menu.check = g_set_shadow;
    }
#undef ITEM
    if (g_kbd) for (g_menu.hover = 0; g_menu.hover < g_menu.n && g_menu.value[g_menu.hover] < -100; g_menu.hover++) {}
    if (g_menu.check >= 0) for (int i = 0; i < g_menu.n; i++) if (g_menu.value[i] == g_menu.check) g_menu.hover = g_kbd ? i : g_menu.hover;
    InvalidateRect(g_win, NULL, FALSE);
}
static void menu_close(void) { g_menu.owner = 0; InvalidateRect(g_win, NULL, FALSE); }

// ---- sheets
static void imp_counts(int *found, int *notsub, int *busy, int *local) {
    *found = *notsub = *busy = *local = 0;
    for (int i = 0; i < g_imp_n; i++) {
        Mod *m = find_mod(g_imp[i].name);
        if (m && !m->missing) (*found)++;
        else if (!g_imp[i].ws) (*local)++;
        else { WsView v = ws_view(g_imp[i].ws); if (v.busy) (*busy)++; else (*notsub)++; }
    }
}
static void sheet_frame(int w, int h, int *x, int *y) {
    fill_rr(0, 0, (float)g_w, (float)g_h, 0, argb(0, PAL->dark ? 150 : 90));
    *x = (g_w - w) / 2;
    *y = (g_h - h) / 2;
    g_overlay_from = g_nhits;
    hit(0, 0, g_w, g_h, W_SHEET, -1, 0, NULL);
    fill_rr((float)*x + S(2), (float)*y + S(6), (float)w, (float)h, (float)S(10), argb(0, 80));
    fill_rr((float)*x, (float)*y, (float)w, (float)h, (float)S(10), argb(PAL->dark ? mix(PAL->panel, 0xFFFFFF, 6) : 0xFFFFFF, 255));
    stroke_rr((float)*x, (float)*y, (float)w, (float)h, (float)S(10), argb(PAL->line2, 255), (float)S(1));
}
static void draw_sheet(void) {
    if (!g_sheet) return;
    int x, y;
    if (g_sheet == SH_IMPORT) {
        int w = S(680), rows = g_imp_n < 8 ? (g_imp_n ? g_imp_n : 1) : 8, rh = S(44);
        if (w > g_w - S(40)) w = g_w - S(40);
        int list_h = rows * rh, h = S(126) + list_h + S(64);
        if (h > g_h - S(40)) { list_h -= h - (g_h - S(40)); h = g_h - S(40); }
        sheet_frame(w, h, &x, &y);
        text(x + S(20), y + S(16), w - S(40), S(26), "Import a mod list", F_TITLE, PAL->text, DT_LEFT);
        static ListEntry tmp[MAX_MODS];
        char code[10];
        memcpy(tmp, g_imp, sizeof(ListEntry) * (size_t)g_imp_n);
        list_code(tmp, g_imp_n, code);
        // the code here (the mods' versions on this PC) against the sender's
        text(x + S(20), y + S(44), w - S(40), S(18), g_imp_code[0] && strcmp(g_imp_code, code)
                 ? pool("%s \xC2\xB7 %d mod%s \xC2\xB7 list code %s here, %s for the sender: not the same mods or versions yet", g_imp_from, g_imp_n, g_imp_n == 1 ? "" : "s", code, g_imp_code)
                 : pool("%s \xC2\xB7 %d mod%s \xC2\xB7 list code %s%s", g_imp_from, g_imp_n, g_imp_n == 1 ? "" : "s", code, g_imp_code[0] ? ", the same as the sender's" : ""),
             F_SMALL, PAL->dim, DT_LEFT);
        int f, ns, b, l;
        imp_counts(&f, &ns, &b, &l);
        int px = x + S(20);
#define PILLL(cond, st, ...) if (cond) { const char *s_ = pool(__VA_ARGS__); px += pill(px + text_w(s_, F_SMALL) + S(16), y + S(80), s_, st, NULL) + S(8); }
        PILLL(1, ST_OK, "%d on this PC", f);
        PILLL(ns, ST_BAD, "%d not subscribed", ns);
        PILLL(b, ST_INFO, "%d downloading", b);
        PILLL(l, ST_WARN, "%d not on the Workshop", l);
#undef PILLL
        int ly = y + S(100);
        fill_rect(x, ly, w, S(1), PAL->line);
        SetRect(&g_sheet_rows_r, x, ly + S(1), x + w, ly + S(1) + list_h);
        SaveDC(g_dc);
        IntersectClipRect(g_dc, x, ly + S(1), x + w, ly + S(1) + list_h);
        gp.clip_rect(g_g, x, ly + S(1), w, list_h, 0);
        int maxs = g_imp_n * rh - list_h;
        if (g_sheet_scroll > maxs) g_sheet_scroll = maxs;
        if (g_sheet_scroll < 0) g_sheet_scroll = 0;
        if (!g_imp_n) text(x + S(20), ly + S(1), w - S(40), rh, "No mod list found in it. A list has one line per mod: a Workshop number (or \"data\") and the pack's name.", F_BODY, PAL->dim, DT_LEFT);
        for (int i = 0; i < g_imp_n; i++) {
            int ry = ly + S(1) + i * rh - g_sheet_scroll;
            if (ry + rh < ly || ry > ly + list_h) continue;
            int cy = ry + rh / 2;
            if (i) fill_rect(x + S(20), ry, w - S(40), S(1), mix(PAL->panel, PAL->line, 160));
            text(x + S(14), ry, S(26), rh, pool("%d", i + 1), F_SMALL, PAL->faint, DT_RIGHT);
            Mod *m = find_mod(g_imp[i].name);
            int ok = m && !m->missing, st;
            WCHAR ic;
            char sub[160], right[64] = "";
            WsView v = ws_view(g_imp[i].ws);
            if (ok) { st = ST_OK; ic = IC_CHECK; snprintf(sub, sizeof sub, m->ws ? "On this PC \xC2\xB7 Workshop %llu" : "On this PC", m->ws); }
            else if (!g_imp[i].ws) { st = ST_WARN; ic = IC_WARN; snprintf(sub, sizeof sub, "Not on the Workshop: ask the player who sent the list for the file"); }
            else if (v.busy) { st = ST_INFO; ic = IC_DOWNLOAD; snprintf(sub, sizeof sub, "Workshop %llu", g_imp[i].ws); snprintf(right, sizeof right, "%s", v.text); }
            else { st = ST_BAD; ic = IC_CLOSE; snprintf(sub, sizeof sub, v.known && v.installed ? "Workshop %llu \xC2\xB7 subscribed, but it has no pack of this name" : "Workshop %llu \xC2\xB7 not subscribed", g_imp[i].ws); }
            fill_circle((float)(x + S(58)), (float)cy, (float)S(11), argb(st_col(st), 46));
            icon(x + S(47), cy - S(11), S(22), S(22), ic, F_ICON_S, st_col(st));
            int rx = x + w - S(20);
            if (st == ST_BAD && v.known && !v.subscribed) rx -= button(rx - text_w("Subscribe", F_SMALL) - S(18), cy - S(14), 0, S(28), "Subscribe", 0, W_SHEET_SUB, i, B_ACCENT, NULL) + S(8);
            else if (st == ST_BAD && !v.known) rx -= button(rx - text_w("Workshop page", F_SMALL) - S(18), cy - S(14), 0, S(28), "Workshop page", 0, W_SHEET_SUB, i, B_GHOST, NULL) + S(8);
            else if (right[0]) { int pw = pill(rx, cy, right, ST_INFO, NULL); if (v.frac > 0) bar(rx - pw, cy + S(14), pw, S(3), v.frac, st_col(ST_INFO)); rx -= pw + S(8); }
            text(x + S(80), cy - S(18), rx - x - S(88), S(20), g_imp[i].name, F_BOLD, PAL->text, DT_LEFT);
            text(x + S(80), cy + S(1), rx - x - S(88), S(18), sub, F_SMALL, st == ST_OK ? PAL->faint : st_col(st), DT_LEFT);
        }
        RestoreDC(g_dc, -1);
        gp.reset_clip(g_g);
        if (maxs > 0) {
            int th = list_h * list_h / (g_imp_n * rh), ty = ly + S(1) + (list_h - th) * g_sheet_scroll / maxs;
            fill_rr((float)(x + w - S(9)), (float)ty, (float)S(5), (float)th, (float)S(2.5f), argb(PAL->faint, 140));
            hit(x, ly, w, list_h, W_SHEET_SCROLL, 0, 0, NULL);
        }
        fill_rect(x, ly + S(1) + list_h, w, S(1), PAL->line);
        int by = y + h - S(50);
        int subs = 0;
        for (int i = 0; i < g_imp_n; i++) { Mod *m = find_mod(g_imp[i].name); if ((!m || m->missing) && g_imp[i].ws && ws_view(g_imp[i].ws).known && !ws_view(g_imp[i].ws).subscribed) subs++; }
        // using the list subscribes to the missing ones (Steam downloads them; each is ticked in its place when it arrives)
        const char *use = subs ? "Subscribe and use" : "Use this list";
        int rx = x + w - S(20);
        rx -= button(rx - (text_w(use, F_BODY) + S(24)), by, 0, S(34), use, 0, W_SHEET, 0, B_ACCENT,
                     subs ? "Subscribe to the missing mods and tick exactly these mods in this order (Enter)" : "Tick exactly these mods in this order (Enter)") + S(8);
        rx -= button(rx - (text_w("Save as preset\xE2\x80\xA6", F_BODY) + S(24)), by, 0, S(34), "Save as preset\xE2\x80\xA6", 0, W_SHEET, 1, B_NORMAL, "Keep it as a preset, and use it") + S(8);
        button(rx - (text_w("Cancel", F_BODY) + S(24)), by, 0, S(34), "Cancel", 0, W_SHEET, 2, B_GHOST, "Esc");
    } else if (g_sheet == SH_NAME) {
        int w = S(460), h = S(212);
        sheet_frame(w, h, &x, &y);
        text(x + S(20), y + S(16), w - S(40), S(26), g_name_for ? "Save the imported list as a preset" : "Save the ticked mods as a new preset", F_TITLE, PAL->text, DT_LEFT);
        text(x + S(20), y + S(46), w - S(40), S(18), "Its name", F_SMALL, PAL->dim, DT_LEFT);
        SetRect(&g_name_r, x + S(20), y + S(68), x + w - S(20), y + S(68) + S(34));
        fill_rr((float)g_name_r.left, (float)g_name_r.top, (float)(w - S(40)), (float)S(34), (float)S(6), argb(PAL->field, 255));
        stroke_rr((float)g_name_r.left, (float)g_name_r.top, (float)(w - S(40)), (float)S(34), (float)S(6), argb(PAL->accent, 255), (float)S(1.5f));
        text(x + S(20), y + S(106), w - S(40), S(36), g_name_err[0] ? g_name_err : "Letters, numbers, spaces and - _ ( ) , . ' & + !", F_SMALL, g_name_err[0] ? st_col(g_name_overwrite ? ST_WARN : ST_BAD) : PAL->faint, DT_LEFT | DT_WORDBREAK);
        int by = y + h - S(52), rx = x + w - S(20);
        rx -= button(rx - (text_w(g_name_overwrite ? "Replace it" : "Save", F_BODY) + S(24)), by, 0, S(34), g_name_overwrite ? "Replace it" : "Save", 0, W_SHEET, 0, B_ACCENT, "Enter") + S(8);
        button(rx - (text_w("Cancel", F_BODY) + S(24)), by, 0, S(34), "Cancel", 0, W_SHEET, 2, B_GHOST, "Esc");
    } else if (g_sheet == SH_CONFIRM) {
        int w = S(440), h = S(170);
        sheet_frame(w, h, &x, &y);
        text(x + S(20), y + S(16), w - S(40), S(26), pool("Delete the preset \"%s\"?", g_preset), F_TITLE, PAL->text, DT_LEFT);
        text(x + S(20), y + S(50), w - S(40), S(40), "Only the saved list goes. The ticks and the mods stay as they are.", F_BODY, PAL->dim, DT_LEFT | DT_WORDBREAK);
        int by = y + h - S(52), rx = x + w - S(20);
        rx -= button(rx - (text_w("Delete", F_BODY) + S(24)), by, 0, S(34), "Delete", IC_DELETE, W_SHEET, 0, B_ACCENT, "Enter") + S(8);
        button(rx - (text_w("Cancel", F_BODY) + S(24)), by, 0, S(34), "Cancel", 0, W_SHEET, 2, B_GHOST, "Esc");
    } else if (g_sheet == SH_ASK) {   // answers: W_SHEET 0 (first button), 1 (second), 2 (Cancel)
        static const char *const title[4] = {"Open CA's launcher", "Steam's Play opens CA's launcher", "64-bit Attila is not installed",
                                             "Welcome to Total War Mod Manager"};
        static const char *const body[4] = {
            "From now on, or only this time? From now on, Steam's Play button opens CA's launcher. To come back, hold Shift "
            "while pressing Play, or set launcher=1 in heapfix.ini in Attila's folder.",
            "heapfix.ini in Attila's folder says launcher=2. Make Total War Mod Manager what Steam's Play button opens again?",
            "64-bit Attila is a free mod by Sol Invictus on the Steam Workshop. Subscribe and install it now? Its installer opens "
            "in its own window. If Steam's 64-bit game file is not on this PC yet, the installer shows how to download it.",
            "From the Attila Engine Performance Fix: 32-bit and 64-bit Attila in one window, with your mod lists, presets and "
            "sharing. Prefer CA's launcher? Press \"CA's launcher\" at the bottom (in 32-bit)."};
        static const char *const first[4] = {"This time only", "Use Total War Mod Manager", "Subscribe and install", "OK"};
        static const char *const second[4] = {"From now on", NULL, NULL, NULL};
        const char *last = g_ask == ASK_CA ? "Cancel" : g_ask == ASK_WELCOME ? NULL : "Not now";
        int k = g_ask < 0 || g_ask > 3 ? 0 : g_ask;
        int w = S(500), h = S(200);
        sheet_frame(w, h, &x, &y);
        text(x + S(20), y + S(16), w - S(40), S(26), title[k], F_TITLE, PAL->text, DT_LEFT);
        text(x + S(20), y + S(50), w - S(40), S(80), body[k], F_BODY, PAL->dim, DT_LEFT | DT_WORDBREAK);
        int by = y + h - S(52), rx = x + w - S(20);
        rx -= button(rx - (text_w(first[k], F_BODY) + S(24)), by, 0, S(34), first[k], 0, W_SHEET, 0, B_ACCENT, "Enter") + S(8);
        if (second[k]) rx -= button(rx - (text_w(second[k], F_BODY) + S(24)), by, 0, S(34), second[k], 0, W_SHEET, 1, B_NORMAL, NULL) + S(8);
        if (last) button(rx - (text_w(last, F_BODY) + S(24)), by, 0, S(34), last, 0, W_SHEET, 2, B_GHOST, "Esc");
    }
}

static void draw_tip(void) {
    if (!g_tip_ready) return;
    const char *tip = NULL;
    for (int i = g_nhits - 1; i >= 0; i--)
        if (PtInRect(&g_hits[i].r, g_mouse)) { tip = g_hits[i].tip; if (tip || g_hits[i].id != W_TIP) break; }
    if (!tip || !tip[0]) return;
    int maxw = S(340);
    RECT r = {0, 0, maxw, 0};
    HFONT o = (HFONT)SelectObject(g_dc, g_font[F_SMALL]);
    int n;
    WCHAR *t = wtext(tip, &n);
    DrawTextW(g_dc, t, n, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(g_dc, o);
    int w = r.right + S(20), h = r.bottom + S(14), x = g_mouse.x + S(12), y = g_mouse.y + S(20);
    if (x + w > g_w - S(6)) x = g_w - S(6) - w;
    if (y + h > g_h - S(6)) y = g_mouse.y - S(10) - h;
    fill_rr((float)x, (float)y, (float)w, (float)h, (float)S(6), argb(PAL->dark ? 0x2A2B31 : 0x2A2926, 245));
    text(x + S(10), y + S(7), w - S(20), h - S(14), tip, F_SMALL, 0xF0EEE8, DT_LEFT | DT_WORDBREAK);
}

static void place_edits(void) {
    int eh = S(20);
    if (g_search) {
        RECT r = g_search_r;
        int show = !g_sheet && !g_menu.owner;
        int ex = r.left + S(30), ew = r.right - r.left - S(30) - (g_search_text[0] ? S(34) : S(60)), ey = r.top + (r.bottom - r.top - eh) / 2;
        RECT now;
        GetWindowRect(g_search, &now);
        MapWindowPoints(NULL, g_win, (POINT *)&now, 2);
        if (now.left != ex || now.top != ey || now.right - now.left != ew || now.bottom - now.top != eh)
            SetWindowPos(g_search, NULL, ex, ey, ew > 10 ? ew : 10, eh, SWP_NOZORDER | SWP_NOACTIVATE);
        if (!!IsWindowVisible(g_search) != show) ShowWindow(g_search, show ? SW_SHOWNA : SW_HIDE);
    }
    if (g_name_edit) {
        int show = g_sheet == SH_NAME;
        if (show) SetWindowPos(g_name_edit, HWND_TOP, g_name_r.left + S(10), g_name_r.top + (S(34) - eh) / 2, g_name_r.right - g_name_r.left - S(20), eh, SWP_NOACTIVATE);
        if (!!IsWindowVisible(g_name_edit) != show) ShowWindow(g_name_edit, show ? SW_SHOWNA : SW_HIDE);
    }
}

static void paint(HDC dc) {
    if (!g_bb || g_w <= 0 || g_h <= 0) return;
    g_dc = g_bb;
    g_g = NULL;
    if (gp.dll) { gp.from_hdc(g_bb, &g_g); gp.smooth(g_g, 4); gp.pixoff(g_g, 4); }
    g_nhits = 0;
    g_overlay_from = 0;
    g_pool_at = 0;
    layout();
    draw_background();
    draw_top();
    draw_notes();
    draw_list();
    draw_side();
    draw_footer();
    draw_toast();
    draw_sheet();
    menu_draw();
    draw_tip();
    if (g_g) gp.del_graphics(g_g);
    g_g = NULL;
    BitBlt(dc, 0, 0, g_w, g_h, g_bb, 0, 0, SRCCOPY);
    place_edits();
}
