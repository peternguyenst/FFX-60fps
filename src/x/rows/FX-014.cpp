// SPDX-License-Identifier: Apache-2.0
// FX-014: the effect time counter DAT_023328d4 (and its twin _DAT_023328d0) is incremented by FUN_007ff680 in the
// Sg_MainLoop second loop, max(exec,1) times per invocation, so render-only invocations advance it too and every
// wave/shimmer/UV phase read from it (ledger FX.md FX-014: FUN_007ec500, FUN_0080fb30, FUN_00a5f670, FUN_00a5fe00)
// runs N times faster (U 25/10/2026: title screen water fast at 120). Fix: roll both counters back after each
// render-only invocation, so they advance once per logic tick as in stock.
// Details: docs/fps60/rows/FX-014.md
#include "../fps60.h"

static bool install() {
    // FUN_007ff680: aligned-stack prologue, SUB ESP,0x1E8, cookie; INC [DAT_023328d4] (+0x26); INC [_DAT_023328d0] (+0x2C)
    uint8_t* f = sig_find_unique("FX-014 effect counter step FUN_007ff680",
        "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC E8 01 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC "
        "FF 05 ?? ?? ?? ?? FF 05 ?? ?? ?? ?? 56",
        0x7FF680);
    if (!f) return false;
    uint8_t* d4 = (uint8_t*)read_abs(f + 0x28);
    uint8_t* d0 = (uint8_t*)read_abs(f + 0x2E);
    if (d0 + 4 != d4) { log_line("  FX-014: counters not adjacent (0x%08X, 0x%08X) - refused", rt_to_gva_exe(d0), rt_to_gva_exe(d4)); return false; }
    log_line("  FX-014: effect counters gva=0x%08X..+8 (expected 0x023328D0)", rt_to_gva_exe(d0));
    return rollback_add_static("FX-014", d0, 8);
}

Row row_FX014 = {
    "FX-014",
    "effect time counter DAT_023328d4/d0 rolled back on render-only invocations (wave/shimmer phases at stock speed)",
    1, install, nullptr, nullptr, 0
};
