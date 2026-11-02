// F11 panel for the frame bridge (dev only; AGENT_LOG 101, look AGENT_LOG 104): DLSS Super Resolution mode, frame
// generation (off / 2x / 3x / 4x), per-object motion vectors, and a readout of the game's and the shown frame rate.
// Drawn on the CPU into a bitmap only when something on it changes (anti-aliased rounded shapes computed here, text by
// GDI), uploaded to a texture, and blended over the back buffer at Present while the panel is open (one small quad; with
// the panel closed nothing is drawn), so frame generation treats it as UI (it is not in the HUD-less image). Clicks are
// read from the mouse state each Present. The game reads its mouse through DirectInput 8 (empire.retail.dll imports
// DirectInput8Create; this DLL is its dinput8.dll): while the panel is open and the cursor is over it, the game's mouse
// device reports no buttons and no wheel.
// SR modes below native size (Quality, Balanced, Performance, Ultra Performance): the game renders its 3D scene into the
// top-left of its targets at the preset's size (bridgeprobe.c, AGENT_LOG 103).
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

UINT fgb_render_w(void), fgb_render_h(void);   // fgbridge.c
unsigned fgb_fg_max(void);
unsigned fgb_sr_backend(void), fgb_fg_backend(void);   // fgbridge.c: 0 none, 1 NVIDIA, 2 AMD FSR
#define SR_AMD (fgb_sr_backend() == 2)   // AMD FSR upscaling / frame generation runs: AMD's names, no DLSS-only rows
#define FG_AMD (fgb_fg_backend() == 2)

typedef struct { void (*sr)(int mode); void (*fg)(int frames); void (*motion)(int on); void (*cap)(int choice); void (*preset)(int choice); void (*sharp)(int choice); void (*view)(int choice); void (*vsync)(int on); void (*fps)(int on); void (*tech)(int sr, int fg); } FgpApply;

static ID3D11Device *p_dev;
static ID3D11DeviceContext *p_ctx;
static ID3D11Texture2D *p_tex;
static ID3D11ShaderResourceView *p_srv;
static HDC p_dc;
static HBITMAP p_bmp;
static unsigned char *p_bits;   // BGRA, top-down: the panel's colours (alpha comes from the shapes, below)
static unsigned char *p_rgba;   // the upload: RGBA, premultiplied alpha
static unsigned char *p_mask;   // per pixel (fixed, made once): card coverage, alpha with the shadow, edge highlight
static HFONT p_title, p_label, p_small, p_tiny;
static int p_w, p_h, p_x, p_y, p_open, p_dirty = 1, p_ok, p_hover = -1;
static float p_u;               // 1 = 1080p
static float p_cx0, p_cy0, p_cx1, p_cy1, p_cr;   // the card (rounded rectangle) inside the texture; outside: its shadow
static UINT p_bw, p_bh;
static void make_mask(void);
static float p_cy1_full;   // the card's bottom with every row (fgp_init); p_cy1 is less while a row is hidden
static int p_sr, p_fg, p_motion;   // current choices: SR 0 off / 1 DLAA / 2..5 Quality..Ultra Performance; FG 0 / 1 / 2 / 3 generated frames; motion 0 / 1, -1 not available, -2 no frame generation on this GPU
static FgpApply p_apply;
static volatile int p_block;       // the game's mouse buttons are hidden (panel open, cursor over it)
static double p_game_fps, p_shown_fps;
static const wchar_t *p_note;      // shown instead of the frame rates (fgp_set_note): why DLSS is off
static int p_preset;               // the DLSS preset (row 4): 0 NVIDIA's default per mode, 1..4 J / K / L / M
static int p_presets_ok = 1;       // the add-on's helper applies presets (an older one: the row greyed out, a note)
static int p_sharp = 2;            // sharpening of the DLSS output (row 5): 0 off, 1..3 low / medium / high
void fgp_set_preset(int choice) { p_preset = choice; p_dirty = 1; }
void fgp_set_presets_available(int on) { p_presets_ok = on; p_dirty = 1; }
void fgp_set_sharp(int choice) { p_sharp = choice; p_dirty = 1; }
static int p_vsync = 1, p_vsync_ok = 1;   // V-Sync of the shown frames (row 7): 0 off, 1 on; p_vsync_ok: the helper applies it
void fgp_set_vsync(int on) { p_vsync = on != 0; p_dirty = 1; }
void fgp_set_vsync_available(int on) { p_vsync_ok = on; p_dirty = 1; }
static int p_vsync_driver;   // NVIDIA's driver setting (FgbShared::driver_vsync): 2 / 3 / 4 = forced on / off / Fast Sync, the row greyed
void fgp_set_vsync_driver(int v) { p_vsync_driver = v; p_dirty = 1; }
static int p_fpson;   // the frame-rate counter (row 8): 0 off, 1..4 top left / top right / bottom left / bottom right
static int p_cdirty = 1;
// the key that opens / closes the panel (settings.txt panel_key=, default F8: Mat's 4K UI Scale 2.1.1+ holds F11 for its
// compare picture, and both mods read the key directly, 2026-11-02)
static int p_key = VK_F8;
static wchar_t p_keyhint[32] = L"F8 to close";
void fgp_set_key(int vk, const wchar_t *name) { p_key = vk; _snwprintf(p_keyhint, 32, L"%ls to close", name); p_dirty = 1; }
void fgp_set_fps_counter(int corner) { p_fpson = corner >= 0 && corner < 5 ? corner : 0; p_dirty = p_cdirty = 1; }
void fgp_set_fg(int fg) { p_fg = fg; p_dirty = 1; }   // after an F11 switch: what the new helper allows
static int p_tsr, p_tfg;   // the picked upscaler / frame generation (rows 9 / 10): 0 NVIDIA DLSS, 1 AMD FSR; from the next start
void fgp_set_tech(int sr, int fg) { p_tsr = sr; p_tfg = fg; p_dirty = 1; }
static int p_drop = -1;   // the open drop-down list (its row), -1 none
void fgb_gpu_caps(int *dlss_sr, int *dlss_fg);   // fgbridge.c
const wchar_t *fgb_gpu_name(void);
static unsigned p_hz;   // the refresh rate of the game's screen (read once, fgp_frame)
static int p_view;                 // Debug View (row 6, test builds: shown when p_apply.view is set): 0 off, 1 no mip bias, 2 no bloom fix

typedef struct { RECT r; int row, value, enabled; } Btn;
static Btn p_btn[64];
static int p_nbtn;

// ---- the game's mouse through DirectInput 8: buttons and wheel hidden while p_block ----
static void *p_mouse;   // the game's mouse device
static HRESULT(STDMETHODCALLTYPE *r_cdev)(void *, const GUID *, void **, void *);
static HRESULT(STDMETHODCALLTYPE *r_state)(void *, DWORD, void *);
static HRESULT(STDMETHODCALLTYPE *r_data)(void *, DWORD, void *, DWORD *, DWORD);
static const GUID guid_sysmouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

