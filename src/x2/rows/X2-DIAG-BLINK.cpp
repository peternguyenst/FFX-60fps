// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-BLINK (diagnostic, default 1 while the mod is in progress): finds blinking magic effects without a user
// report per spell (user 22/10/2026: "I cannot test every spell and tell you one by one"). A blink = something drawn on
// logic frames but not on render-only frames, so render-only frames issue fewer GPU draw calls. The row counts
// ID3D11DeviceContext draw calls (DrawIndexed / Draw / DrawIndexedInstanced / DrawInstanced, vtable slots 12 / 13 /
// 20 / 21 of the immediate context, found through a throwaway device like X2-DIAG-PRESENT) per invocation window
// (rows pre() to the next pre()), labels each window by the invocation kind, and for every loaded magic DLL compares
// each render-only window with the logic window right before it. At the DLL's unload it logs one line:
//   X2-DIAG-BLINK: magic_NNNN.dll: logic avg A, render-only avg B, below both neighbours S%, above both L% [BLINK SUSPECT]
//     | by size 2-3/4-7/8-15/16-31/32+ draws: below a/b/c/d/e, above a/b/c/d/e
// where "below both" = a render-only window with at least X2-DIAG-BLINK-MIN (default 2) fewer draws than the logic
// windows before and after it; suspect when S - L >= 25 points over 20+ pairs. The size histogram is there because the
// no-magic baseline is itself 29-33% below / 0% above (D 23/10/2026 01:49-01:55), so only the deficit size can tell a
// missing effect (tens of draws, H) from that small one-sided noise. Every 10 s with no magic DLL loaded the same numbers are logged as a baseline. Observe only.
#include "../x2.h"
#include <d3d11.h>
#include <string.h>
#pragma comment(lib, "d3d11.lib")

namespace x2 {

extern Row row_DIAGBLINK;

typedef void(STDMETHODCALLTYPE* drawidx_t)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* draw_t)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* drawidxinst_t)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE* drawinst_t)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);

static drawidx_t g_o12 = nullptr;
static draw_t g_o13 = nullptr;
static drawidxinst_t g_o20 = nullptr;
static drawinst_t g_o21 = nullptr;
static volatile LONG g_draws = 0;
// X2-DIAG-CAPTURE: called before every counted draw (type 12 / 13 / 20 / 21 = vtable slot, then the call's arguments);
// true skips the draw (X2-DIAG-CAPTURE-FBSKIP).
bool (*volatile g_draw_tap)(ID3D11DeviceContext* c, int type, UINT a, UINT b, UINT d, INT e) = nullptr;

static void STDMETHODCALLTYPE h12(ID3D11DeviceContext* c, UINT a, UINT b, INT d) { InterlockedIncrement(&g_draws); if (g_draw_tap && g_draw_tap(c, 12, a, b, 0, d)) return; g_o12(c, a, b, d); }
static void STDMETHODCALLTYPE h13(ID3D11DeviceContext* c, UINT a, UINT b) { InterlockedIncrement(&g_draws); if (g_draw_tap && g_draw_tap(c, 13, a, b, 0, 0)) return; g_o13(c, a, b); }
static void STDMETHODCALLTYPE h20(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) { InterlockedIncrement(&g_draws); if (g_draw_tap && g_draw_tap(c, 20, a, b, d, e)) return; g_o20(c, a, b, d, e, f); }
static void STDMETHODCALLTYPE h21(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) { InterlockedIncrement(&g_draws); if (g_draw_tap && g_draw_tap(c, 21, a, b, d, (INT)e)) return; g_o21(c, a, b, d, e); }

struct Stat {
    uint8_t* base;
    char name[24];
    uint64_t logic_draws, ro_draws;
    uint32_t logic_n, ro_n, pairs, short_n, long_n;
    uint32_t short_sz[5], long_sz[5];             // by deficit size: 2-3, 4-7, 8-15, 16-31, 32+ draws
};
enum { MAX_DLL = 8 };
static Stat g_dll[MAX_DLL];
static Stat g_base_stat;                          // no magic DLL loaded
static int g_min_short = 2;
static int g_prev_kind = -1;
static LONG g_prev_logic_draws = -1;              // draws of the last logic window (-1: none yet)
static LONG g_pend_ro = -1;                       // render-only window waiting for the next logic window
static ULONGLONG g_base_t0 = 0;

// A render-only window is judged against the logic windows on both sides: "short" = below both by g_min_short or
// more, "long" = above both. Animation noise gives about as many long as short (U/D 22/10/2026 23:46: no magic DLL,
// 31-40% "short" against the previous logic window only); a blink gives short >> long.
static void add_window(Stat& s, int kind, LONG n) {
    if (kind == INV_RENDER_ONLY) { s.ro_draws += n; s.ro_n++; }
    else if (kind == INV_LOGIC) { s.logic_draws += n; s.logic_n++; }
}
static int size_bucket(LONG d) { return d < 4 ? 0 : d < 8 ? 1 : d < 16 ? 2 : d < 32 ? 3 : 4; }
static void add_pair(Stat& s, int delta, LONG size) {
    s.pairs++;
    if (delta > 0) { s.short_n++; s.short_sz[size_bucket(size)]++; }
    if (delta < 0) { s.long_n++; s.long_sz[size_bucket(size)]++; }
    if (s.base) mag035_pair(s.base, delta);
}

