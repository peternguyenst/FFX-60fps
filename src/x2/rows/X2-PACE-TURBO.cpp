// SPDX-License-Identifier: Apache-2.0
// X2-PACE-TURBO: the game's speed booster (x2 / x4) works at 60 fps.
//
// The booster makes the game run several logic steps per host frame (the backlog count): at stock 30 Hz, x2 gives 2 and
// x4 gives 4 per frame. At 60 Hz each frame is half as long, so the count is halved too (x2: 1, x4: 2), and our
// alternation still made every second invocation render-only: x2 played at normal speed and x4 at about x3. While the
// booster is engaged (its flag at gva 0xDEAF78, or the X2-DEV-TURBO key) every invocation runs logic instead, which
// gives exactly the stock speeds: 60 logic steps per second at x2, 120 at x4. Read in pre(), applied by the next
// invocation's pacing. Evidence: re/MAINLOOP.md §5.1.
#include "../x2.h"

namespace x2 {

extern Row row_PACETURBO;

static volatile int32_t* g_engaged = nullptr;   // [0xDEAF78]
static bool g_on = false;

static bool install() {
    // same body signature as X2-DEV-TURBO (FUN_0051c1b0, one hit); CMP [engaged],0 at +0x27
    uint8_t* f = sig_find("X2-PACE-TURBO booster delta scale FUN_0051c1b0",
        "55 8B EC 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? A2 ?? ?? ?? ?? 84 C0 74 ?? A1 ?? ?? ?? ?? 85 C0 "
        "74 ?? 83 3D ?? ?? ?? ?? 00 D9 04 85 ?? ?? ?? ?? D8 4D 08 D9 5D 08", 0x51C1B0);
    if (!f) return false;
    g_engaged = (volatile int32_t*)abs_operand("booster engaged", f + 0x29);
    if (!g_engaged) return false;
    log_line("  X2-PACE-TURBO: engaged flag gva=0x%08X (0x00DEAF78); booster -> logic on every invocation",
             rt_to_gva((const void*)g_engaged));
    return true;
}

static void pre() {
    bool on = *g_engaged != 0 || devturbo_held();
    if (on != g_on) {
        g_on = on;
        pacing_turbo(on);
        log_line("X2-PACE-TURBO: booster %s - %s", on ? "engaged" : "released",
                 on ? "logic on every invocation" : "60 Hz alternation");
    }
}

static void post() {
    if (!row_PACETURBO.on && g_on) { g_on = false; pacing_turbo(false); }
}

Row row_PACETURBO = {
    "X2-PACE-TURBO",
    "game booster (x2 / x4) at 60 Hz: logic on every invocation while it is engaged (x2 was 1x, x4 was 3x)",
    1, install, pre, post, 0
};

} // namespace x2
