// d3d64: the d3d11 device, the swap chain and a per-frame point under Sol Invictus's 64-bit Attila, plus what the x86
// mod's frame hook (heapfix/fps.c) does in a release build: the fps log and the per-frame rounding-mode check.
// The game file never calls d3d11: attila64.exe does, and D3D11CreateDevice is a static import of it. So:
//   1. the exe's import of D3D11CreateDevice goes through d3d64_create (the loader's start-up test of DirectX 11 ran
//      before plugins load; the next call is the game's real device);
//   2. from that device: IDXGIDevice -> adapter -> IDXGIFactory2, whose CreateSwapChainForHwnd slot (15) goes through
//      d3d64_swapchain, as the loader makes its swap chain that way;
//   3. on the swap chain, the Present slot (8) goes through d3d64_present: once per frame, on the drawing thread.
// The vtables patched are dxgi's own (shared by every object of that class in the process). Only the game's swap
// chain's Present is a frame; any other swap chain's passes straight through.
// On the OpenGL fallback no device is ever made and nothing here runs.
// (Read against the loader's published source, 1.3.6: its plugin interface has no device, context, swap chain or
// frame callback, so the import is the way in. dx11_device.inc makes the device and swap chain exactly this way.)
// fps log (as x86): while fps_on.txt is in the attila64 folder at start, one line per second goes to fps_log.csv
// there, and a summary when the file is deleted.
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "perffix64.h"
#include "d3d64.h"

ID3D11DeviceContext *(*g_d3d_wrap)(ID3D11Device *dev, ID3D11DeviceContext *real);
HRESULT (STDMETHODCALLTYPE *g_d3d_swap_hook)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **, d3d64_swap_fn real);
HRESULT (*g_d3d_present_hook)(IDXGISwapChain *, UINT, UINT, d3d64_present_fn real);
ID3D11Device *g_d3d_dev;
ID3D11DeviceContext *g_d3d_ctx;
IDXGISwapChain1 *g_d3d_swap;         // the game's swap chain: the one made for g_d3d_dev (not referenced: the game owns it)
DWORD g_d3d_thread;
volatile LONG g_d3d_device_no;       // how many devices the game has made: a part that keeps objects of a device compares it
volatile LONG g_frame_no;            // frames presented
volatile LONG g_x87_cw_changed;      // frames on which the drawing thread's x87 rounding mode was not the default
void (*g_d3d_frame)(void *swapchain);   // called before each Present on the drawing thread (uicache, the panel)
// called right after the game made a device, the first and every later one, with the context the game was given
// (the add-on's wrapper when there is one): Flag Wave hooks the table the game draws through
void (*g_d3d_device)(void *dev, void *ctx);

#define RING 16384
static LONG64 g_ring[RING];
static volatile LONG g_count, g_timing;
static wchar_t g_fps_dir[MAX_PATH];

typedef HRESULT (WINAPI *create_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (STDMETHODCALLTYPE *swap_fn)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
typedef HRESULT (STDMETHODCALLTYPE *present_fn)(IDXGISwapChain *, UINT, UINT);
static create_fn g_real_create;
static void (*g_d3d_log)(const char *fmt, ...);

static int slot_set(void **slot, void *to, void **old) {
    DWORD prot;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &prot)) return 0;
    *old = *slot; *slot = to;
    VirtualProtect(slot, sizeof *slot, prot, &prot);
    return 1;
}

// The vtable slots taken. dxgi keeps one vtable per class, so one hook serves every object of the class and a later
// device normally finds its slots taken already. A later device may still come with another class (the debug layer),
// so each slot is remembered with what it held, and the hooks look their original up by the object they are called on.
#define VHOOKS 4
struct vhook { void **slot; void *orig; };
static struct vhook g_swap_hooks[VHOOKS], g_present_hooks[VHOOKS];
static void *vhook_orig(const struct vhook *t, void **slot) {
    for (int i = 0; i < VHOOKS; i++) if (t[i].slot == slot) return t[i].orig;
    return NULL;
}
// 1 = the slot calls `to` (now, or already)
static int vhook_set(struct vhook *t, void **slot, void *to) {
    if (vhook_orig(t, slot)) return 1;   // taken before; if another hook sits on top now, it calls ours as its original
    if (*slot == to) return 0;                                                         // ours, but not from this table: no original known
    for (int i = 0; i < VHOOKS; i++) {
        if (t[i].slot) continue;
        t[i].orig = *slot; t[i].slot = slot;   // known before the slot goes live: the hook may run at once
        void *old;
        if (slot_set(slot, to, &old)) return 1;
        t[i].slot = NULL; t[i].orig = NULL;
        return 0;
    }
    return 0;
}