static void patch(void **slot, void *to, void **old) {
    DWORD prot;
    if (*slot == to || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &prot)) return;
    *old = *slot;
    *slot = to;
    VirtualProtect(slot, sizeof *slot, prot, &prot);
}
static HRESULT STDMETHODCALLTYPE w_state(void *d, DWORD cb, void *data) {   // GetDeviceState: DIMOUSESTATE(2)
    HRESULT hr = r_state(d, cb, data);
    if (SUCCEEDED(hr) && d == p_mouse && p_block && data && cb >= 16) {
        ((LONG *)data)[2] = 0;                       // lZ (wheel)
        memset((unsigned char *)data + 12, 0, cb - 12);   // rgbButtons
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE w_data(void *d, DWORD cbo, void *rg, DWORD *n, DWORD fl) {   // GetDeviceData (buffered)
    HRESULT hr = r_data(d, cbo, rg, n, fl);
    if (SUCCEEDED(hr) && d == p_mouse && p_block && rg && n && cbo >= 8) {
        DWORD k = 0;
        for (DWORD i = 0; i < *n; i++) {
            const unsigned char *e = (const unsigned char *)rg + i * cbo;
            DWORD ofs = *(const DWORD *)e;
            if (ofs == 8 || (ofs >= 12 && ofs < 20)) continue;   // DIMOFS_Z, DIMOFS_BUTTON0..7
            if (k != i) memmove((unsigned char *)rg + k * cbo, e, cbo);
            k++;
        }
        *n = k;
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE w_cdev(void *di, const GUID *g, void **dev, void *outer) {   // IDirectInput8::CreateDevice
    HRESULT hr = r_cdev(di, g, dev, outer);
    if (SUCCEEDED(hr) && dev && *dev && g && !memcmp(g, &guid_sysmouse, sizeof *g)) {
        p_mouse = *dev;
        void **vt = *(void ***)*dev;
        patch(&vt[9], (void *)w_state, (void **)&r_state);
        patch(&vt[10], (void *)w_data, (void **)&r_data);
    }
    return hr;
}
void fgp_di8_created(void *di) {   // from the dinput8.dll export (heapfix.c, dev builds)
    void **vt = *(void ***)di;
    patch(&vt[3], (void *)w_cdev, (void **)&r_cdev);
}
int fgp_mouse_found(void) { return p_mouse != NULL; }

// ---- drawing: anti-aliased rectangles (coverage from the signed distance) blended into the colour bitmap ----
// Look (AGENT_LOG 104), read as: an in-battle settings overlay for an Attila player, in the game's own register (dark
// panels, bronze trim, classical capitals), dial ENERGY 1 / RHYTHM 1 / MOTION 1. Reasons, one per technique:
// - dark: it sits over a battlefield and next to the game's dark UI;
// - bronze: the one accent, on the current choice only (and the hairline frame, the game's trim colour), so the eye finds
//   what is set at a glance;
// - serif capitals (Palatino Linotype, in Windows) for the heading and row names: the game's classical voice; numbers and
//   captions in Segoe UI for legibility;
// - shadow: the one raised surface, lifted off the scene behind it; 3 px corners, one radius for everything;
// - no motion: it changes only when clicked.
#define RGBC(r, g, b) ((unsigned)(r) << 16 | (unsigned)(g) << 8 | (unsigned)(b))
static const unsigned C_TOP = RGBC(27, 25, 22), C_BOTTOM = RGBC(17, 16, 15);   // panel: warm charcoal, darker at the bottom
static const unsigned C_TRACK = RGBC(33, 31, 28), C_HOVER = RGBC(52, 48, 42), C_BRONZE = RGBC(186, 146, 84);
static const unsigned C_TEXT = RGBC(232, 225, 212), C_DIM = RGBC(158, 149, 134), C_ON_BRONZE = RGBC(26, 19, 9), C_OFF = RGBC(92, 87, 80);
static const unsigned C_RULE = RGBC(66, 56, 42);

static float rr_dist(float px, float py, float x0, float y0, float x1, float y1, float r) {   // < 0 inside
    float cx = px < x0 + r ? x0 + r : px > x1 - r ? x1 - r : px;
    float cy = py < y0 + r ? y0 + r : py > y1 - r ? y1 - r : py;
    float dx = px - cx, dy = py - cy;
    return sqrtf(dx * dx + dy * dy) - r;
}
static void fill_rr(float x0, float y0, float x1, float y1, float r, unsigned c, float opacity) {
    int ix0 = (int)floorf(x0) - 1, iy0 = (int)floorf(y0) - 1, ix1 = (int)ceilf(x1) + 1, iy1 = (int)ceilf(y1) + 1;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > p_w) ix1 = p_w;
    if (iy1 > p_h) iy1 = p_h;
    float cr = (float)(c >> 16 & 255), cg = (float)(c >> 8 & 255), cb = (float)(c & 255);
    for (int y = iy0; y < iy1; y++)
        for (int x = ix0; x < ix1; x++) {
            float a = 0.5f - rr_dist(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r);
            if (a <= 0) continue;
            a = (a > 1 ? 1 : a) * opacity;
            unsigned char *p = p_bits + ((size_t)y * p_w + x) * 4;
            p[0] = (unsigned char)(p[0] + (cb - p[0]) * a + 0.5f);
            p[1] = (unsigned char)(p[1] + (cg - p[1]) * a + 0.5f);
            p[2] = (unsigned char)(p[2] + (cr - p[2]) * a + 0.5f);
        }
}
static void text(const wchar_t *s, HFONT f, unsigned c, int x0, int y0, int x1, int y1, UINT align) {
    RECT r = {x0, y0, x1, y1};
    SelectObject(p_dc, f);
    SetTextColor(p_dc, RGB(c >> 16 & 255, c >> 8 & 255, c & 255));
    DrawTextW(p_dc, s, -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// one row of choices: a flat dark strip, thin dividers between the options, the current one in bronze; options past the
// first nen (not available this session or on this GPU) are drawn greyed out and take no clicks
static void choices(int row, float x0, float y0, float x1, float y1, const wchar_t *const *names, int n, int chosen, int nen) {
    float u = p_u, r = 3 * u;
    fill_rr(x0, y0, x1, y1, r, C_TRACK, 1);
    float sw = (x1 - x0) / n;
    for (int k = 0; k < n; k++) {
        float sx0 = x0 + k * sw, sx1 = sx0 + sw;
        int enabled = k < nen;
        int hot = enabled && p_nbtn == p_hover, sel = enabled && k == chosen;
        if (sel) fill_rr(sx0 + 2 * u, y0 + 2 * u, sx1 - 2 * u, y1 - 2 * u, r, C_BRONZE, 1);
        else if (hot) fill_rr(sx0 + 2 * u, y0 + 2 * u, sx1 - 2 * u, y1 - 2 * u, r, C_HOVER, 1);
        if (k && k != chosen && k != chosen + 1) fill_rr(sx0 - 0.5f * u, y0 + 9 * u, sx0 + 0.5f * u, y1 - 9 * u, 0, C_RULE, 1);
        text(names[k], p_small, !enabled ? C_OFF : sel ? C_ON_BRONZE : hot ? C_TEXT : C_DIM, (int)sx0, (int)y0, (int)sx1, (int)y1, DT_CENTER);
        if (p_nbtn < 64) {
            RECT rc = {(LONG)sx0, (LONG)y0, (LONG)sx1, (LONG)y1};
            p_btn[p_nbtn].r = rc; p_btn[p_nbtn].row = row; p_btn[p_nbtn].value = k; p_btn[p_nbtn].enabled = enabled;
            p_nbtn++;
        }
    }
}

static const wchar_t *const sr_names[6] = {L"Off", L"DLAA", L"Quality", L"Balanced", L"Performance", L"Ultra Perf."};
static const wchar_t *const sr_names_fsr[6] = {L"Off", L"Native AA", L"Quality", L"Balanced", L"Performance", L"Ultra Perf."};
static const wchar_t *const fg_names[4] = {L"Off", L"2x", L"3x", L"4x"};
static const wchar_t *const mv_names[2] = {L"Full", L"Fast"};   // per_object / camera_only: shown Full first, so slot = 1 - p_motion
static const wchar_t *const cap_names[5] = {L"Off", L"Auto", L"60", L"90", L"120"};
static const wchar_t *const preset_names[5] = {L"Default", L"J", L"K", L"L", L"M"};
static const wchar_t *const sharp_names[4] = {L"Off", L"Low", L"Medium", L"High"};
static const wchar_t *const vsync_names[2] = {L"Off", L"On"};
#define VIEW_N 3
static const wchar_t *const view_names[VIEW_N] = {L"Off", L"No mip bias", L"No bloom fix"};
// the frame cap (row 3): the choice (0 off, 1 auto, 2.. 60 / 90 / 120) and the rendered-frame cap in effect (0 none)
static int p_cap = 1;
static unsigned p_cap_fps;
void fgp_set_cap(int choice, unsigned fps) { p_cap = choice; p_cap_fps = fps; p_dirty = 1; }


static void add_btn(float x0, float y0, float x1, float y1, int row, int value, int enabled) {
    if (p_nbtn >= 64) return;
    RECT rc = {(LONG)x0, (LONG)y0, (LONG)x1, (LONG)y1};
    p_btn[p_nbtn].r = rc; p_btn[p_nbtn].row = row; p_btn[p_nbtn].value = value; p_btn[p_nbtn].enabled = enabled;
    p_nbtn++;
}
// a drop-down: the box shows the pick (value -1 opens / closes it); its list is drawn after every row (drop_list), so it
// lies over the rows below and takes the clicks there (the hover search goes from the last button back)
static struct { int row, n, chosen, allowed; float x0, y0, x1, y1; const wchar_t *const *names; } p_dl;
static void dropdown(int row, float x0, float y0, float x1, float y1, const wchar_t *const *names, int n, int chosen, int allowed) {
    float u = p_u, r = 3 * u;
    int hot = p_nbtn == p_hover, open = p_drop == row;
    fill_rr(x0, y0, x1, y1, r, open || hot ? C_HOVER : C_TRACK, 1);
    text(names[chosen], p_small, C_TEXT, (int)(x0 + 12 * u), (int)y0, (int)(x1 - 30 * u), (int)y1, DT_LEFT);
    text(open ? L"\x25B4" : L"\x25BE", p_small, C_BRONZE, (int)(x1 - 28 * u), (int)y0, (int)(x1 - 8 * u), (int)y1, DT_CENTER);
    add_btn(x0, y0, x1, y1, row, -1, 1);
    if (open) { p_dl.row = row; p_dl.n = n; p_dl.chosen = chosen; p_dl.allowed = allowed; p_dl.names = names;
                p_dl.x0 = x0; p_dl.y0 = y1 + 2 * u; p_dl.x1 = x1; p_dl.y1 = y1; }
}
static void drop_list(void) {
    if (p_drop < 0 || p_dl.row != p_drop) return;
    float u = p_u, ih = 34 * u, y = p_dl.y0;
    fill_rr(p_dl.x0 - 1 * u, y - 1 * u, p_dl.x1 + 1 * u, y + ih * p_dl.n + 1 * u, 3 * u, C_RULE, 1);
    for (int k = 0; k < p_dl.n; k++, y += ih) {
        int enabled = p_dl.allowed >> k & 1, hot = enabled && p_nbtn == p_hover, sel = k == p_dl.chosen;
        fill_rr(p_dl.x0, y, p_dl.x1, y + ih, 2 * u, sel ? C_BRONZE : hot ? C_HOVER : C_TRACK, 1);
        text(p_dl.names[k], p_small, sel ? C_ON_BRONZE : !enabled ? C_OFF : C_TEXT, (int)(p_dl.x0 + 12 * u), (int)y, (int)p_dl.x1, (int)(y + ih), DT_LEFT);
        add_btn(p_dl.x0, y, p_dl.x1, y + ih, p_dl.row, k, enabled);
    }
}
static const wchar_t *const fps_names[5] = {L"Off", L"Top left", L"Top right", L"Bottom left", L"Bottom right"};
static const wchar_t *const tsr_names[2] = {L"NVIDIA DLSS", L"AMD FSR"};
static const wchar_t *const tfg_names[2] = {L"NVIDIA DLSS Frame Generation", L"AMD FSR Frame Generation"};
// what this PC should use (always shown; the Defaults button sets it), from the GPU, the screen size and its refresh rate:
// NVIDIA's parts where the GPU has them, AMD FSR for the rest; Attila is mostly limited by the CPU, so a strong GPU gets the
// best image (DLAA / Native AA up to 1440p, Quality at 4K) and a weaker one more upscaling; frame generation 2x, never more
// (user 2026-11-02 via Rome 2: 3x / 4x stay the player's choice); preset NVIDIA's default, sharpness Medium with DLSS and
// High with AMD FSR (AGENT_LOG 194's sweep: FSR Quality at Medium kept 85 % of native detail, High 105 %), V-Sync On
typedef struct { int sr_amd, fg_amd, sr, fg; } Rec;
static int gpu_class(void) {   // 2 strong, 1 middle, 0 entry, from the name: "RTX 5090", "RX 7900 XTX", "RX 9070"
    const wchar_t *g = fgb_gpu_name(), *r = wcsstr(g, L"RTX "), *a = wcsstr(g, L"RX ");
    if (r) {
        unsigned n = wcstoul(r + 4, NULL, 10), series = n / 100, tier = n % 100;
        return tier >= 80 || (tier >= 70 && series >= 40) ? 2 : tier >= 60 ? 1 : 0;
    }
    if (a) {
        unsigned n = wcstoul(a + 3, NULL, 10), t = n % 1000;
        if (t < 100) t *= 10;   // 9070: tier 70
        return t >= 800 || (t >= 700 && n >= 9000) ? 2 : t >= 600 ? 1 : 0;
    }
    return 1;
}
static Rec recommended(void) {
    int dsr, dfg;
    fgb_gpu_caps(&dsr, &dfg);
    Rec r;
    r.sr_amd = !dsr; r.fg_amd = !(dsr && dfg);
    int fg_ok = 1;
    int c = gpu_class();
    r.sr = p_bh >= 2000 ? 4 - c : p_bh >= 1300 ? (c == 2 ? 1 : 3 - c) : (c ? 1 : 2);   // 4K: Quality / Balanced / Performance
    r.fg = fg_ok;   // 2x
    return r;
}
static void redraw(void) {
    float u = p_u;
    float cy1 = p_cy1_full - (SR_AMD ? 58 * u : 0);   // the DLSS preset row is not shown while AMD FSR upscales
    if (cy1 != p_cy1) { p_cy1 = cy1; make_mask(); }
    // the panel: vertical gradient (the shadow area outside it is cut by the alpha below)
    for (int y = 0; y < p_h; y++) {
        float t = (y - p_cy0) / (p_cy1 - p_cy0);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        unsigned r = (unsigned)((C_TOP >> 16 & 255) + (((int)(C_BOTTOM >> 16 & 255)) - (int)(C_TOP >> 16 & 255)) * t);
        unsigned g = (unsigned)((C_TOP >> 8 & 255) + (((int)(C_BOTTOM >> 8 & 255)) - (int)(C_TOP >> 8 & 255)) * t);
        unsigned b = (unsigned)((C_TOP & 255) + (((int)(C_BOTTOM & 255)) - (int)(C_TOP & 255)) * t);
        unsigned *row = (unsigned *)(p_bits + (size_t)y * p_w * 4);
        for (int x = 0; x < p_w; x++) row[x] = r << 16 | g << 8 | b;
    }
    SetBkMode(p_dc, TRANSPARENT);
    p_nbtn = 0;
    float x0 = p_cx0 + 26 * u, x1 = p_cx1 - 26 * u, y = p_cy0 + 16 * u;
    // heading, and how to close it
    text(SR_AMD ? L"AMD FSR" : FG_AMD ? L"DLSS + AMD FSR" : L"DLSS", p_title, C_TEXT, (int)x0, (int)y, (int)(x0 + (FG_AMD ? 300 : 200) * u), (int)(y + 36 * u), DT_LEFT);
    text(p_keyhint, p_tiny, C_DIM, (int)(x1 - 98 * u), (int)y, (int)x1, (int)(y + 36 * u), DT_RIGHT);
    {   // Defaults: everything back to the recommendation below (the frame-rate counter stays as it is)
        float bx0 = x1 - 200 * u, bx1 = x1 - 100 * u, by0 = y + 5 * u, by1 = y + 31 * u;
        int hot = p_nbtn == p_hover;
        fill_rr(bx0, by0, bx1, by1, 3 * u, hot ? C_HOVER : C_TRACK, 1);
        text(L"Defaults", p_small, hot ? C_TEXT : C_DIM, (int)bx0, (int)by0, (int)bx1, (int)by1, DT_CENTER);
        add_btn(bx0, by0, bx1, by1, 20, 0, 1);
    }
    y += 44 * u;
    fill_rr(x0, y, x1, y + 1 * u, 0, C_RULE, 1);
    y += 18 * u;
    // the settings: name and what it does now on the left, the choices on the right
    wchar_t cap[11][64];
    _snwprintf(cap[0], 64, SR_AMD ? L"AMD FSR, render %u \x00d7 %u" : L"Render %u \x00d7 %u", fgb_render_w(), fgb_render_h());
    unsigned fgmax = fgb_fg_max();   // NVIDIA's rules for this GPU: frame generation on RTX 40 / 50, 3x and 4x on RTX 50
    if (!fgmax) _snwprintf(cap[1], 64, L"Needs an RTX 40 or 50 series GPU");
    else if (FG_AMD) _snwprintf(cap[1], 64, p_fg ? L"AMD FSR, 1 generated per rendered frame" : L"AMD FSR Frame Generation, 2x");
    else if (p_fg) _snwprintf(cap[1], 64, L"%d generated per rendered frame", p_fg);
    else _snwprintf(cap[1], 64, fgmax == 1 ? L"3x and 4x need an RTX 50 series GPU" : L"Every frame rendered");
    _snwprintf(cap[2], 64, p_motion == -2 ? L"Needs frame generation" : p_motion < 0 ? L"Not available this session"
               : p_motion ? L"Soldiers, horses, trees and grass" : L"Camera only: for weak CPUs");
    if (p_cap_fps) _snwprintf(cap[3], 64, L"%u rendered, %u shown fps", p_cap_fps, p_cap_fps * (p_fg + 1));
    else _snwprintf(cap[3], 64, p_cap == 1 ? L"Screen rate with frame generation" : L"No cap");
    static const wchar_t *const preset_caps[5] = {L"", L"Less ghosting, more flicker", L"Best image for DLAA / Quality",
                                                  L"Sharpest, the slowest", L"Sharp, faster than L"};
    // Default: NVIDIA's documented preset for the mode (Streamline 2.14.16 reports none in use, and the NVIDIA app's override
    // can change it): DLAA / Quality / Balanced K, Performance M, Ultra Performance L
    if (SR_AMD) _snwprintf(cap[4], 64, L"NVIDIA DLSS only");
    else if (!p_presets_ok) _snwprintf(cap[4], 64, L"Update the DLSS add-on for this");
    else if (!p_sr) _snwprintf(cap[4], 64, L"Needs Super Resolution or DLAA");
    else if (!p_preset) _snwprintf(cap[4], 64, L"NVIDIA's pick here: %lc", L"-KKKML"[p_sr < 6 ? p_sr : 0]);
    else _snwprintf(cap[4], 64, L"%ls", preset_caps[p_preset]);
    if (SR_AMD) _snwprintf(cap[5], 64, !p_sr ? L"Needs upscaling or Native AA" : p_sharp ? L"Crisper FSR output" : L"FSR output as it is");
    else _snwprintf(cap[5], 64, !p_sr ? L"Needs Super Resolution or DLAA" : p_sharp ? L"Crisper DLSS output" : L"DLSS output as it is");
    _snwprintf(cap[6], 64, L"Test build only, not saved");
    _snwprintf(cap[7], 64, p_vsync_driver == 2 ? L"Forced On in NVIDIA's settings" : p_vsync_driver == 3 ? L"Forced Off in NVIDIA's settings"
               : p_vsync_driver == 4 ? L"Fast Sync in NVIDIA's settings" : !p_vsync_ok ? L"Update the DLSS add-on for this"
               : p_vsync ? L"No tearing, up to the screen's rate"
                                                                                     : L"No cap, the picture may tear");
    static const wchar_t *const label[11] = {L"Super Resolution", L"Frame Generation", L"Motion Vectors", L"Frame Cap", L"DLSS Preset",
                                             L"Sharpness", L"Debug View", L"V-Sync", L"FPS Counter", L"Upscaler", L"Frame Gen Tech"};
    _snwprintf(cap[8], 64, p_fpson ? L"Rendered / shown frames per second" : L"Hidden");
    int dsr, dfg;
    fgb_gpu_caps(&dsr, &dfg);
    int run_sr = SR_AMD ? 1 : 0, run_fg = FG_AMD ? 1 : 0;
    _snwprintf(cap[9], 64, p_tsr != run_sr ? L"From the next game start" : dsr ? L"Running now" : L"DLSS needs an NVIDIA RTX GPU");
    _snwprintf(cap[10], 64, p_tfg != run_fg ? L"From the next game start" : dfg ? L"Running now" : L"DLSS needs an RTX 40 or 50 series GPU");
    // the preset and sharpness right under Super Resolution, V-Sync under Frame Cap (rows keep their ids)
    static const int order[11] = {9, 10, 0, 4, 5, 1, 2, 3, 7, 8, 6};
    const int nrows = 10;
    float lab = x0 + 196 * u, rh = 38 * u;
    p_dl.row = -1;
    for (int i = 0; i < nrows + (p_apply.view ? 1 : 0); i++) {
        int row = order[i];
        if (row == 4 && SR_AMD) continue;
        float c0 = row && row != 3 && row != 8 && row < 9 ? lab + (x1 - lab) * 0.30f : lab;   // the shorter rows leave their caption more room
        text(row == 0 && SR_AMD ? L"Upscaling" : label[row], p_label, C_TEXT, (int)x0, (int)(y - 3 * u), (int)c0, (int)(y + rh * 0.55f), DT_LEFT);
        text(cap[row], p_tiny, C_DIM, (int)x0, (int)(y + rh * 0.5f), (int)(c0 - 10 * u), (int)(y + rh + 3 * u), DT_LEFT | DT_END_ELLIPSIS);
        if (row == 0) choices(0, c0, y, x1, y + rh, SR_AMD ? sr_names_fsr : sr_names, 6, p_sr, 6);
        else if (row == 1) choices(1, c0, y, x1, y + rh, fg_names, 4, p_fg, (int)fgmax + 1);
        else if (row == 3) choices(3, c0, y, x1, y + rh, cap_names, 5, p_cap, 5);
        else if (row == 4) choices(4, c0, y, x1, y + rh, preset_names, 5, p_preset, p_sr && p_presets_ok && !SR_AMD ? 5 : 0);
        else if (row == 5) choices(5, c0, y, x1, y + rh, sharp_names, 4, p_sharp, p_sr ? 4 : 0);
        else if (row == 6) choices(6, c0, y, x1, y + rh, view_names, VIEW_N, p_view, p_sr ? VIEW_N : 0);
        else if (row == 7) choices(7, c0, y, x1, y + rh, vsync_names, 2, p_vsync, p_vsync_ok && p_vsync_driver < 2 ? 2 : 0);
        else if (row == 8) choices(8, c0, y, x1, y + rh, fps_names, 5, p_fpson, 5);
        else if (row == 9) dropdown(9, c0 + 40 * u, y, x1, y + rh, tsr_names, 2, p_tsr, (dsr ? 1 : 0) | 2);
        else if (row == 10) dropdown(10, c0 + 40 * u, y, x1, y + rh, tfg_names, 2, p_tfg, (dsr && dfg && !p_tsr ? 1 : 0) | 2);
        else {
            choices(2, c0, y, x1, y + rh, mv_names, 2, 1 - p_motion, p_motion >= 0 ? 2 : 0);
            y += rh + 8 * u;   // per-object motion's cost, under its row
            text(L"Full tracks what moves on its own: no ghosting on soldiers and swaying trees. Fast helps weaker CPUs in very big battles.",
                 p_tiny, C_DIM, (int)x0, (int)y, (int)x1, (int)(y + 18 * u), DT_LEFT | DT_END_ELLIPSIS);
            y += 18 * u - rh - 8 * u;
        }
        y += rh + 20 * u;
    }
    {   // the recommendation for this PC, always shown: the settings (from the GPU, the screen and its refresh rate), the model
        Rec r = recommended();
        const wchar_t *g = fgb_gpu_name(), *gs = wcsstr(g, L"RTX ");
        if (!gs) gs = wcsstr(g, L"RX ");
        if (!gs) gs = wcsstr(g, L"Arc ");
        wchar_t rec[200], hz[16] = L"", mdl[200];
        if (p_hz) _snwprintf(hz, 16, L", %u Hz", p_hz);
        _snwprintf(rec, 200, L"Recommended for %ls at %u \x00d7 %u%ls: %ls %ls%ls%ls%ls", gs ? gs : g, p_bw, p_bh, hz,
                   r.sr_amd ? L"AMD FSR" : L"NVIDIA DLSS", (r.sr_amd ? sr_names_fsr : sr_names)[r.sr], r.fg ? L", " : L"",
                   !r.fg ? L"" : r.fg_amd ? L"AMD FSR Frame Generation " : L"DLSS Frame Generation ", r.fg ? fg_names[r.fg] : L"");
        // line 2: the settings for the model picked in rows 9 / 10 (AMD FSR upscaling needs each soldier's own motion: its
        // reactive mask marks moving units only from per-object vectors, AGENT_LOG 163 / 171)
        int msr = p_tsr, mfg = p_tfg;
        if (msr) _snwprintf(mdl, 200, L"With AMD FSR: motion vectors Full, sharpness High, V-Sync On");
        else _snwprintf(mdl, 200, L"With NVIDIA DLSS%ls: preset Default (%lc), motion vectors Full, sharpness Medium, V-Sync On",
                        mfg ? L" and AMD FSR Frame Generation" : L"", L"-KKKML"[p_sr > 0 && p_sr < 6 ? p_sr : 2]);
        if (p_tsr != (SR_AMD ? 1 : 0) || p_tfg != (FG_AMD ? 1 : 0))   // a pick waiting for the restart: said instead, louder
            _snwprintf(rec, 200, L"Restart the game to switch to %ls upscaling with %ls frame generation", p_tsr ? L"AMD FSR" : L"NVIDIA DLSS",
                       p_tfg ? L"AMD FSR" : L"NVIDIA DLSS");
        text(rec, p_tiny, C_BRONZE, (int)x0, (int)(y - 8 * u), (int)x1, (int)(y + 14 * u), DT_LEFT | DT_END_ELLIPSIS);
        y += 22 * u;
        text(mdl, p_tiny, C_BRONZE, (int)x0, (int)(y - 8 * u), (int)x1, (int)(y + 14 * u), DT_LEFT | DT_END_ELLIPSIS);
        y += 22 * u;
    }
    y -= 4 * u;
    fill_rr(x0, y, x1, y + 1 * u, 0, C_RULE, 1);
    y += 10 * u;
    // the readout: the game's frame rate and the shown one (with generated frames), and when frame generation runs
    wchar_t v[96];
    _snwprintf(v, 96, L"Game %.0f fps    Shown %.0f fps", p_game_fps, p_shown_fps);
    if (p_note) text(p_note, p_small, C_BRONZE, (int)x0, (int)y, (int)x1, (int)(y + 26 * u), DT_LEFT | DT_END_ELLIPSIS);
    else text(v, p_small, C_TEXT, (int)x0, (int)y, (int)(x0 + 300 * u), (int)(y + 26 * u), DT_LEFT);
    if (fgmax && !p_note && !FG_AMD) text(L"Frame generation needs the game window in focus", p_tiny, C_DIM, (int)(x0 + 300 * u), (int)y, (int)x1, (int)(y + 26 * u),
         DT_RIGHT | DT_END_ELLIPSIS);
    drop_list();   // over the rows below it
    GdiFlush();
    // the colours through the fixed mask: premultiplied by the card's alpha, the edge highlight, the shadow's alpha
    for (size_t i = 0, n = (size_t)p_w * p_h; i < n; i++) {
        const unsigned char *b = p_bits + i * 4, *k = p_mask + i * 3;
        unsigned char *o = p_rgba + i * 4;
        unsigned ac = k[0], e = k[2];
        static const unsigned trim[3] = {C_BRONZE >> 16 & 255, C_BRONZE >> 8 & 255, C_BRONZE & 255};
        for (int j = 0; j < 3; j++) {
            unsigned c = (b[2 - j] * (255 - e) + trim[j] * e) / 255;
            o[j] = (unsigned char)((c * ac + 127) / 255);
        }
        o[3] = k[1];
    }
    ID3D11DeviceContext_UpdateSubresource(p_ctx, (ID3D11Resource *)p_tex, 0, NULL, p_rgba, p_w * 4, 0);
}

// ---- blending the panel over the back buffer (premultiplied alpha), the game's state put back afterwards ----
static const char p_src[] =
    "Texture2D<float4> t : register(t0);\n"
    "void vs(uint i : SV_VertexID, out float4 p : SV_Position, out float2 uv : UV) {\n"
    "  uv = float2((i << 1) & 2, i & 2); p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "float4 ps(float4 p : SV_Position, float2 uv : UV) : SV_Target { uint w, h; t.GetDimensions(w, h); return t.Load(int3(uv * float2(w, h), 0)); }\n";
static ID3D11VertexShader *p_vs;
static ID3D11PixelShader *p_ps;
static ID3D11BlendState *p_blend;
static ID3D11RasterizerState *p_rs;
static ID3D11DepthStencilState *p_dss;
static ID3D11RenderTargetView *p_rtv;

static int pipeline(void) {
    pD3DCompile comp = (pD3DCompile)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    ID3DBlob *v = NULL, *f = NULL;
    D3D11_BLEND_DESC bd = {0};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    D3D11_DEPTH_STENCIL_DESC dd = {FALSE, D3D11_DEPTH_WRITE_MASK_ZERO, D3D11_COMPARISON_ALWAYS, FALSE, 0, 0, {0}, {0}};
    int ok = comp && SUCCEEDED(comp(p_src, strlen(p_src), "fgp", NULL, NULL, "vs", "vs_5_0", 0, 0, &v, NULL)) &&
             SUCCEEDED(comp(p_src, strlen(p_src), "fgp", NULL, NULL, "ps", "ps_5_0", 0, 0, &f, NULL)) &&
             SUCCEEDED(ID3D11Device_CreateVertexShader(p_dev, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v), NULL, &p_vs)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(p_dev, ID3D10Blob_GetBufferPointer(f), ID3D10Blob_GetBufferSize(f), NULL, &p_ps)) &&
             SUCCEEDED(ID3D11Device_CreateBlendState(p_dev, &bd, &p_blend)) && SUCCEEDED(ID3D11Device_CreateRasterizerState(p_dev, &rd, &p_rs)) &&
             SUCCEEDED(ID3D11Device_CreateDepthStencilState(p_dev, &dd, &p_dss)) &&
             SUCCEEDED(ID3D11Device_CreateShaderResourceView(p_dev, (ID3D11Resource *)p_tex, NULL, &p_srv));
    if (v) ID3D10Blob_Release(v);
    if (f) ID3D10Blob_Release(f);
    return ok;
}


// ---- the frame-rate counter (top right): rendered / shown frames per second, its own small texture ----
static HDC c_dc;
static HBITMAP c_bmp;
static unsigned char *c_bits, *c_rgba;
static ID3D11Texture2D *c_tex;
static ID3D11ShaderResourceView *c_srv;
static int c_w, c_h;
static HFONT c_font;
static void counter_redraw(void) {
    if (!c_dc) {
        c_w = (int)(130 * p_u); c_h = (int)(24 * p_u);
        BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), c_w, -c_h, 1, 32, BI_RGB, 0, 0, 0, 0, 0}, {{0, 0, 0, 0}}};
        c_dc = CreateCompatibleDC(NULL);
        c_bmp = c_dc ? CreateDIBSection(c_dc, &bi, DIB_RGB_COLORS, (void **)&c_bits, NULL, 0) : NULL;
        c_rgba = malloc((size_t)c_w * c_h * 4);
        c_font = CreateFontW(-(int)(13 * p_u), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        if (!c_bmp || !c_rgba) { p_fpson = 0; return; }
        SelectObject(c_dc, c_bmp);
    }
    if (!c_tex) {   // on the panel's device: made again after fgp_move_device (the old device's texture is not usable)
        D3D11_TEXTURE2D_DESC td = {(UINT)c_w, (UINT)c_h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(ID3D11Device_CreateTexture2D(p_dev, &td, NULL, &c_tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(p_dev, (ID3D11Resource *)c_tex, NULL, &c_srv))) { p_fpson = 0; return; }
    }
    memset(c_bits, 0, (size_t)c_w * c_h * 4);
    wchar_t s[48];
    if (p_shown_fps > p_game_fps * 1.2) _snwprintf(s, 48, L"%.0f / %.0f fps", p_game_fps, p_shown_fps);   // frame generation runs
    else _snwprintf(s, 48, L"%.0f fps", p_game_fps);
    RECT r = {(int)(4 * p_u), 0, c_w - (int)(4 * p_u), c_h};
    SetBkMode(c_dc, TRANSPARENT);
    SelectObject(c_dc, c_font);
    SetTextColor(c_dc, RGB(255, 255, 255));
    DrawTextW(c_dc, s, -1, &r, (p_fpson == 1 || p_fpson == 3 ? DT_LEFT : DT_RIGHT) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    // the numbers only (no background): the text's coverage in white, around it a 2-pixel dark outline so it stays
    // readable on bright sky and smoke; premultiplied RGBA (the outline adds alpha only)
    for (int y = 0; y < c_h; y++)
        for (int x = 0; x < c_w; x++) {
            const unsigned char *b = c_bits + ((size_t)y * c_w + x) * 4;
            unsigned char *o = c_rgba + ((size_t)y * c_w + x) * 4;
            float t = b[1] / 255.0f, ol = 0;
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && yy >= 0 && xx < c_w && yy < c_h) { float v = c_bits[((size_t)yy * c_w + xx) * 4 + 1] / 255.0f; if (v > ol) ol = v; }
                }
            float a = t > ol * 0.8f ? t : ol * 0.8f;
            o[0] = o[1] = o[2] = (unsigned char)(t * 255 + 0.5f);
            o[3] = (unsigned char)(a * 255 + 0.5f);
        }
    ID3D11DeviceContext_UpdateSubresource(p_ctx, (ID3D11Resource *)c_tex, 0, NULL, c_rgba, c_w * 4, 0);
}
static void blend_panel(ID3D11Resource *bb, ID3D11ShaderResourceView *tex, int tx, int ty, int tw, int th) {
    ID3D11DeviceContext *c = p_ctx;
    // the back buffer's view for this call only: a view kept between frames holds a reference to the back buffer, and the
    // game's ResizeBuffers on that swapchain would fail while it lives (ultrareview 2026-11-02)
    if (FAILED(ID3D11Device_CreateRenderTargetView(p_dev, bb, NULL, &p_rtv))) { p_rtv = NULL; return; }
    ID3D11RenderTargetView *rtv[8] = {0}; ID3D11DepthStencilView *dsv = NULL;
    ID3D11BlendState *bs = NULL; FLOAT bf[4]; UINT mask;
    D3D11_VIEWPORT vp[16]; UINT nvp = 16;
    ID3D11RasterizerState *rs = NULL; ID3D11DepthStencilState *dss = NULL; UINT sref = 0;
    ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL; ID3D11GeometryShader *gs = NULL; ID3D11HullShader *hs = NULL; ID3D11DomainShader *ds = NULL;
    ID3D11InputLayout *il = NULL; D3D11_PRIMITIVE_TOPOLOGY topo; ID3D11ShaderResourceView *srv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(c, 8, rtv, &dsv);
    ID3D11DeviceContext_OMGetBlendState(c, &bs, bf, &mask);
    ID3D11DeviceContext_RSGetViewports(c, &nvp, vp);
    ID3D11DeviceContext_RSGetState(c, &rs);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &dss, &sref);
    ID3D11DeviceContext_VSGetShader(c, &vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(c, &gs, NULL, NULL);
    ID3D11DeviceContext_HSGetShader(c, &hs, NULL, NULL);
    ID3D11DeviceContext_DSGetShader(c, &ds, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(c, &il);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &topo);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 1, &srv);

    D3D11_VIEWPORT pv = {(float)tx, (float)ty, (float)tw, (float)th, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &p_rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(c, p_blend, NULL, 0xffffffff);
    ID3D11DeviceContext_RSSetViewports(c, 1, &pv);
    ID3D11DeviceContext_RSSetState(c, p_rs);
    ID3D11DeviceContext_OMSetDepthStencilState(c, p_dss, 0);
    ID3D11DeviceContext_VSSetShader(c, p_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, p_ps, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &tex);
    ID3D11DeviceContext_Draw(c, 3, 0);

    UINT nrt = 0;
    for (UINT i = 0; i < 8; i++) if (rtv[i]) nrt = i + 1;
    ID3D11DeviceContext_OMSetRenderTargets(c, nrt, rtv, dsv);
    ID3D11DeviceContext_OMSetBlendState(c, bs, bf, mask);
    ID3D11DeviceContext_RSSetViewports(c, nvp, vp);
    ID3D11DeviceContext_RSSetState(c, rs);
    ID3D11DeviceContext_OMSetDepthStencilState(c, dss, sref);
    ID3D11DeviceContext_VSSetShader(c, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, ps, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, gs, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, hs, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, ds, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(c, il);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, topo);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &srv);
    for (int i = 0; i < 8; i++) if (rtv[i]) ID3D11RenderTargetView_Release(rtv[i]);
#define REL(x) if (x) (x)->lpVtbl->Release(x)
    REL(dsv); REL(bs); REL(rs); REL(dss); REL(vs); REL(ps); REL(gs); REL(hs); REL(ds); REL(il); REL(srv);
#undef REL
    ID3D11RenderTargetView_Release(p_rtv);
    p_rtv = NULL;
}

int fgp_init(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT bw, UINT bh, DXGI_FORMAT fmt, int sr, int fg, int motion, FgpApply apply) {
    if (fmt != DXGI_FORMAT_R8G8B8A8_UNORM) return 0;   // Attila's back buffer; others not handled
    p_dev = dev; p_ctx = ctx; p_bw = bw; p_bh = bh; p_sr = sr; p_fg = fg; p_motion = motion; p_apply = apply;
    if (p_fg > (int)fgb_fg_max()) p_fg = (int)fgb_fg_max();   // never show a choice this GPU does not allow as the current one
    float u = p_u = bh / 1080.0f;
    float m = 20 * u;   // room for the shadow around the panel
    p_cx0 = m; p_cy0 = m; p_cx1 = m + 790 * u;
    int extra = 3 + 1 + 2;   // + preset, sharpness, V-Sync, FPS counter, upscaler, frame generation tech
    p_cy1 = m + (374 + 44 + (extra + (apply.view ? 1 : 0)) * 58) * u; p_cr = 3 * u;   // + those rows (+ Debug View), the recommendation lines
    p_w = (int)ceilf(p_cx1 + m); p_h = (int)ceilf(p_cy1 + m);
    p_cy1_full = p_cy1;
    p_x = (int)(bw * 0.03f - m); p_y = (int)(bh * 0.08f - m);
    if (p_x < 0) p_x = 0;
    if (p_y < 0) p_y = 0;
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), p_w, -p_h, 1, 32, BI_RGB, 0, 0, 0, 0, 0}, {{0, 0, 0, 0}}};
    p_dc = CreateCompatibleDC(NULL);
    p_bmp = CreateDIBSection(p_dc, &bi, DIB_RGB_COLORS, (void **)&p_bits, NULL, 0);
    p_rgba = malloc((size_t)p_w * p_h * 4);
    if (!p_dc || !p_bmp || !p_rgba) return 0;
    SelectObject(p_dc, p_bmp);
    p_mask = malloc((size_t)p_w * p_h * 3);
    if (!p_mask) return 0;
    make_mask();
    p_title = CreateFontW(-(int)(27 * u), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Palatino Linotype");
    p_label = CreateFontW(-(int)(17 * u), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Palatino Linotype");
    p_small = CreateFontW(-(int)(15 * u), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    p_tiny = CreateFontW(-(int)(13 * u), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    D3D11_TEXTURE2D_DESC td = {(UINT)p_w, (UINT)p_h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &p_tex)) || !pipeline()) return 0;
    p_ok = 1;
    return 1;
}

// the mask: the card (slightly see-through, a hairline highlight on its edge, lit from above) and a soft shadow below it;
// made again when the card's height changes (the DLSS preset row is hidden while AMD FSR upscales)
static void make_mask(void) {
    float u = p_u, m = 20 * u;
    for (int yy = 0; yy < p_h; yy++)
        for (int xx = 0; xx < p_w; xx++) {
            float px = xx + 0.5f, py = yy + 0.5f;
            float d = rr_dist(px, py, p_cx0, p_cy0, p_cx1, p_cy1, p_cr), cov = 0.5f - d;
            cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
            float ds = rr_dist(px, py - 6 * u, p_cx0, p_cy0, p_cx1, p_cy1, p_cr), s = 1 - ds / m;
            s = ds <= 0 ? 1 : s < 0 ? 0 : s * s;
            float ac = cov * 0.96f, a = ac + s * 0.35f * (1 - ac);
            float e = 1 - fabsf(d + 0.75f * u) / (0.75f * u);   // the bronze hairline on the panel's edge
            e = e > 0 ? e * 0.55f : 0;
            unsigned char *k = p_mask + ((size_t)yy * p_w + xx) * 3;
            k[0] = (unsigned char)(ac * 255 + 0.5f); k[1] = (unsigned char)(a * 255 + 0.5f); k[2] = (unsigned char)(e * 255 + 0.5f);
        }
}

void fgp_set_note(const wchar_t *note) { p_note = note; p_dirty = 1; }

// The panel's GPU objects made again on another device (a settings change can come with a new D3D11 device; the panel
// then sits over the frames the mod shows after the bridge, fgbridge.c fallback). bw / bh: that back buffer's size, for
// the mouse; the panel keeps its size and place.
int fgp_move_device(ID3D11Device *dev, ID3D11DeviceContext *ctx, UINT bw, UINT bh) {
    if (!p_dc) return 0;   // never set up
#define DROP(x) if (x) { (x)->lpVtbl->Release(x); (x) = NULL; }
    DROP(p_rtv); DROP(p_srv); DROP(p_tex); DROP(p_vs); DROP(p_ps); DROP(p_blend); DROP(p_rs); DROP(p_dss);
    DROP(c_srv); DROP(c_tex);   // the FPS counter's too: counter_redraw makes them on the new device (ultrareview 2026-11-02)
#undef DROP
    p_cdirty = 1;
    p_dev = dev; p_ctx = ctx; p_bw = bw; p_bh = bh;
    D3D11_TEXTURE2D_DESC td = {(UINT)p_w, (UINT)p_h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    p_ok = SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &p_tex)) && pipeline();
    p_dirty = 1;
    return p_ok;
}

