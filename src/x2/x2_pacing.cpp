// SPDX-License-Identifier: Apache-2.0
// 60 Hz pacing: the heart of the mod (same idea as the FFX mod's pacing.cpp).
//
// The stock game runs its main loop (Sg_MainLoop) once every second screen refresh: its frame divider is 2. We keep
// the divider at 1, so the loop runs on every refresh, and tell the game on every other run to skip its logic and only
// draw (the exec flag: 1 = run logic, 0 = draw only). The game logic still runs 30 times a second and the screen
// shows 60 frames. Each run is an "invocation"; pacing_pre() picks its kind (logic, render-only, backlog, stock;
// x2.h) just before it starts.
// Rules:
//   - the game asking for divider 2 (30 Hz) is swallowed while 60 Hz is on; asking for 1 passes
//   - backlog invocations (the game catching up, no draw) always run logic, without moving the alternation
//   - any row can hold stock pacing for a while (FMV, menu, load: pacing_hold_stock); holds nest
//   - the game's booster (X2-PACE-TURBO): every invocation runs logic
//   - X2-PACE-MENUDIV2 (Steam Deck "Fast Results"): see its comment further down
// Tested by tests/x2/test_pacing.cpp.
#include "x2.h"

namespace x2 {

static uint32_t g_game_divider = 0;        // last divider value the GAME asked for
static bool     g_game_divider_known = false;
static bool     g_was_60hz = false;
static int      g_stock_holds = 0;          // rows currently holding stock pacing
static int      g_next_is_logic = 1;         // 1 = next invocation is LOGIC
static int      g_inv_kind = INV_STOCK;
static uint64_t g_kind_counts[4] = { 0, 0, 0, 0 };
static uint64_t g_inv_count = 0;
static int      g_log_lines_left = 200;

#define PACING_LOG(...) do { if (g_cfg.log_pacing_events && g_log_lines_left > 0) { g_log_lines_left--; log_line(__VA_ARGS__); } } while (0)

static void set_exec(int v) {
    if (g_cfg.exec_method == 1 || !g_set_exec) *g_exec_byte = (int8_t)v;
    else g_set_exec(v);
}

static void set_divider(uint32_t v) {
    if (g_set_divider) g_set_divider(v);   // trampoline once hooked: bypasses our hook
}

int pacing_kind() { return g_inv_kind; }

void pacing_hold_stock(const char* who, bool hold) {
    g_stock_holds += hold ? 1 : -1;
    if (g_stock_holds < 0) g_stock_holds = 0;
    PACING_LOG("pacing: %s %s stock pacing (holders now %d)", who, hold ? "holds" : "releases", g_stock_holds);
}

// Give the game its stock pacing back now (divider, exec 1), e.g. when it enters load mode at the end of an
// invocation: no Sg_MainLoop runs during a load, so pacing_pre cannot do it. The next invocation re-enters 60 Hz.
void pacing_leave_now(const char* why) {
    if (!g_was_60hz) return;
    set_divider(g_game_divider ? g_game_divider : 2);
    set_exec(1);
    g_next_is_logic = 1;
    g_was_60hz = false;
    PACING_LOG("pacing: stock now for %s (divider -> %u, exec -> 1) at inv %llu", why, g_game_divider, g_inv_count);
}

static bool g_turbo_on = false;
void pacing_turbo(bool on) { g_turbo_on = on; }

// X2-DIAG-CAPTURE-PHASEFLIP: one repeated kind (two logic or two render-only invocations in a row) moves the logic /
// render-only alternation against the presented-frame count, to tell which one a blink follows.
static volatile LONG g_flip_requested = 0;
void pacing_flip_phase() { InterlockedExchange(&g_flip_requested, 1); }
// X2-DIAG-CAPTURE-BACKLOG: one no-draw logic invocation (backlog count 2, as the game's own catch-up) without moving
// the logic / render-only alternation (capture 19:10: the blink's sign changed at a hitch with a BACKLOG invocation,
// never at a forced repeat). A magic DLL that runs its effect from Mag_Draw does not step on it.
static volatile LONG g_backlog_requested = 0;
void pacing_inject_backlog() { InterlockedExchange(&g_backlog_requested, 1); }

void pacing_set(bool on) {
    if ((g_cfg.pacing != 0) == on) return;
    g_cfg.pacing = on ? 1 : 0;
    log_line("pacing: %s at runtime", on ? "60 Hz" : "stock 30 Hz");
}

// X2-PACE-MENUDIV2 ([rows], default on under Wine / Proton, off on Windows): while a row holds stock pacing (PS2
// menus), keep the game at divider 2 when it asks for 1. The battle results screen asks for 60 fps; on the Steam Deck
// (Proton) it then takes 6-10 s (stock ~4 s; PC near-instant): it ends when background work finishes, and that work
// needs main-thread idle time between frames (D 18/10 LOADWAITS: 8.5 ms per frame in the command-ring wait). At divider
// 2 it takes ~1 s, but alone its animation plays at half speed (the menu steps once per host frame). Skipping every second HD render
// or present at divider 1 did not help (8.3 s / 15 s): the frame loop rate itself is what matters. re/ledger/LOAD-DECK.md.
// Full-speed animation at 30 fps (U 18/10: works, ~1 s and normal animation): the menu steps in Menu_CoroutineIterate 0x7645E0 only when
// Sg_GetTurboFactor() <= the host-frame counter [0xDF6414], which the runner resets to 0 after each Sg_MainLoop. Each
// host frame starts with backlog count 2 (Sg_MainLoop: count >= 2 -> skip flags, count--: a no-draw invocation, then a
// drawn one) and the counter is set to 1 before the second, so the menu steps twice per rendered frame. The pad is
// latched per invocation, so one press is seen once.
static int g_opt_menudiv2 = -1;
static bool g_menu_kept_at_30 = false;                    // the game asked for divider 1 under a hold and we kept 2
static volatile int32_t* g_host_frame_counter = nullptr;    // [0xDF6414], from the runner (call site + 0xD: MOV ECX,[ctr])
// Config page "Fast Results" (X2-CFG-MENU): the default, and the runtime switch (the next divider request follows it).
int  pacing_menudiv2_default() { return on_wine() ? 1 : 0; }
bool pacing_menudiv2_get() {
    if (g_opt_menudiv2 < 0) g_opt_menudiv2 = config_row("X2-PACE-MENUDIV2", pacing_menudiv2_default());
    return g_opt_menudiv2 != 0;
}
void pacing_menudiv2_set(bool on) { g_opt_menudiv2 = on ? 1 : 0; }
bool pacing_divider_request(uint32_t req) {
    uint32_t prev = g_game_divider;
    if (req != 1) g_menu_kept_at_30 = false;
    if (pacing_menudiv2_get() && g_stock_holds > 0 && req == 1) {         // g_game_divider stays 2, so the stock path keeps divider 2
        g_menu_kept_at_30 = true;
        PACING_LOG("pacing: game asks divider 1 (wanted %u) under a stock hold -> swallowed, kept at 2 (X2-PACE-MENUDIV2)", prev);
        return true;
    }
    g_game_divider = req;
    g_game_divider_known = true;
    bool swallow = g_cfg.pacing && g_stock_holds == 0 && req >= 2;
    if (req != prev) PACING_LOG("pacing: game asks divider %u (was %u) -> %s", req, prev, swallow ? "swallowed, held at 1" : "passed");
    return swallow;
}

void pacing_pre() {
    g_inv_count++;
    if (!g_game_divider_known && g_divider_hooked) {       // the game set its divider before our hook existed
        g_game_divider = *g_divider;
        g_game_divider_known = true;
        log_line("pacing: initial game divider %u", g_game_divider);
    }

    bool want_active = g_cfg.pacing && g_divider_hooked && g_stock_holds == 0 && g_game_divider_known && g_game_divider >= 2;
    if (!want_active) {
        if (g_opt_menudiv2 && g_menu_kept_at_30 && g_stock_holds > 0) {
            if (!g_host_frame_counter && g_mainloop_callsite && sig_match(g_mainloop_callsite + 0xD, "8B 0D"))
                g_host_frame_counter = (volatile int32_t*)abs_operand("host frame counter", g_mainloop_callsite + 0xF);
            if (*g_backlog_count == 0) *g_backlog_count = 2;
            else if (*g_backlog_count == 1 && g_host_frame_counter && *g_host_frame_counter < 1) *g_host_frame_counter = 1;
        }
        if (g_was_60hz) {                          // leaving 60 Hz: give the game back its pacing
            set_divider(g_game_divider ? g_game_divider : 2);
            set_exec(1);
            g_next_is_logic = 1;
            PACING_LOG("pacing: stock (divider -> %u, exec -> 1) at inv %llu", g_game_divider, g_inv_count);
        }
        g_was_60hz = false;
        g_inv_kind = INV_STOCK;
        g_kind_counts[INV_STOCK]++;
        return;
    }

    if (!g_was_60hz) PACING_LOG("pacing: 60 Hz on at inv %llu (game divider %u, frame counter %u)", g_inv_count, g_game_divider,
                            g_frame_counter ? *g_frame_counter : 0);
    if (*g_divider != 1) {
        if (g_was_60hz) PACING_LOG("pacing: divider was %u at pre of inv %llu - re-asserted 1", *g_divider, g_inv_count);
        set_divider(1);
    }

    int target;
    if (InterlockedExchange(&g_backlog_requested, 0) && *g_backlog_count == 0) *g_backlog_count = 2;
    if (*g_backlog_count > 1) {
        target = 1;
        g_inv_kind = INV_BACKLOG;
    } else if (g_turbo_on) {                         // X2-PACE-TURBO: count = booster / 2 per host frame, all logic
        target = 1;
        g_next_is_logic = 1;
        g_inv_kind = INV_LOGIC;
    } else {
        if (InterlockedExchange(&g_flip_requested, 0)) g_next_is_logic ^= 1;
        target = g_next_is_logic;
        g_next_is_logic ^= 1;
        g_inv_kind = target ? INV_LOGIC : INV_RENDER_ONLY;
    }
    if (*g_exec_byte != target) set_exec(target);
    g_kind_counts[g_inv_kind]++;
    g_was_60hz = true;
}

void pacing_post() {
    if ((g_inv_count % 36000) == 0) {                      // ~10 min at 60 Hz
        log_line("pacing stats: inv=%llu LOGIC=%llu BUILD=%llu BACKLOG=%llu STOCK=%llu", g_inv_count,
                 g_kind_counts[INV_LOGIC], g_kind_counts[INV_RENDER_ONLY], g_kind_counts[INV_BACKLOG], g_kind_counts[INV_STOCK]);
    }
}

} // namespace x2