static void report(const Stat& s, const char* what) {
    if (!s.logic_n || !s.ro_n) return;
    double a = (double)s.logic_draws / s.logic_n, b = (double)s.ro_draws / s.ro_n;
    double sp = s.pairs ? 100.0 * s.short_n / s.pairs : 0.0, lp = s.pairs ? 100.0 * s.long_n / s.pairs : 0.0;
    bool suspect = s.pairs >= 20 && sp - lp >= 25.0;
    log_line("X2-DIAG-BLINK: %s: logic avg %.1f draws (%u), render-only avg %.1f (%u, %.0f%%), below both neighbours %.0f%%, "
             "above both %.0f%% of %u%s | by size 2-3/4-7/8-15/16-31/32+ draws: below %u/%u/%u/%u/%u, above %u/%u/%u/%u/%u",
             what, a, s.logic_n, b, s.ro_n, a > 0 ? 100.0 * b / a : 0.0, sp, lp, s.pairs, suspect ? " [BLINK SUSPECT]" : "",
             s.short_sz[0], s.short_sz[1], s.short_sz[2], s.short_sz[3], s.short_sz[4], s.long_sz[0], s.long_sz[1],
             s.long_sz[2], s.long_sz[3], s.long_sz[4]);
}

static void pre() {
    LONG n = InterlockedExchange(&g_draws, 0);
    int kind = g_prev_kind;
    if (kind >= 0) {
        bool any = false;
        for (Stat& s : g_dll) if (s.base) any = true;
        int delta = 0;
        LONG size = 0;
        bool pair = kind == INV_LOGIC && g_pend_ro >= 0 && g_prev_logic_draws >= 0;
        if (pair) {
            LONG lo = g_prev_logic_draws < n ? g_prev_logic_draws : n, hi = g_prev_logic_draws > n ? g_prev_logic_draws : n;
            delta = lo - g_pend_ro >= g_min_short ? 1 : g_pend_ro - hi >= g_min_short ? -1 : 0;
            size = delta > 0 ? lo - g_pend_ro : g_pend_ro - hi;
        }
        for (Stat& s : g_dll) if (s.base) { add_window(s, kind, n); if (pair) add_pair(s, delta, size); }
        if (!any) { add_window(g_base_stat, kind, n); if (pair) add_pair(g_base_stat, delta, size); }
        if (kind == INV_LOGIC) { g_prev_logic_draws = n; g_pend_ro = -1; }
        else if (kind == INV_RENDER_ONLY) g_pend_ro = n;
        else { g_prev_logic_draws = -1; g_pend_ro = -1; }
    }
    g_prev_kind = pacing_kind();
    ULONGLONG t = GetTickCount64();
    if (t - g_base_t0 >= 10000) {
        g_base_t0 = t;
        report(g_base_stat, "no magic DLL (baseline, last 10 s)");
        memset(&g_base_stat, 0, sizeof(g_base_stat));
    }
}

static void on_load(uint8_t* base, uint32_t, const char* name) {
    for (Stat& s : g_dll)
        if (!s.base) {
            memset(&s, 0, sizeof(s));
            s.base = base;
            strncpy_s(s.name, name, _TRUNCATE);
            return;
        }
}

static void on_unload(uint8_t* base) {
    for (Stat& s : g_dll)
        if (s.base == base) {
            report(s, s.name);
            s.base = nullptr;
        }
}

static bool find_draws(void** fns) {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                   nullptr, &ctx);
    if (SUCCEEDED(hr) && ctx) {
        void** vt = *(void***)ctx;
        fns[0] = vt[12]; fns[1] = vt[13]; fns[2] = vt[20]; fns[3] = vt[21];
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (FAILED(hr)) log_line("  X2-DIAG-BLINK: throwaway device failed (0x%08lX)", (unsigned long)hr);
    return SUCCEEDED(hr);
}

static bool install() {
    g_min_short = config_row("X2-DIAG-BLINK-MIN", 2);
    void* f[4] = {};
    if (!find_draws(f)) return false;
    if (!hook_entry("X2-DIAG-BLINK DrawIndexed", f[0], (void*)&h12, (void**)&g_o12)) return false;
    if (!hook_entry("X2-DIAG-BLINK Draw", f[1], (void*)&h13, (void**)&g_o13)) { hook_remove(f[0]); return false; }
    if (!hook_entry("X2-DIAG-BLINK DrawIndexedInstanced", f[2], (void*)&h20, (void**)&g_o20)) { hook_remove(f[0]); hook_remove(f[1]); return false; }
    if (!hook_entry("X2-DIAG-BLINK DrawInstanced", f[3], (void*)&h21, (void**)&g_o21)) { hook_remove(f[0]); hook_remove(f[1]); hook_remove(f[2]); return false; }
    if (!magic_mods_subscribe("X2-DIAG-BLINK", on_load, on_unload)) return false;
    log_line("  X2-DIAG-BLINK: D3D11 draw calls counted per invocation; per magic DLL verdict at unload (short = %d+ fewer)",
             g_min_short);
    return true;
}

Row row_DIAGBLINK = {
    "X2-DIAG-BLINK",
    "diagnostic: per magic DLL, render-only frames drawing less than logic frames (blink suspects) from D3D11 draw counts",
    1, install, pre, nullptr, 0
};

} // namespace x2