void fgp_set_fps(double game, double shown) {
    if (game != p_game_fps || shown != p_shown_fps) { p_game_fps = game; p_shown_fps = shown; p_dirty = p_cdirty = 1; }
}

// the Defaults button: every choice back to the recommendation (as if clicked), the frame-rate counter kept
static void fgp_defaults(void) {
    Rec r = recommended();
    unsigned fgmax = fgb_fg_max();
    int fg = r.fg > (int)fgmax ? (int)fgmax : r.fg, ra = r.sr_amd, fa = r.fg_amd;
#define SET(var, val, cb) if ((var) != (val)) { (var) = (val); if (p_apply.cb) p_apply.cb(var); }
    SET(p_sr, r.sr, sr);
    SET(p_fg, fg, fg);
    if (p_motion >= 0) { SET(p_motion, 1, motion); }   // Full (per object) for everyone (AGENT_LOG 179)
    SET(p_cap, 1, cap);
    SET(p_preset, 0, preset);
    SET(p_sharp, ra ? 3 : 2, sharp);   // High with AMD FSR upscaling, Medium with DLSS
    if (p_vsync_ok) { SET(p_vsync, 1, vsync); }
#undef SET
    if (ra != p_tsr || fa != p_tfg) { p_tsr = ra; p_tfg = fa; if (p_apply.tech) p_apply.tech(ra, fa); }
    (void)fa; (void)ra;
    p_drop = -1;
    p_dirty = 1;
}

