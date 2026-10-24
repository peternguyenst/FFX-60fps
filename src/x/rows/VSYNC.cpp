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

// DIAG-FRAMETIME: time inside Present and what the game passes (observe only; the values pass through unchanged
// while the row is off).
static LARGE_INTEGER g_qpf = {};
static double   g_pres_ms = 0;
static uint32_t g_pres_n = 0, g_pres_sync1 = 0;
static UINT     g_last_flags = 0;
static DWORD    g_pres_tid = 0;

static HRESULT STDMETHODCALLTYPE h_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    UINT use = sync;
    if (row_VSYNC.on && !g_disabled) {
        if (InterlockedIncrement(&g_forced) == 1) log_line("VSYNC: first Present with SyncInterval 1 (the game passed %u)", sync);
        use = 1;
    }
    static IDXGISwapChain* s_logged = nullptr;
    if (sc != s_logged) {
        s_logged = sc;
        DXGI_SWAP_CHAIN_DESC d = {};
        BOOL fs = FALSE;
        if (SUCCEEDED(sc->GetDesc(&d))) {
            sc->GetFullscreenState(&fs, nullptr);
            log_line("VSYNC/diag: swap chain %p: %ux%u refresh %u/%u, buffers %u, SwapEffect %u (0 DISCARD, 1 SEQUENTIAL, "
                     "3 FLIP_SEQUENTIAL, 4 FLIP_DISCARD), Windowed %d, exclusive fullscreen %d, flags 0x%X "
                     "(0x800 = ALLOW_TEARING); game passes SyncInterval %u, Present flags 0x%X",
                     (void*)sc, d.BufferDesc.Width, d.BufferDesc.Height, d.BufferDesc.RefreshRate.Numerator,
                     d.BufferDesc.RefreshRate.Denominator, d.BufferCount, (unsigned)d.SwapEffect, d.Windowed, fs, d.Flags,
                     sync, flags);
        }
    }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    HRESULT hr = g_present_orig(sc, use, flags);
    QueryPerformanceCounter(&b);
    if (g_qpf.QuadPart) g_pres_ms += (b.QuadPart - a.QuadPart) * 1000.0 / g_qpf.QuadPart;
    g_pres_n++;
    if (use) g_pres_sync1++;
    g_last_flags = flags;
    g_pres_tid = GetCurrentThreadId();
    return hr;
}

void vsync_present_stats(double* ms, uint32_t* n, uint32_t* synced, unsigned* flags, unsigned long* tid) {
    *ms = g_pres_ms; *n = g_pres_n; *synced = g_pres_sync1; *flags = g_last_flags; *tid = g_pres_tid;
    g_pres_ms = 0; g_pres_n = 0; g_pres_sync1 = 0;
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

static bool hook_present() {
    if (g_present) return true;
    QueryPerformanceFrequency(&g_qpf);
    void* p = find_present();
    if (!p) return false;
    if (!hook_entry("VSYNC Present", p, (void*)&h_present, (void**)&g_present_orig)) return false;
    g_present = p;
    return true;
}

bool vsync_hook_for_diag() { return hook_present(); }

static bool install() {
    if (!hook_present()) return false;
    void* p = g_present;
    log_line("  VSYNC: IDXGISwapChain::Present at %p hooked; SyncInterval 1 while on%s", p,
             rate_mult_cfg() > 2 ? " (above 60 fps the monitor refresh caps the game speed)" : "");
    return true;
}

Row row_VSYNC = {
    "VSYNC",
    "V-Sync: frames wait for the screen refresh (no tearing; the game presents without it)",
    0, install, nullptr, nullptr, 0
};
