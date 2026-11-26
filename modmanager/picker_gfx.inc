// Portions of this file (the GDI+ loading (gp, gdiplus_start)) come from Sol Invictus's 64-bit Attila (attila64/loader/picker.c) and are under its MIT license: see SOL_INVICTUS_MIT.txt.
// ---- Drawing for the start window (redo, 2026-11-25): smooth shapes and pictures with GDI+ (gdiplus.dll, loaded when the
// window opens, as before: no link change), sharp text with GDI (ClearType), everything sized for the monitor's scaling.

typedef struct { UINT32 version; void *debug_cb; BOOL no_thread; BOOL no_codecs; } GpStartIn;
typedef float REAL;
static struct {
    HMODULE dll;
    ULONG_PTR token;
    int (WINAPI *startup)(ULONG_PTR *, const GpStartIn *, void *);
    void (WINAPI *shutdown)(ULONG_PTR);
    int (WINAPI *load)(const WCHAR *, void **);
    int (WINAPI *width)(void *, UINT *);
    int (WINAPI *height)(void *, UINT *);
    int (WINAPI *from_hdc)(HDC, void **);
    int (WINAPI *interp)(void *, int);
    int (WINAPI *draw)(void *, void *, INT, INT, INT, INT);
    int (WINAPI *del_graphics)(void *);
    int (WINAPI *dispose)(void *);
    int (WINAPI *smooth)(void *, int);
    int (WINAPI *pixoff)(void *, int);
    int (WINAPI *new_path)(int, void **);
    int (WINAPI *arc)(void *, REAL, REAL, REAL, REAL, REAL, REAL);
    int (WINAPI *line)(void *, REAL, REAL, REAL, REAL);
    int (WINAPI *close)(void *);
    int (WINAPI *del_path)(void *);
    int (WINAPI *solid)(UINT32, void **);
    int (WINAPI *fill_path)(void *, void *, void *);
    int (WINAPI *del_brush)(void *);
    int (WINAPI *pen)(UINT32, REAL, int, void **);
    int (WINAPI *draw_path)(void *, void *, void *);
    int (WINAPI *del_pen)(void *);
    int (WINAPI *fill_ellipse)(void *, void *, REAL, REAL, REAL, REAL);
    int (WINAPI *bitmap_scan0)(INT, INT, INT, INT, BYTE *, void **);
    int (WINAPI *image_graphics)(void *, void **);
    int (WINAPI *clear)(void *, UINT32);
    int (WINAPI *clip_path)(void *, void *, int);
    int (WINAPI *from_hbitmap)(HBITMAP, HPALETTE, void **);
    int (WINAPI *save)(void *, const WCHAR *, const CLSID *, const void *);
    int (WINAPI *draw_lines)(void *, void *, const void *, INT);
    int (WINAPI *reset_clip)(void *);
    int (WINAPI *clip_rect)(void *, INT, INT, INT, INT, int);
} gp;

static void gdiplus_start(void) {
    GpStartIn in = {1, NULL, FALSE, FALSE};
    if (!(gp.dll = LoadLibraryA("gdiplus.dll"))) return;
#define GP(f, n) *(FARPROC *)&gp.f = GetProcAddress(gp.dll, n)
    GP(startup, "GdiplusStartup"); GP(shutdown, "GdiplusShutdown"); GP(load, "GdipLoadImageFromFile");
    GP(width, "GdipGetImageWidth"); GP(height, "GdipGetImageHeight"); GP(from_hdc, "GdipCreateFromHDC");
    GP(interp, "GdipSetInterpolationMode"); GP(draw, "GdipDrawImageRectI"); GP(del_graphics, "GdipDeleteGraphics");
    GP(dispose, "GdipDisposeImage"); GP(smooth, "GdipSetSmoothingMode"); GP(pixoff, "GdipSetPixelOffsetMode");
    GP(new_path, "GdipCreatePath"); GP(arc, "GdipAddPathArc"); GP(line, "GdipAddPathLine"); GP(close, "GdipClosePathFigure");
    GP(del_path, "GdipDeletePath"); GP(solid, "GdipCreateSolidFill"); GP(fill_path, "GdipFillPath"); GP(del_brush, "GdipDeleteBrush");
    GP(pen, "GdipCreatePen1"); GP(draw_path, "GdipDrawPath"); GP(del_pen, "GdipDeletePen"); GP(fill_ellipse, "GdipFillEllipse");
    GP(bitmap_scan0, "GdipCreateBitmapFromScan0"); GP(image_graphics, "GdipGetImageGraphicsContext"); GP(clear, "GdipGraphicsClear");
    GP(clip_path, "GdipSetClipPath"); GP(from_hbitmap, "GdipCreateBitmapFromHBITMAP"); GP(save, "GdipSaveImageToFile");
    GP(draw_lines, "GdipDrawLines"); GP(reset_clip, "GdipResetClip"); GP(clip_rect, "GdipSetClipRectI");
#undef GP
    void **f = (void **)&gp.startup;
    int all = 1;
    for (size_t i = 0; i < (sizeof gp - offsetof(__typeof__(gp), startup)) / sizeof(void *); i++) if (!f[i]) all = 0;
    if (!all || gp.startup(&gp.token, &in, NULL) != 0) {
        FreeLibrary(gp.dll);
        memset(&gp, 0, sizeof gp);
    }
}
static void gdiplus_stop(void) {
    if (gp.dll) { gp.shutdown(gp.token); FreeLibrary(gp.dll); memset(&gp, 0, sizeof gp); }
}

