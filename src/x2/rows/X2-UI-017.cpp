// SPDX-License-Identifier: Apache-2.0
// X2-UI-017: no button press is lost.
//
// The game reads the pad and works out which buttons were just pressed, by comparing with the previous read, on every
// invocation. A press that arrived on a render-only invocation was "just pressed" only there, and that invocation runs
// no game logic: the press was lost. Both calls of the pad read (in Sg_WaitFrameSync and Sg_MainLoop) go to a hook
// that reads the pad only on logic invocations. Evidence: re/ledger/PORT-A.md §4.
#include "../x2.h"

namespace x2 {

typedef void(__cdecl* latch_t)(void);            // void(void), plain RET 0x7465F3
static latch_t g_latch = nullptr;
static volatile LONG g_skipped = 0, g_passed = 0;
extern Row row_UI017;

static void __cdecl h_latch() {
    if (row_UI017.on && !g_disabled && render_only()) { InterlockedIncrement(&g_skipped); return; }
    InterlockedIncrement(&g_passed);
    g_latch();
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_skipped > 0) {
        reported = 1;
        log_line("X2-UI-017: first pad latch skipped on a render-only invocation (passed %ld)", g_passed);
    }
}

static bool install() {
    // Sg_WaitFrameSync: vblank += 2 (twice), then CALL the pad latch
    uint8_t* a = sig_find("X2-UI-017 latch (Sg_WaitFrameSync)",
        "D9 1D ?? ?? ?? ?? 83 05 ?? ?? ?? ?? 02 83 05 ?? ?? ?? ?? 02 E8 ?? ?? ?? ?? 80 3D", 0x606BE7);
    // Sg_MainLoop: CALL Sg_WaitFrameSync, or else the latch CALL
    uint8_t* b = sig_find("X2-UI-017 latch (Sg_MainLoop)",
        "E8 ?? ?? ?? ?? 83 C4 04 EB 05 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 83 F8 01", 0x60521A);
    if (!a || !b) return false;
    uint8_t* sites[2] = { a + 0x14, b + 0x0A };
    uint8_t* fn = call_target(sites[0]);
    if (!fn || call_target(sites[1]) != fn) {
        log_line("  X2-UI-017: the two latch CALLs do not share a target - refused");
        return false;
    }
    g_latch = (latch_t)fn;
    for (int i = 0; i < 2; i++) {
        if (!patch_call_rel32(sites[i], fn, (void*)&h_latch)) {
            if (i == 1) patch_call_rel32(sites[0], (void*)&h_latch, fn);
            log_line("  X2-UI-017: patch %d failed - refused", i);
            return false;
        }
    }
    log_line("  X2-UI-017: pad latch gva=0x%08X (expected 0x007464D0), CALLs gva=0x%08X/0x%08X -> stub",
             rt_to_gva(fn), rt_to_gva(sites[0]), rt_to_gva(sites[1]));
    return true;
}

Row row_UI017 = {
    "X2-UI-017",
    "pad latch (FUN_007464d0) only on logic invocations: presses are not lost on render-only frames",
    1, install, nullptr, post, 0
};

} // namespace x2
