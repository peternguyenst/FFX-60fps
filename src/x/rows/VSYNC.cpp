// SPDX-License-Identifier: Apache-2.0
// VSYNC (option, default 0; Config page row): port of FFX-2's X2-VSYNC. FFX reads VSync from GameSetting.ini but
// neither the launcher nor the game shows it, so players cannot turn it on. While on, IDXGISwapChain::Present gets
// SyncInterval 1 (wait for the next screen refresh: no tearing). Above 60 fps the refresh rate caps the loop, and the
// mod's pacing ties game speed to frames, so the game slows down when the fps setting is above the monitor's rate.
// Present is found through a throwaway device and swap chain on a hidden window (every swap chain shares the vtable)
// and hooked with MinHook when the row is first turned on; the game's swap chain is not otherwise touched.
// Details: docs/fps60/rows/VSYNC.md
#include "../fps60.h"
#include <d3d11.h>
#include <dxgi.h>
#pragma comment(lib, "d3d11.lib")

typedef HRESULT(STDMETHODCALLTYPE* present_t)(IDXGISwapChain* sc, UINT sync, UINT flags);

static present_t g_present_orig = nullptr;
static void*     g_present = nullptr;
static volatile LONG g_forced = 0;

static HRESULT STDMETHODCALLTYPE h_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!row_VSYNC.on || g_disabled) return g_present_orig(sc, sync, flags);
    if (InterlockedIncrement(&g_forced) == 1) log_line("VSYNC: first Present with SyncInterval 1 (the game passed %u)", sync);
    return g_present_orig(sc, 1, flags);
}

// Present's address from a throwaway swap chain (DXGI's vtable slot 8, shared by all swap chains of the process).
static void* find_present() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"fps60_present_probe";
    RegisterClassW(&wc);
    HWND w = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w) return nullptr;
    DXGI_SWAP_CHAIN_DESC d = {};
    d.BufferCount = 1;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = w;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    void* p = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &d, &sc, &dev, nullptr, &ctx);
    if (SUCCEEDED(hr) && sc) p = (*(void***)sc)[8];
    if (ctx) ctx->Release();
    if (sc) sc->Release();
    if (dev) dev->Release();
    DestroyWindow(w);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (!p) log_line("  VSYNC: throwaway swap chain failed (0x%08lX)", (unsigned long)hr);
    return p;
}

static bool install() {
    if (g_present) return true;
    void* p = find_present();
    if (!p) return false;
    if (!hook_entry("VSYNC Present", p, (void*)&h_present, (void**)&g_present_orig)) return false;
    g_present = p;
    log_line("  VSYNC: IDXGISwapChain::Present at %p hooked; SyncInterval 1 while on%s", p,
             rate_mult_cfg() > 2 ? " (above 60 fps the monitor refresh caps the game speed)" : "");
    return true;
}

Row row_VSYNC = {
    "VSYNC",
    "V-Sync: frames wait for the screen refresh (no tearing; the game presents without it)",
    0, install, nullptr, nullptr, 0
};