// ---- Themes. Dark and Light are flat; the three picture themes put a picture from <exe folder>\themes behind see-through
// panels (sources in themes\CREDITS.txt). "plain" in settings.txt (Plain grey, up to 1.3.20) opens as Light.
enum { TH_LIGHT, TH_DARK, TH_GLASS, TH_ISLAMIC, TH_MONGOL, N_THEMES };
static const char *const g_theme_names[N_THEMES] = {"Light", "Dark", "Stained glass", "Islamic tiles", "Mongol painting"};
static const char *const g_theme_keys[N_THEMES] = {"plain", "dark", "glass", "islamic", "mongol"};
typedef struct {
    int dark;
    unsigned bg, panel, field, line, line2, text, dim, faint, accent, accent_text, sel, play;
    int panel_alpha;   // 255 = solid; less = the picture shows through
    const char *art;
} Pal;
static const Pal g_pals[N_THEMES] = {
    {0, 0xECEAE6, 0xF8F7F4, 0xFFFFFF, 0xDCD9D3, 0xC6C2BA, 0x22211F, 0x5F5D58, 0x8C8982, 0x9A7425, 0x7E5C14, 0xE7E2D7, 0xA8342A, 255, NULL},
    {1, 0x141518, 0x1C1D21, 0x24252A, 0x2E3036, 0x3F414A, 0xE8E5DE, 0xA6A6AC, 0x7A7B82, 0xC9A14A, 0xE2BD68, 0x2B2D35, 0xA8342A, 255, NULL},
    {1, 0x140F18, 0x18141C, 0x241F2A, 0x3A3242, 0x544A60, 0xEEE8DC, 0xB8AFC6, 0x8C8398, 0xC9A14A, 0xE2BD68, 0x3C2D4A, 0xA8342A, 222, "glass.jpg"},
    {1, 0x0A1724, 0x0C1A28, 0x13263A, 0x22405A, 0x30587A, 0xEDE6D8, 0xAEC0D0, 0x7F96AA, 0xD8B35A, 0xE8C673, 0x174560, 0xA8342A, 222, "islamic.jpg"},
    {1, 0x1A120B, 0x24160C, 0x2E1E12, 0x4A3220, 0x634830, 0xEEE4D2, 0xC9B28E, 0x988263, 0xC9A14A, 0xE2BD68, 0x4C2C1A, 0xA8342A, 226, "mongol.jpg"},
};
static int g_theme;
#define PAL (&g_pals[g_theme])
// state colours: ok, warn, bad, info
static unsigned st_col(int which) {
    static const unsigned dark[4] = {0x66BB80, 0xDDA845, 0xE57F72, 0x72ACDF}, light[4] = {0x2F7D4A, 0x92640C, 0xB2453A, 0x2C6EA8};
    return (PAL->dark ? dark : light)[which & 3];
}
enum { ST_OK, ST_WARN, ST_BAD, ST_INFO, ST_MUTE };

