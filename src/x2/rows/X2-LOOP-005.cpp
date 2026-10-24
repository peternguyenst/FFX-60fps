// SPDX-License-Identifier: Apache-2.0
// X2-LOOP-005: the camera does not ease twice per tick.
//
// On invocations that skip logic (our render-only frames), the main loop eases the camera again when a debug byte
// (gva 0xDF67E8) is set, which it is in the retail game. So the camera moved at double speed. The byte is cleared for
// render-only invocations and put back after. Evidence: re/ledger/PORT-A.md §2.
#include "../x2.h"

namespace x2 {

static volatile uint8_t* g_flag = nullptr;
static uint8_t g_saved = 0;
static bool    g_cleared = false;
static volatile LONG g_skips = 0;

static bool install() {
    uint8_t* at = sig_find("X2-LOOP-005 exec==0 camera fallback",
        "E8 ?? ?? ?? ?? 85 C0 75 1C 50 E8 ?? ?? ?? ?? 83 C4 04 80 3D ?? ?? ?? ?? 00 74 0A E8 ?? ?? ?? ?? E8", 0x605823);
    if (!at) return false;
    if (!g_get_exec || call_target(at) != g_get_exec) {
        log_line("  X2-LOOP-005: first CALL is not Sg_GetCurExecFrames - refused");
        return false;
    }
    g_flag = (volatile uint8_t*)abs_operand("camera gate", at + 0x14);
    if (!g_flag) return false;
    log_line("  X2-LOOP-005: gate = %u (expected gva 0x00DF67E8), camera calls gva=0x%08X / 0x%08X", *g_flag,
             rt_to_gva(call_target(at + 0x1B)), rt_to_gva(call_target(at + 0x20)));
    return true;
}

static void pre() {
    g_cleared = false;
    if (g_disabled || !render_only()) return;
    g_saved = *g_flag;
    if (!g_saved) return;
    *g_flag = 0;
    g_cleared = true;
    InterlockedIncrement(&g_skips);
}

static void post() {
    if (g_cleared && *g_flag == 0) *g_flag = g_saved;
    g_cleared = false;
    static LONG reported = 0;
    if (!reported && g_skips > 0) {
        reported = 1;
        log_line("X2-LOOP-005: first render-only invocation with the fallback camera ease suppressed");
    }
}

Row row_LOOP005 = {
    "X2-LOOP-005",
    "exec==0 camera fallback (FUN_006ef4d0 + FUN_00651fd0) suppressed on render-only invocations",
    1, install, pre, post, 0
};

} // namespace x2
