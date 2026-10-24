// SPDX-License-Identifier: Apache-2.0
// X2-HD-001: the HD renderer draws its passes in the same order on logic and render-only frames (fixed Ultima's
// blinking sphere glow).
//
// The HD frame render (Hd_FrameRender) draws the models either before or after its post-processing chain (depth and
// glow), depending on a flag (hdframe+0xE920). The game sets that flag at every invocation, and magic service +0xEBC
// (Hd_PassOrderModelsFirst) clears it. Ultima's effect calls +0xEBC from its logic step only, so render-only frames
// kept the flag set: the glow was drawn over a black scene on every second frame.
// The fix: when an invocation made no +0xEBC call but the one before did, this row makes the call once at the end of
// the invocation (before the frame is rendered), so render-only frames use the logic frame's pass order. It covers any
// spell or script op that calls +0xEBC on logic ticks only. Evidence: re/ledger/ULTIMA-0106.md;
// signature tools/sigs_x2_capture.txt.
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