static COLORREF cref(unsigned rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }
static UINT32 argb(unsigned rgb, int a) { return ((UINT32)(a & 255) << 24) | (rgb & 0xFFFFFF); }
static unsigned mix(unsigned a, unsigned b, int t) {   // t/255 of b over a
    int r = (int)((a >> 16) & 255) + (((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t) / 255;
    int g = (int)((a >> 8) & 255) + (((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t) / 255;
    int bl = (int)(a & 255) + (((int)(b & 255) - (int)(a & 255)) * t) / 255;
    return (unsigned)(r << 16 | g << 8 | bl);
}

// ---- Scale: every size in this file is in units of a 100% screen (96 dpi) and goes through S().
static float g_scale = 1.0f;
static int S(float v) { return (int)(v * g_scale + (v >= 0 ? 0.5f : -0.5f)); }

// ---- Fonts, made again for each scale.
enum { F_BODY, F_BOLD, F_SMALL, F_HEAD, F_TITLE, F_MONO, F_ICON, F_ICON_S, F_PLAY, F_BIG, F_ICON_L, N_FONTS };
static HFONT g_font[N_FONTS];
static WCHAR g_face[LF_FACESIZE] = L"Segoe UI", g_icon_face[LF_FACESIZE] = L"Segoe MDL2 Assets";

static int face_exists(HDC dc, const WCHAR *face) {
    HFONT f = CreateFontW(-12, 0, 0, 0, 400, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, face), o = (HFONT)SelectObject(dc, f);
    WCHAR got[LF_FACESIZE] = L"";
    GetTextFaceW(dc, LF_FACESIZE, got);
    SelectObject(dc, o);
    DeleteObject(f);
    return !_wcsicmp(got, face);
}
static void fonts_make(void) {
    static int checked;
    if (!checked) {   // Windows 11 faces when they are there
        HDC dc = GetDC(NULL);
        if (face_exists(dc, L"Segoe UI Variable Text")) wcscpy(g_face, L"Segoe UI Variable Text");
        if (face_exists(dc, L"Segoe Fluent Icons")) wcscpy(g_icon_face, L"Segoe Fluent Icons");
        ReleaseDC(NULL, dc);
        checked = 1;
    }
    for (int i = 0; i < N_FONTS; i++) if (g_font[i]) DeleteObject(g_font[i]);
    static const struct { float px; int weight; int which; } spec[N_FONTS] = {
        {13.5f, 400, 0}, {13.5f, 600, 0}, {12, 400, 0}, {11.5f, 600, 0}, {17, 600, 0}, {13.5f, 600, 2}, {14, 400, 1}, {11, 400, 1}, {15.5f, 600, 0}, {15, 600, 0}, {15, 400, 1}};
    for (int i = 0; i < N_FONTS; i++)
        g_font[i] = CreateFontW(-S(spec[i].px), 0, 0, 0, spec[i].weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH, spec[i].which == 1 ? g_icon_face : spec[i].which == 2 ? L"Cascadia Mono" : g_face);
}

// ---- The frame being drawn
static HDC g_dc;            // back buffer
static void *g_g;           // GDI+ graphics on it

static WCHAR *wtext(const char *s, int *len) {   // UTF-8 -> UTF-16 (a few calls at a time share these buffers)
    static WCHAR buf[4][2048];
    static int at;
    WCHAR *w = buf[at++ & 3];
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 2048);
    if (n <= 0) { w[0] = 0; n = 1; }
    if (len) *len = n - 1;
    return w;
}

static void fill_rr(float x, float y, float w, float h, float r, UINT32 c) {
    if (!gp.dll) {   // no GDI+: square corners
        HBRUSH b = CreateSolidBrush(RGB((c >> 16) & 255, (c >> 8) & 255, c & 255));
        RECT q = {(int)x, (int)y, (int)(x + w), (int)(y + h)};
        FillRect(g_dc, &q, b);
        DeleteObject(b);
        return;
    }
    void *p = NULL, *b = NULL;
    gp.new_path(0, &p);
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    if (r < 0.5f) {
        gp.line(p, x, y, x + w, y); gp.line(p, x + w, y, x + w, y + h); gp.line(p, x + w, y + h, x, y + h);
    } else {
        float d = r * 2;
        gp.arc(p, x, y, d, d, 180, 90);
        gp.arc(p, x + w - d, y, d, d, 270, 90);
        gp.arc(p, x + w - d, y + h - d, d, d, 0, 90);
        gp.arc(p, x, y + h - d, d, d, 90, 90);
    }
    gp.close(p);
    gp.solid(c, &b);
    gp.fill_path(g_g, b, p);
    gp.del_brush(b);
    gp.del_path(p);
}
static void stroke_rr(float x, float y, float w, float h, float r, UINT32 c, float width) {
    if (!gp.dll) return;
    void *p = NULL, *pen = NULL;
    x += width / 2; y += width / 2; w -= width; h -= width; r -= width / 2;
    gp.new_path(0, &p);
    if (r * 2 > h) r = h / 2;
    if (r < 0.5f) r = 0.5f;
    float d = r * 2;
    gp.arc(p, x, y, d, d, 180, 90);
    gp.arc(p, x + w - d, y, d, d, 270, 90);
    gp.arc(p, x + w - d, y + h - d, d, d, 0, 90);
    gp.arc(p, x, y + h - d, d, d, 90, 90);
    gp.close(p);
    gp.pen(c, width, 2, &pen);
    gp.draw_path(g_g, pen, p);
    gp.del_pen(pen);
    gp.del_path(p);
}
static void fill_circle(float cx, float cy, float r, UINT32 c) {
    if (!gp.dll) return;
    void *b = NULL;
    gp.solid(c, &b);
    gp.fill_ellipse(g_g, b, cx - r, cy - r, r * 2, r * 2);
    gp.del_brush(b);
}
static void fill_rect(int x, int y, int w, int h, unsigned rgb) {
    HBRUSH b = CreateSolidBrush(cref(rgb));
    RECT q = {x, y, x + w, y + h};
    FillRect(g_dc, &q, b);
    DeleteObject(b);
}
static void check_mark(float x, float y, float s, UINT32 c, float width) {   // a tick inside an s x s box at x, y
    if (!gp.dll) return;
    struct { REAL x, y; } pt[3] = {{x + s * 0.24f, y + s * 0.53f}, {x + s * 0.42f, y + s * 0.71f}, {x + s * 0.77f, y + s * 0.31f}};
    void *pen = NULL;
    gp.pen(c, width, 2, &pen);
    gp.draw_lines(g_g, pen, pt, 3);
    gp.del_pen(pen);
}

// Text in a box. flags: DT_* (DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS are added unless DT_WORDBREAK is given).
static void text(int x, int y, int w, int h, const char *s, int font, unsigned rgb, UINT flags) {
    int n;
    WCHAR *t = wtext(s, &n);
    RECT r = {x, y, x + w, y + h};
    HFONT o = (HFONT)SelectObject(g_dc, g_font[font]);
    SetTextColor(g_dc, cref(rgb));
    SetBkMode(g_dc, TRANSPARENT);
    if (!(flags & DT_WORDBREAK)) flags |= DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
    DrawTextW(g_dc, t, n, &r, flags | DT_NOPREFIX);
    SelectObject(g_dc, o);
}
static int text_w(const char *s, int font) {
    int n;
    WCHAR *t = wtext(s, &n);
    SIZE z = {0, 0};
    HDC dc = g_dc ? g_dc : GetDC(NULL);
    HFONT o = (HFONT)SelectObject(dc, g_font[font]);
    GetTextExtentPoint32W(dc, t, n, &z);
    SelectObject(dc, o);
    if (!g_dc) ReleaseDC(NULL, dc);
    return z.cx;
}
static void icon(int x, int y, int w, int h, WCHAR glyph, int font, unsigned rgb) {
    WCHAR t[2] = {glyph, 0};
    RECT r = {x, y, x + w, y + h};
    HFONT o = (HFONT)SelectObject(g_dc, g_font[font]);
    SetTextColor(g_dc, cref(rgb));
    SetBkMode(g_dc, TRANSPARENT);
    DrawTextW(g_dc, t, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(g_dc, o);
}
// icon glyphs (Segoe Fluent Icons / Segoe MDL2 Assets share these)
enum { IC_SEARCH = 0xE721, IC_DOWN = 0xE70D, IC_UP = 0xE70E, IC_PLAY = 0xE768, IC_DOWNLOAD = 0xE896, IC_UPLOAD = 0xE898,
       IC_COPY = 0xE8C8, IC_FOLDER = 0xED25, IC_GLOBE = 0xE774, IC_CLOSE = 0xE711, IC_WARN = 0xE7BA, IC_INFO = 0xE946,
       IC_SAVE = 0xE74E, IC_DELETE = 0xE74D, IC_ADD = 0xE710, IC_UNDO = 0xE7A7, IC_CHECK = 0xE73E, IC_REFRESH = 0xE72C };

// A picture drawn to fill a box (the rest cropped), with rounded corners.
static void picture_cover(void *img, float x, float y, float w, float h, float r) {
    UINT iw = 0, ih = 0;
    if (!img || gp.width(img, &iw) || gp.height(img, &ih) || !iw || !ih) return;
    void *p = NULL;
    gp.new_path(0, &p);
    float d = r * 2;
    gp.arc(p, x, y, d, d, 180, 90); gp.arc(p, x + w - d, y, d, d, 270, 90);
    gp.arc(p, x + w - d, y + h - d, d, d, 0, 90); gp.arc(p, x, y + h - d, d, d, 90, 90);
    gp.close(p);
    gp.clip_path(g_g, p, 0);
    double sc = w / iw > h / ih ? w / iw : h / ih;
    int dw = (int)(iw * sc + 1), dh = (int)(ih * sc + 1);
    gp.interp(g_g, 7);
    gp.draw(g_g, img, (int)(x + (w - dw) / 2), (int)(y + (h - dh) / 2), dw, dh);
    gp.reset_clip(g_g);
    gp.del_path(p);
}
