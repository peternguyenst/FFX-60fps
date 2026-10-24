// SPDX-License-Identifier: Apache-2.0
// X2-HD-001 (FFX-2 only): the HD frame's pass order flips between logic and render-only frames.
// Ultima (magic_0106) sphere glow blink, U 22-23/10/2026, fixed U 23/10/2026 21:03; outer repo
// re/ledger/ULTIMA-0106.md (capture 20:49 texture means, REA decompiles).
// Hd_FrameRender 0x47B880 orders its passes by hdframe+0xE920: 0 -> Hd_ModelPass, then the post chain FUN_004aaa00
// (depth / glow chain added over the screen) sees the models; != 0 -> the chain first (on a scene without them),
// FUN_0045f6b0 draws the models after. Hd_ModelListReset 0x468DC0 (every invocation) sets the flag when no pass marker
// was pushed since the last reset; magic service +0xEBC Hd_PassOrderModelsFirst FUN_0044d260 (-> FUN_0046ac50, list
// switch back, flag 0) clears it. 0106's task calls +0xEBC on every logic tick (`if (task+0x74) svc[0xEBC]()`), and
// render-only invocations do not run the task: their render keeps the flag the reset set, so the glow chain blurs a
// black scene on every second frame (D 21:03: +0xEBC LOGIC 505 / RENDER 0).
// Fix: when an invocation made no +0xEBC call and the previous one did, the row calls it once after Sg_MainLoop returns
// (post, before Sg_AppRender), so the render-only frame renders with the logic frame's pass order. Generic: any DLL or
// OPU op (0x86AD10) that calls +0xEBC on logic ticks only is covered. D 21:03: frame L-R gap +5.3 / +5.7 -> +0.1 / +0.4.
// (A first version also replayed pass markers, FUN_00449eb0: none was ever pushed during Ultima, removed.)
// Evidence: FUN_0044d260 sig tools/sigs_x2_capture.txt (`MOV ECX,[g_dwHdFrame]; JMP FUN_0046ac50`, 1 hit); REA
// ev_a04d1f01 (0x468DC0), ev_bf6828d4 (0x4676F0).
#include "../x2.h"

namespace x2 {

extern Row row_HD001;

typedef void(__cdecl* clear_t)(void);
static clear_t  g_clear = nullptr;               // MinHook trampoline
static uint8_t* g_clear_fn = nullptr;            // FUN_0044d260 (magic service +0xEBC)
static bool     g_hooked = false;
static int  g_clear_cur = 0, g_clear_prev = 0;
static bool g_clearing = false;
static volatile LONG g_cleared[4] = {}, g_clear_replays = 0;

static void __cdecl h_clear() {
    if (!g_clearing) { g_clear_cur++; InterlockedIncrement(&g_cleared[pacing_kind() & 3]); }
    g_clear();
}

static bool install() {
    // FUN_0044d260: MOV ECX,[g_dwHdFrame]; JMP FUN_0046ac50 (rel32 checked by the pattern)
    g_clear_fn = sig_find("X2-HD-001 svc +0xEBC FUN_0044d260", "8B 0D ?? ?? ?? ?? E9 E5 D9 01 00", 0x44D260);
    if (!g_clear_fn) return false;
    log_line("  X2-HD-001: svc +0xEBC gva=0x%08X; hook at the first pre()", rt_to_gva(g_clear_fn));
    return true;
}

static void pre() {
    if (!g_hooked) {
        g_hooked = true;                         // game thread, between invocations
        if (!hook_entry("X2-HD-001", g_clear_fn, (void*)&h_clear, (void**)&g_clear)) {
            log_line("X2-HD-001: hook failed - row off");
            row_HD001.on = 0;
            return;
        }
    }
    g_clear_prev = g_clear_cur;
    g_clear_cur = 0;
}

static void post() {
    if (g_clear && !g_disabled && g_clear_cur == 0 && g_clear_prev > 0) {
        g_clearing = true;                       // after Sg_MainLoop, before the app render
        g_clear();
        g_clearing = false;
        InterlockedIncrement(&g_clear_replays);
    }
    static DWORD last = 0;
    static LONG last_c = 0;
    DWORD now = GetTickCount();
    LONG c = g_cleared[0] + g_cleared[1] + g_cleared[2] + g_cleared[3];
    if (now - last >= 10000 && c != last_c) {
        last = now; last_c = c;
        log_line("X2-HD-001: svc +0xEBC calls LOGIC %ld RENDER %ld BACKLOG %ld STOCK %ld, replayed %ld", g_cleared[INV_LOGIC],
                 g_cleared[INV_RENDER_ONLY], g_cleared[INV_BACKLOG], g_cleared[INV_STOCK], g_clear_replays);
    }
}

Row row_HD001 = {
    "X2-HD-001",
    "HD pass order: magic service +0xEBC (models before the glow chain) repeated on the invocation that skips it (Ultima glow blink)",
    1, install, pre, post, 0
};

} // namespace x2
