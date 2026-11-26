// overlay.c's Present hook against the loop in a player's 7.5 crash dumps: the hooked "DXGI" Present calls the
// swapchain's Present again (through the vtable, so back into ours) for the same swapchain. With the fix it ends at
// once; a nested Present of another swapchain still goes through.
#include "../overlay.c"

// overlay.c's outside needs (fgpanel.c, heapfix.c, patch.h)
int fgp_init(ID3D11Device *d, ID3D11DeviceContext *c, UINT bw, UINT bh, DXGI_FORMAT f, int sr, int fg, int mo, FgpApply ap) {
    (void)d; (void)c; (void)bw; (void)bh; (void)f; (void)sr; (void)fg; (void)mo; (void)ap; return 0;
}
int fgp_move_device(ID3D11Device *d, ID3D11DeviceContext *c, UINT bw, UINT bh) { (void)d; (void)c; (void)bw; (void)bh; return 0; }
void fgp_frame(ID3D11Resource *bb, HWND w) { (void)bb; (void)w; }
void fgp_set_cull_only(void) {}
void fgp_di8_created(void *di) { (void)di; }
void culling_pick(int c) { (void)c; }
void (*g_dev_di8_created)(void *di);

#include <stdio.h>
static IDXGISwapChainVtbl vt;
static IDXGISwapChain game = {&vt}, other = {&vt};
static int real_calls, getbuffer_calls, logged;
static void tlog(const char *fmt, ...) { (void)fmt; logged++; }
static HRESULT STDMETHODCALLTYPE fake_getbuffer(IDXGISwapChain *sc, UINT i, REFIID r, void **o) {
    (void)sc; (void)i; (void)r; *o = NULL; getbuffer_calls++; return E_FAIL;   // draw() stops here
}
// "DXGI": the first Present of the game's swapchain calls the vtable again for the same swapchain (as the dumps show)
// and, from inside, presents another swapchain once
static HRESULT STDMETHODCALLTYPE fake_real(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (++real_calls > 1000) return E_FAIL;   // the old code: endless (the test fails instead of overflowing)
    if (sc == &game) {
        sc->lpVtbl->Present(sc, sync, flags);
        if (real_calls == 1) other.lpVtbl->Present(&other, 0, 0);
    }
    return S_OK;
}

int main(void) {
    o_log = tlog;
    vt.GetBuffer = fake_getbuffer;
    vt.Present = w_present;
    r_present = fake_real;
    HRESULT hr = game.lpVtbl->Present(&game, 1, 0);
    // DXGI once for the game's swapchain, once for the other one; the nested game Present skipped
    int ok = hr == S_OK && real_calls == 2 && getbuffer_calls == 1 && logged == 1;
    printf("game Present: hr %08lx, DXGI calls %d (want 2), panel draws %d (want 1), log lines %d (want 1)\n", hr, real_calls,
           getbuffer_calls, logged);
    real_calls = 0;
    hr = game.lpVtbl->Present(&game, 1, 0);   // the next frame works the same: nothing left set, logged once only
    ok = ok && hr == S_OK && real_calls == 2 && getbuffer_calls == 2 && logged == 1;
    printf("next frame: DXGI calls %d (want 2), panel draws %d (want 2), log lines %d (want 1)\n", real_calls, getbuffer_calls, logged);
    puts(ok ? "overlay_nest: ok" : "overlay_nest: FAILED");
    return !ok;
}