// each Present (main thread), before the frame is handed to the helper: input, then the panel over the back buffer
void fgp_frame(ID3D11Resource *backbuffer, HWND hwnd) {
    if (!p_ok) return;
    static int hz_read;
    if (!hz_read) {   // the screen's refresh rate, for the recommendation
        hz_read = 1;
        MONITORINFOEXW mi = {.cbSize = sizeof mi};
        DEVMODEW dm = {.dmSize = sizeof dm};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi) &&
            EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) { p_hz = dm.dmDisplayFrequency; p_dirty = 1; }
    }
    static SHORT key_prev, lb_prev;
    int focus = GetForegroundWindow() == hwnd;
    SHORT key = focus ? GetAsyncKeyState(p_key) : 0, lb = focus ? GetAsyncKeyState(VK_LBUTTON) : 0;
    if ((key & 0x8000) && !(key_prev & 0x8000)) { p_open = !p_open; p_dirty = 1; }
    key_prev = key;
    int over = 0, mx = 0, my = 0, hover = -1;
    if (p_open) {
        POINT c;
        RECT cr;
        if (GetCursorPos(&c) && ScreenToClient(hwnd, &c) && GetClientRect(hwnd, &cr) && cr.right > 0 && cr.bottom > 0) {
            mx = c.x * (int)p_bw / cr.right - p_x;   // panel texture pixels
            my = c.y * (int)p_bh / cr.bottom - p_y;
            over = mx >= p_cx0 && my >= p_cy0 && mx < p_cx1 && my < p_cy1;
        }
        for (int i = p_nbtn - 1; over && i >= 0 && hover < 0; i--) {
            const Btn *b = &p_btn[i];
            if (b->enabled && mx >= b->r.left && mx < b->r.right && my >= b->r.top && my < b->r.bottom) hover = i;
        }
        if ((lb & 0x8000) && !(lb_prev & 0x8000) && p_drop >= 0 && (hover < 0 || p_btn[hover].row != p_drop)) { p_drop = -1; p_dirty = 1; }
        if (hover >= 0 && (lb & 0x8000) && !(lb_prev & 0x8000)) {
            const Btn *b = &p_btn[hover];
            if (b->row >= 9 && b->row <= 10 && b->value < 0) { p_drop = p_drop == b->row ? -1 : b->row; p_dirty = 1; }
            else if (b->row == 9 || b->row == 10) {   // a pick from the list: DLSS frame generation needs DLSS upscaling
                int sr = b->row == 9 ? b->value : p_tsr, fg = b->row == 10 ? b->value : p_tfg;
                if (sr) fg = 1;
                if (sr != p_tsr || fg != p_tfg) { p_tsr = sr; p_tfg = fg; if (p_apply.tech) p_apply.tech(sr, fg); }
                p_drop = -1;
            }
            if (b->row == 8 && b->value != p_fpson) { p_fpson = b->value; p_cdirty = 1; if (p_apply.fps) p_apply.fps(p_fpson); }   // a corner, or off
            if (b->row == 20) fgp_defaults();
            if (b->row == 0 && b->value != p_sr) { p_sr = b->value; if (p_apply.sr) p_apply.sr(p_sr); }
            if (b->row == 1 && b->value != p_fg) { p_fg = b->value; if (p_apply.fg) p_apply.fg(p_fg); }
            if (b->row == 2 && 1 - b->value != p_motion) { p_motion = 1 - b->value; if (p_apply.motion) p_apply.motion(p_motion); }
            if (b->row == 3 && b->value != p_cap) { p_cap = b->value; if (p_apply.cap) p_apply.cap(p_cap); }
            if (b->row == 4 && b->value != p_preset) { p_preset = b->value; if (p_apply.preset) p_apply.preset(p_preset); }
            if (b->row == 5 && b->value != p_sharp) { p_sharp = b->value; if (p_apply.sharp) p_apply.sharp(p_sharp); }
            if (b->row == 6 && b->value != p_view) { p_view = b->value; if (p_apply.view) p_apply.view(p_view); }
            if (b->row == 7 && b->value != p_vsync) { p_vsync = b->value; if (p_apply.vsync) p_apply.vsync(p_vsync); }
            p_dirty = 1;
        }
    }
    if (hover != p_hover) { p_hover = hover; p_dirty = 1; }
    lb_prev = lb;
    p_block = p_open && over;
    if (p_fpson && p_shown_fps > 0) {   // the counter, panel open or not
        if (p_cdirty || !c_tex) { counter_redraw(); p_cdirty = 0; }
        int left = p_fpson == 1 || p_fpson == 3, top = p_fpson <= 2, mg = (int)(12 * p_u);
        if (c_srv) blend_panel(backbuffer, c_srv, left ? mg : (int)p_bw - c_w - mg, top ? mg : (int)p_bh - c_h - mg, c_w, c_h);
    }
    if (!p_open) return;
    if (p_dirty) { redraw(); p_dirty = 0; }
    blend_panel(backbuffer, p_srv, p_x, p_y, p_w, p_h);
}

// tests (bridgetest): open / choose without a mouse
void fgp_test_open(int open) { p_open = open; p_dirty = 1; }
void fgp_test_drop(int row) { p_drop = row; p_dirty = 1; }   // a drop-down list open (bridgetest panel)