static HRESULT STDMETHODCALLTYPE d3d64_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    present_fn real = (present_fn)vhook_orig(g_present_hooks, (void **)&sc->lpVtbl->Present);
    if (!real) real = (present_fn)g_present_hooks[0].orig;
    if (!(flags & DXGI_PRESENT_TEST) && (void *)sc == (void *)g_d3d_swap) {   // the game's own swap chain only
        InterlockedIncrement(&g_frame_no);
        // the x87 fix relies on round-to-nearest on the threads that run engine code: say so if that ever changes
        unsigned short cw; __asm__ volatile("fnstcw %0" : "=m"(cw));
        if ((cw & 0xC00) && InterlockedIncrement(&g_x87_cw_changed) == 1 && g_d3d_log)
            g_d3d_log("x87fix: the drawing thread's x87 rounding mode is %#x, not round-to-nearest - float conversions may differ from the game's own; please report this", cw & 0xC00);
        if (g_timing) {
            LARGE_INTEGER t; QueryPerformanceCounter(&t);
            g_ring[g_count & (RING - 1)] = t.QuadPart;
            InterlockedIncrement(&g_count);
        }
        if (g_d3d_frame) g_d3d_frame(sc);
        if (g_d3d_present_hook) return g_d3d_present_hook(sc, sync, flags, real);   // the upscaling add-on presents
    }
    return real(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE d3d64_swapchain(IDXGIFactory2 *f, IUnknown *dev, HWND wnd, const DXGI_SWAP_CHAIN_DESC1 *d,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *out, IDXGISwapChain1 **sc) {
    swap_fn real = (swap_fn)vhook_orig(g_swap_hooks, (void **)&f->lpVtbl->CreateSwapChainForHwnd);
    if (!real) real = (swap_fn)g_swap_hooks[0].orig;
    // the upscaling add-on puts the game's swap chain on a hidden window (its helper presents into the game's)
    HRESULT hr = g_d3d_swap_hook ? g_d3d_swap_hook(f, dev, wnd, d, fs, out, sc, real) : real(f, dev, wnd, d, fs, out, sc);
    if (SUCCEEDED(hr) && sc && *sc && (void *)dev == (void *)g_d3d_dev) {
        if (vhook_set(g_present_hooks, (void **)&(*sc)->lpVtbl->Present, (void *)d3d64_present)) {
            g_d3d_swap = *sc;
            if (g_d3d_log) g_d3d_log("d3d11: swap chain %ux%u found, per-frame point on Present", d ? d->Width : 0, d ? d->Height : 0);
        } else if (g_d3d_log) g_d3d_log("d3d11: the swap chain's Present could not be reached - no fps log, no per-frame checks");
    }
    return hr;
}

// Every call of attila64.exe's import after plugins load is the game's device: the loader's start-up test of
// DirectX 11 ran before. The loader makes the device once; its destroy_device releases device, context and swap
// chain, and a create_device after that makes new ones (dx11_device.inc: dx_destroy, dx_create_device). A device seen
// here after the first therefore replaces it: the pointers move on, and the parts that keep objects of a device are
// told (g_d3d_device) or see the number change (g_d3d_device_no).
static HRESULT WINAPI d3d64_create(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL *fl, UINT nfl, UINT sdk,
                                   ID3D11Device **dev, D3D_FEATURE_LEVEL *got, ID3D11DeviceContext **ctx) {
    HRESULT hr = g_real_create(a, t, sw, flags, fl, nfl, sdk, dev, got, ctx);
    if (FAILED(hr) || !dev || !*dev || !ctx || !*ctx) return hr;
    int again = g_d3d_dev != NULL;
    g_d3d_swap = NULL;   // the old device's swap chain is gone with it; this device's comes next
    g_d3d_dev = *dev; g_d3d_ctx = *ctx; g_d3d_thread = GetCurrentThreadId();
    InterlockedIncrement(&g_d3d_device_no);
    if (again && g_d3d_log) g_d3d_log("d3d11: the game made a new device (the one before is closed): the fix moves to it");
    if (g_d3d_wrap) {   // the upscaling add-on: the game gets its wrapper; everything else here keeps the real context
        ID3D11DeviceContext *w = g_d3d_wrap(*dev, *ctx);
        if (w) *ctx = w;
    }
    if (g_d3d_device) g_d3d_device(*dev, *ctx);   // after the wrapper, so Flag Wave gets the context the game will use
    IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory2 *fac = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(*dev, &IID_IDXGIDevice, (void **)&xd)) && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)) &&
        SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac)) &&
        vhook_set(g_swap_hooks, (void **)&fac->lpVtbl->CreateSwapChainForHwnd, (void *)d3d64_swapchain)) {
        if (g_d3d_log) g_d3d_log("d3d11: the game's device found (feature level %#x)", got ? (unsigned)*got : 0);
    } else if (g_d3d_log) g_d3d_log("d3d11: device found, but not its DXGI factory - no fps log, no per-frame checks");
    if (fac) IDXGIFactory2_Release(fac);
    if (ad) IDXGIAdapter_Release(ad);
    if (xd) IDXGIDevice_Release(xd);
    return hr;
}

