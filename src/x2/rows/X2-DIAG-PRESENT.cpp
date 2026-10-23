// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-PRESENT (diagnostic, default 0): times IDXGISwapChain::Present, the moment a frame actually goes to the
// screen. U 13/10/2026: a 60 fps screen recording shows the camera "move, move, pause, move, pause" while every
// game-thread frame and every app render call is evenly spaced (D: X2-DIAG-RATE, F9 traces), so the unevenness is
// after the game logic. Every ~5 s: presents per second, interval min/avg/max, intervals over 20 ms, the SyncInterval
// and Flags values seen. Present is found through a throwaway device + swap chain on a hidden window (the vtable is
// shared by every swap chain), entry-hooked with MinHook; the game's own swap chain is not touched otherwise.
// X2-VSYNC (default 0, own row in this file: one Present hook serves both): the game presents with SyncInterval 0 (D
// 21:46 and every X2-DIAG-PRESENT line since), so frames are not tied to the refresh. U 18/10/2026: tearing ("split
// line") in FFX-2 on the Steam Deck, not on Windows; U 13/10 Windows: a "ghost" double image when walking left/right
// (tearing). While the row is on, Present gets SyncInterval 1 (wait for the next refresh). Off at startup: the hook is
// still not installed (Config menu choice applies at the next start).
#include "../x2.h"
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#pragma comment(lib, "d3d11.lib")

namespace x2 {

typedef HRESULT(STDMETHODCALLTYPE* present_t)(IDXGISwapChain* sc, UINT sync, UINT flags);

static present_t g_present_orig = nullptr;
static void*     g_present = nullptr;
static LARGE_INTEGER g_f, g_last, g_win;
static volatile LONG g_n = 0, g_long = 0;
static double    g_min = 1e9, g_max = 0, g_sum = 0;
static UINT      g_sync_seen = 0xFFFFFFFF, g_flags_seen = 0;
static bool      g_sync_mixed = false;
static CRITICAL_SECTION g_cs;
// X2-PRESENT-VSYNC (test, default 0): the game presents with SyncInterval 0 (D fps60x2.log 21:46: no V-Sync, so frames
// tear; walking left/right shows a "ghost" double image). 1 = pass SyncInterval 1 (wait for the next refresh).
static UINT g_force_sync = 0;
extern Row row_DIAGPRESENT, row_VSYNC;
static bool g_hooked = false, g_stats_ready = false;
void (*volatile g_present_tap)(IDXGISwapChain* sc) = nullptr;   // X2-DIAG-LUMA: called before every Present
void (*volatile g_present_tap2)(IDXGISwapChain* sc) = nullptr;  // X2-DIAG-CAPTURE: called before every Present
void (*volatile g_present_tap3)(IDXGISwapChain* sc) = nullptr;  // X2-DIAG-AUTOBLINK: called before every Present

static HRESULT STDMETHODCALLTYPE h_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    UINT out = row_VSYNC.on ? 1 : g_force_sync ? g_force_sync : sync;
    if (g_present_tap) g_present_tap(sc);
    if (g_present_tap2) g_present_tap2(sc);
    if (g_present_tap3) g_present_tap3(sc);
    if (!row_DIAGPRESENT.on || !g_stats_ready) return g_present_orig(sc, out, flags);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    EnterCriticalSection(&g_cs);
    if (g_last.QuadPart) {
        double dt = (double)(now.QuadPart - g_last.QuadPart) * 1000.0 / (double)g_f.QuadPart;
        if (dt < g_min) g_min = dt;
        if (dt > g_max) g_max = dt;
        g_sum += dt;
        if (dt > 20.0) g_long++;
    }
    g_last = now;
    g_n++;
    if (g_sync_seen == 0xFFFFFFFF) g_sync_seen = sync;
    else if (g_sync_seen != sync) g_sync_mixed = true;
    g_flags_seen |= flags;
    LeaveCriticalSection(&g_cs);
    return g_present_orig(sc, out, flags);
}

// Present's address from a throwaway swap chain (DXGI's vtable slot 8, shared by all swap chains of the process).
static void* find_present() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"x2_present_probe";
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
    if (!p) log_line("  X2-DIAG-PRESENT: throwaway swap chain failed (0x%08lX)", (unsigned long)hr);
    return p;
}

bool present_hook() {
    if (g_hooked) return true;
    g_present = find_present();
    if (!g_present) return false;
    if (!hook_entry("X2 Present", g_present, (void*)&h_present, (void**)&g_present_orig)) return false;
    g_hooked = true;
    log_line("  X2 Present: IDXGISwapChain::Present at %p hooked", g_present);
    return true;
}

static bool install() {
    InitializeCriticalSection(&g_cs);
    QueryPerformanceFrequency(&g_f);
    QueryPerformanceCounter(&g_win);
    g_force_sync = (UINT)config_row("X2-PRESENT-VSYNC", 0);
    g_stats_ready = true;
    if (!present_hook()) return false;
    log_line("  X2-DIAG-PRESENT: %s", g_force_sync ? "X2-PRESENT-VSYNC: SyncInterval forced" : "observe only");
    return true;
}

static bool install_vsync() {
    if (!present_hook()) return false;
    log_line("  X2-VSYNC: Present with SyncInterval 1 while on (the game passes 0)");
    return true;
}

static void post() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    double s = (double)(t.QuadPart - g_win.QuadPart) / (double)g_f.QuadPart;
    if (s < 5.0) return;
    g_win = t;
    EnterCriticalSection(&g_cs);
    LONG n = g_n;
    if (n > 1)
        log_line("X2-DIAG-PRESENT: %.1f presents/s, interval ms min %.1f avg %.2f max %.1f, >20ms %ld, SyncInterval %u%s, flags 0x%X",
                 n / s, g_min, g_sum / (n - 1), g_max, (LONG)g_long, g_sync_seen, g_sync_mixed ? " (mixed)" : "", g_flags_seen);
    else
        log_line("X2-DIAG-PRESENT: %ld presents in %.1f s", n, s);
    g_n = 0; g_long = 0; g_min = 1e9; g_max = 0; g_sum = 0; g_sync_seen = 0xFFFFFFFF; g_sync_mixed = false; g_flags_seen = 0;
    g_last.QuadPart = 0;
    LeaveCriticalSection(&g_cs);
}

Row row_DIAGPRESENT = {
    "X2-DIAG-PRESENT",
    "diagnostic: IDXGISwapChain::Present rate and interval (presentation evenness)",
    0, install, nullptr, post, 0
};

Row row_VSYNC = {
    "X2-VSYNC",
    "V-Sync: frames wait for the screen refresh (no tearing; the game presents without it)",
    0, install_vsync, nullptr, nullptr, 0
};

} // namespace x2
