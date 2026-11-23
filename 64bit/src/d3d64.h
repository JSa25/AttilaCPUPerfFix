// d3d64.c's hook points for the upscaling add-on (bridge64_tail.inc sets them when the add-on is installed)
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
typedef HRESULT (STDMETHODCALLTYPE *d3d64_swap_fn)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
typedef HRESULT (STDMETHODCALLTYPE *d3d64_present_fn)(IDXGISwapChain *, UINT, UINT);
// the context the game gets from D3D11CreateDevice instead of the real one (NULL: the real one)
extern ID3D11DeviceContext *(*g_d3d_wrap)(ID3D11Device *dev, ID3D11DeviceContext *real);
// the game's CreateSwapChainForHwnd, given the original to call
extern HRESULT (STDMETHODCALLTYPE *g_d3d_swap_hook)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **, d3d64_swap_fn real);
// the game's swap chain's Present after the frame point, given the original to call
extern HRESULT (*g_d3d_present_hook)(IDXGISwapChain *, UINT, UINT, d3d64_present_fn real);
unsigned ctxwrap_methods(void);   // out/gen/ctxwrap64.c