// a module's import slot for `name` from `dll` (also used by slots64.c)
void **import_slot64(HMODULE m, const char *dll, const char *name) {
    unsigned char *b = (unsigned char *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd->VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(b + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(b + d->OriginalFirstThunk), *iat = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (!strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(b + names->u1.AddressOfData))->Name, name)) return (void **)&iat->u1.Function;
        }
    }
    return NULL;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
// fps from the 99th-percentile frame time ("1% low"), the worst frame, and the average.
static void stats(double *ms, int n, double secs, double *avg_fps, double *low1, double *worst) {
    qsort(ms, n, sizeof *ms, cmp_d);
    *avg_fps = n / secs;
    *worst = ms[n - 1];
    double p99 = ms[(int)(n * 0.99) < n ? (int)(n * 0.99) : n - 1];
    *low1 = p99 > 0 ? 1000.0 / p99 : 0;
}

static DWORD WINAPI fps_logger(LPVOID unused) {
    (void)unused;
    wchar_t flag[MAX_PATH + 16], csv[MAX_PATH + 16];
    _snwprintf(flag, MAX_PATH + 16, L"%ls\\fps_on.txt", g_fps_dir);
    _snwprintf(csv, MAX_PATH + 16, L"%ls\\fps_log.csv", g_fps_dir);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    FILE *f = _wfopen(csv, L"a");
    if (!f) return 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "# recording %04d-%02d-%02d %02d:%02d:%02d (delete fps_on.txt to stop)\nsecond,fps,one_percent_low_fps,worst_frame_ms\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fflush(f);
    int cap = 1 << 20, all_n = 0, sec = 0;
    static double ms[RING];   // not on this thread's small stack
    double *all = malloc(cap * sizeof *all);
    LONG last = g_count;
    while (GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES) {
        Sleep(1000);
        LONG now = g_count;
        if (now - last > RING - 1) last = now - (RING - 1);   // a slow second for this thread: the newest frames only
        int n = 0;
        for (LONG i = last + 1; i < now; i++) {
            double d = (double)(g_ring[i & (RING - 1)] - g_ring[(i - 1) & (RING - 1)]) * 1000.0 / freq.QuadPart;
            ms[n++] = d;
            if (all && all_n < cap) all[all_n++] = d;
        }
        last = now - 1;   // the frame in progress ends after this sample: counted next time
        sec++;
        if (n < 2) continue;   // a loading screen or a pause
        double secs = 0;
        for (int i = 0; i < n; i++) secs += ms[i];
        double avg, low, worst;
        stats(ms, n, secs / 1000.0, &avg, &low, &worst);
        fprintf(f, "%d,%.1f,%.1f,%.2f\n", sec, avg, low, worst);
        fflush(f);
    }
    if (all && all_n > 1) {
        double secs = 0, avg, low, worst;
        for (int i = 0; i < all_n; i++) secs += all[i] / 1000.0;
        stats(all, all_n, secs > 0 ? secs : 1, &avg, &low, &worst);
        fprintf(f, "# summary: %d frames in %.1f s, average %.1f fps, 1%% low %.1f fps, worst frame %.1f ms\n", all_n, secs, avg, low, worst);
    }
    free(all);
    fclose(f);
    InterlockedExchange(&g_timing, 0);
    return 0;
}

// dir: the attila64 folder. 1 = the exe's D3D11CreateDevice import is hooked, 0 = not found (another loader).
int install_d3d64(const wchar_t *dir, void (*log)(const char *fmt, ...)) {
    void **slot = import_slot64(GetModuleHandleW(NULL), "d3d11.dll", "D3D11CreateDevice");
    if (!slot || !slot_set(slot, (void *)d3d64_create, (void **)&g_real_create)) return 0;
    g_d3d_log = log;
    wcsncpy(g_fps_dir, dir, MAX_PATH - 1);
    wchar_t flag[MAX_PATH + 16];
    _snwprintf(flag, MAX_PATH + 16, L"%ls\\fps_on.txt", g_fps_dir);
    if (GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES) {
        InterlockedExchange(&g_timing, 1);
        CloseHandle(CreateThread(NULL, 0, fps_logger, NULL, 0, NULL));
        log("fps log on (fps_on.txt): one line per second in fps_log.csv; delete fps_on.txt to stop");
    }
    return 1;
}
