// SPDX-License-Identifier: Apache-2.0
// X2-LOAD-001 (FFX-2 only, default 1): loads (save load, map change, battle start / end) take far longer at 60 Hz on the
// Steam Deck (U 18/10/2026: save load 14 s with the mod, 2 s stock, fast with pacing=0; PC unaffected). While the game is
// in load mode (App_Animate / Sg_AppRender 0x42F4C0 / 0x42F7F0 test app+0x3AD = FUN_0044cd70(): [0x00DD10D0] != 0 and
// (byte +0x70 != 0 or int +0xE0 >= 0)) Sg_MainLoop does not run: each app frame runs the load step FUN_00455bd0 and
// draws the loading screen FUN_00453800. pacing_pre never sees those frames, so they kept our divider 1 (60 frames/s)
// and the last invocation's exec byte (0 after a render-only one). Fix: at the end of an invocation that leaves the game
// in load mode, the stock divider and exec 1 are put back (pacing_leave_now); the first invocation after the load turns
// 60 Hz on again. Logs each load and its length.
#include "../x2.h"

namespace x2 {

typedef int (__cdecl* loading_t)();
static loading_t g_loading = nullptr;            // FUN_0044cd70 (cdecl, no args, plain RET)
static bool  g_in = false;
static DWORD g_t0 = 0;
static LONG  g_n = 0;
extern Row row_LOAD001;

static void post() {
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
    log_line("  X2-LOAD-001: load-mode test gva=0x%08X (expected 0x0044CD70)", rt_to_gva(fn));
    return true;
}

Row row_LOAD001 = {
    "X2-LOAD-001",
    "loads at stock pacing: the game's load-mode frames (no Sg_MainLoop) get the stock divider and exec 1",
    1, install, pre, post, 0
};

} // namespace x2
