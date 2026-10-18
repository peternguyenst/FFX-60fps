// SPDX-License-Identifier: Apache-2.0
// X2-LOAD-001 (FFX-2 only, default 1): loads (save load, map change, battle start / end) take far longer at 60 Hz on the
// Steam Deck (U 18/10/2026: save load 14 s with the mod, 2 s stock, fast with pacing=0; PC unaffected). While the game is
// in load mode (App_Animate / Sg_AppRender 0x42F4C0 / 0x42F7F0 test app+0x3AD = FUN_0044cd70(): [0x00DD10D0] != 0 and
// (byte +0x70 != 0 or int +0xE0 >= 0)) Sg_MainLoop does not run: each app frame runs the load step FUN_00455bd0 and
// draws the loading screen FUN_00453800. pacing_pre never sees those frames, so they kept our divider 1 (60 frames/s)
// and the last invocation's exec byte (0 after a render-only one). Fix: at the end of an invocation that leaves the game
// in load mode, the stock divider and exec 1 are put back (pacing_leave_now); the first invocation after the load turns
// 60 Hz on again. Logs each load and its length.
// U 18/10 19:23: still slow; D: the Deck save load never entered load mode (Sg_MainLoop ran at ~60 inv/s for the 15 s).
// X2-LOAD-001-TRACE=N (diagnostic, default 0): after each map-id change ([0x00D61748], X2-TITLE-001's site 0x466FCD),
// log N invocations: kind, invocation ms, ms since the previous invocation start, backlog count, divider, exec byte,
// and the early-out inputs: HD state block [0x00DCED88] [0] / +0x338 / +0x33C (FUN_00452220 == 2 -> Sg_MainLoop returns
// at its top) and the PS2-menu flags [0x016BC55C] / [0x016BC560] (0x60550C -> 0x6060A9). D 19:26: ~600 invocations of
// 0.03 ms each for ~10 s after the map change.
#include "../x2.h"

namespace x2 {

typedef int (__cdecl* loading_t)();
static loading_t g_loading = nullptr;            // FUN_0044cd70 (cdecl, no args, plain RET)
static bool  g_in = false;
static DWORD g_t0 = 0;
static LONG  g_n = 0;
extern Row row_LOAD001;
static volatile int32_t* g_map = nullptr;
static int32_t g_lastmap = 0x7FFFFFFF;
static int   g_trace = 0, g_tleft = 0;
static LARGE_INTEGER g_qf, g_tstart, g_tprev;
static uint8_t** g_hds = nullptr;                // &DAT_00dced88
static volatile int32_t* g_m1 = nullptr;         // 0x016BC55C
static volatile int32_t* g_m2 = nullptr;         // 0x016BC560

static void post() {
    if (g_tleft > 0) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        static const char* const K[4] = { "L", "R", "B", "S" };
        int st = -1, b8 = -1, bc = -1, m1 = -1, m2 = -1;
        __try {
            uint8_t* h = g_hds ? *g_hds : nullptr;
            if (h) { st = *(int32_t*)h; b8 = h[0x338]; bc = h[0x33C]; }
            if (g_m1) m1 = *g_m1;
            if (g_m2) m2 = *g_m2;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        log_line("LOADTRACE %s inv %.2f ms gap %.2f ms backlog %d div %u exec %d | hd %d %d %d menu %d %d", K[pacing_kind() & 3],
                 (now.QuadPart - g_tstart.QuadPart) * 1000.0 / g_qf.QuadPart,
                 (g_tstart.QuadPart - g_tprev.QuadPart) * 1000.0 / g_qf.QuadPart,
                 *g_backlog_count, *g_divider, *g_exec_byte, st, b8, bc, m1, m2);
        if (--g_tleft == 0) log_line("LOADTRACE end");
    }
    bool l = false;
    __try { l = g_loading() != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!l) return;
    pacing_leave_now("X2-LOAD-001 (load mode)");
    if (!g_in) {
        g_in = true;
        g_t0 = GetTickCount();
        if (InterlockedIncrement(&g_n) <= 50) log_line("X2-LOAD-001: load mode entered - stock pacing for the load");
    }
}

static void pre() {
    if (g_trace && g_map) {
        int32_t m = *g_map;
        if (m != g_lastmap) {
            if (g_lastmap != 0x7FFFFFFF) { g_tleft = g_trace; log_line("LOADTRACE: map 0x%X -> 0x%X, next %d invocations", g_lastmap, m, g_trace); }
            g_lastmap = m;
        }
        if (g_tleft > 0) { g_tprev = g_tstart; QueryPerformanceCounter(&g_tstart); }
    }
    if (!g_in) return;
    g_in = false;
    if (g_n <= 50) log_line("X2-LOAD-001: load over after %lu ms", GetTickCount() - g_t0);
}

static bool install() {
    // FUN_0044cd70: MOV EAX,[0x00dd10d0]; TEST EAX,EAX; JZ; CMP byte [EAX+0x70],0; JNZ; CMP dword [EAX+0xE0],0; JGE
    uint8_t* fn = sig_find("X2-LOAD-001 load-mode test FUN_0044cd70",
        "A1 ?? ?? ?? ?? 85 C0 74 1E 80 78 70 00 75 0F 83 B8 E0 00 00 00 00 7D 06", 0x44CD70);
    if (!fn) return false;
    g_loading = (loading_t)fn;
    g_trace = config_row("X2-LOAD-001-TRACE", 0);
    if (g_trace) {
        uint8_t* at = sig_find("X2-LOAD-001 map id (title-map test)", "83 3D ?? ?? ?? ?? 17 75 49 E8", 0x466FCD);
        g_map = at ? (volatile int32_t*)abs_operand("map id", at + 2) : nullptr;
        uint8_t* hs = sig_find("X2-LOAD-001 HD state block (list clear test)",
            "A3 ?? ?? ?? ?? 80 B8 38 03 00 00 00 75 0E 80 B8 3C 03 00 00 00 75 05 83 38 01 74 07 8B CE E8", 0x464FEF);
        g_hds = hs ? (uint8_t**)abs_operand("HD state ptr", hs + 1) : nullptr;
        uint8_t* mf = sig_find("X2-LOAD-001 menu early-out 0x60550C",
            "83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 01", 0x60550C);
        if (mf) { g_m1 = (volatile int32_t*)abs_operand("menu flag 1", mf + 2); g_m2 = (volatile int32_t*)abs_operand("menu flag 2", mf + 0xF); }
        QueryPerformanceFrequency(&g_qf);
        QueryPerformanceCounter(&g_tstart);
    }
    log_line("  X2-LOAD-001: load-mode test gva=0x%08X (expected 0x0044CD70)", rt_to_gva(fn));
    return true;
}

Row row_LOAD001 = {
    "X2-LOAD-001",
    "loads at stock pacing: the game's load-mode frames (no Sg_MainLoop) get the stock divider and exec 1",
    1, install, pre, post, 0
};

} // namespace x2
