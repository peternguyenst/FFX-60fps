// SPDX-License-Identifier: Apache-2.0
// X2-PACE-TURBO: the game's booster (x2 / x4) at 60 Hz. Sg_MainLoop scales its delta by the booster factor
// (Sg_TurboDeltaScale FUN_0051c1b0, 0x60514A) and Sg_WaitFrameSync FUN_00606900 turns the delta into the backlog count:
// vblanks = (carry + delta) * 60, count 1 below 2 vblanks, else ftol(vblanks / 2) with the rest carried (S, MAINLOOP §5.1).
// Stock (divider 2, delta 1/30 s): x2 -> count 2, x4 -> count 4 logic steps per host frame. At 60 Hz the delta is 1/60 s:
// x2 -> count 1 (no backlog) and the exec alternation still runs logic on every second invocation = 1x; x4 -> count 2 =
// 1 backlog + 1/2 = 1.5 per host frame = 3x (U 21/10/2026: "x2 no longer works, x4 is maybe x2"). The count is already
// factor / 2 per 60 Hz host frame, so while the booster is engaged every invocation runs logic (no render-only ones):
// x2 = 60 logic steps/s, x4 = 120/s, as stock. Engaged = [0xDEAF78] (set by the scale function while the booster is on
// and allowed, S) or the X2-DEV-TURBO key held; read in pre(), used by the next invocation's pacing_pre().
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
