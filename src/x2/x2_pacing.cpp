// SPDX-License-Identifier: Apache-2.0
// 60 Hz pacing for FFX-2, same policy as the FFX mod (pacing.cpp there): hold the frame divider at 1 and
// alternate exec 1/0, so every other Sg_MainLoop invocation is render-only. Game requests for 30 Hz are
// swallowed; requests for every-vsync pass through. Backlog invocations (skip-render catch-up) run logic.
#include "x2.h"

namespace x2 {

static uint32_t g_wanted = 0;        // last divider value the GAME asked for
static bool     g_wanted_known = false;
static bool     g_was_active = false;
static int      g_hold = 0;          // rows currently holding stock pacing
static int      g_phase = 1;         // 1 = next invocation is LOGIC
static int      g_kind = INV_STOCK;
static uint64_t g_count[4] = { 0, 0, 0, 0 };
static uint64_t g_inv = 0;
static int      g_log_budget = 200;

#define PLOG(...) do { if (g_cfg.log_pacing_events && g_log_budget > 0) { g_log_budget--; log_line(__VA_ARGS__); } } while (0)

static void set_exec(int v) {
    if (g_cfg.exec_method == 1 || !g_set_exec) *g_exec_byte = (int8_t)v;
    else g_set_exec(v);
}

static void set_divider(uint32_t v) {
    if (g_set_divider) g_set_divider(v);   // trampoline once hooked: bypasses our hook
}

int pacing_kind() { return g_kind; }

void pacing_hold_stock(const char* who, bool hold) {
    g_hold += hold ? 1 : -1;
    if (g_hold < 0) g_hold = 0;
    PLOG("pacing: %s %s stock pacing (holders now %d)", who, hold ? "holds" : "releases", g_hold);
}

// Give the game its stock pacing back now (divider, exec 1), e.g. when it enters load mode at the end of an
// invocation: no Sg_MainLoop runs during a load, so pacing_pre cannot do it. The next invocation re-enters 60 Hz.
void pacing_leave_now(const char* why) {
    if (!g_was_active) return;
    set_divider(g_wanted ? g_wanted : 2);
    set_exec(1);
    g_phase = 1;
    g_was_active = false;
    PLOG("pacing: stock now for %s (divider -> %u, exec -> 1) at inv %llu", why, g_wanted, g_inv);
}

void pacing_set(bool on) {
    if ((g_cfg.pacing != 0) == on) return;
    g_cfg.pacing = on ? 1 : 0;
    log_line("pacing: %s at runtime", on ? "60 Hz" : "stock 30 Hz");
}

bool pacing_divider_request(uint32_t req) {
    uint32_t prev = g_wanted;
    g_wanted = req;
    g_wanted_known = true;
    bool swallow = g_cfg.pacing && g_hold == 0 && req >= 2;
    if (req != prev) PLOG("pacing: game asks divider %u (was %u) -> %s", req, prev, swallow ? "swallowed, held at 1" : "passed");
    return swallow;
}

void pacing_pre() {
    g_inv++;
    if (!g_wanted_known && g_divider_hooked) {       // the game set its divider before our hook existed
        g_wanted = *g_divider;
        g_wanted_known = true;
        log_line("pacing: initial game divider %u", g_wanted);
    }

    bool want_active = g_cfg.pacing && g_divider_hooked && g_hold == 0 && g_wanted_known && g_wanted >= 2;
    if (!want_active) {
        if (g_was_active) {                          // leaving 60 Hz: give the game back its pacing
            set_divider(g_wanted ? g_wanted : 2);
            set_exec(1);
            g_phase = 1;
            PLOG("pacing: stock (divider -> %u, exec -> 1) at inv %llu", g_wanted, g_inv);
        }
        g_was_active = false;
        g_kind = INV_STOCK;
        g_count[INV_STOCK]++;
        return;
    }

    if (!g_was_active) PLOG("pacing: 60 Hz on at inv %llu (game divider %u, frame counter %u)", g_inv, g_wanted,
                            g_frame_counter ? *g_frame_counter : 0);
    if (*g_divider != 1) {
        if (g_was_active) PLOG("pacing: divider was %u at pre of inv %llu - re-asserted 1", *g_divider, g_inv);
        set_divider(1);
    }

    int target;
    if (*g_backlog_count > 1) {
        target = 1;
        g_kind = INV_BACKLOG;
    } else {
        target = g_phase;
        g_phase ^= 1;
        g_kind = target ? INV_LOGIC : INV_BUILD;
    }
    if (*g_exec_byte != target) set_exec(target);
    g_count[g_kind]++;
    g_was_active = true;
}

void pacing_post() {
    if ((g_inv % 36000) == 0) {                      // ~10 min at 60 Hz
        log_line("pacing stats: inv=%llu LOGIC=%llu BUILD=%llu BACKLOG=%llu STOCK=%llu", g_inv,
                 g_count[INV_LOGIC], g_count[INV_BUILD], g_count[INV_BACKLOG], g_count[INV_STOCK]);
    }
}

} // namespace x2
