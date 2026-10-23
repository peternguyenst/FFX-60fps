// SPDX-License-Identifier: Apache-2.0
// MAG-029: particle programs attached to battle effect units (FUN_008008b0, "RUN particle data", ledger FX-011) step
// through Ppp_ProgramStep from the EXE, so MAG-006 leaves them unpaused. The call comes once per invocation from the
// effect-unit walk, so on render-only invocations the program clock (prog+8 += prog+0x10) advances too: N steps per
// logic tick at N passes. U 24/10/2026 at 120 fps: battle orbs, Wakka's ball effect and enemy breath run much faster.
// This row puts that call on MAG-006's EXE caller list (paused on render-only invocations, draw still runs) and logs
// the calls per invocation kind. v3 of MAG-006 crashed pausing this caller (strstr(NULL) in the draw); that crash
// was a paused first step, which MAG-006-FRESH (v4) and MAG-019 now cover.
// Details: docs/fps60/rows/MAG-029.md
#include "../fps60.h"

void mag006_watch_caller(uint8_t* ret, volatile LONG* counts);

static volatile LONG g_calls[4];                 // Ppp_ProgramStep calls from the site, per InvKind
static uint32_t      g_inv[4];                   // invocations per InvKind
static bool          g_pause = false;

static bool install() {
    uint8_t* ppp = mag006_entry();
    if (!ppp) { log_line("  MAG-029: needs MAG-006 (its Ppp_ProgramStep hook) - refused"); return false; }
    // FUN_008008b0: MOV EAX,[..]; PUSH 0x30; PUSH EDI; MOV [EAX],imm; CALL Ppp_ProgramStep (+0x0E); MOV ESI,EAX (+0x13);
    // ADD ESP,8; TEST ESI,ESI; JZ; MOVZX EAX,word [EDI+0x1E]
    uint8_t* at = sig_find_unique("MAG-029 effect-unit program step call (FUN_008008b0)",
        "A1 ?? ?? ?? ?? 6A 30 57 C7 00 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B F0 83 C4 08 85 F6 74 ?? 0F B7 47 1E", 0x8008FE);
    if (!at) return false;
    if (call_target(at + 0x0E) != ppp) { log_line("  MAG-029: CALL at %p is not Ppp_ProgramStep - refused", at + 0x0E); return false; }
    uint8_t* ret = at + 0x13;
    g_pause = config_row("MAG-029-PAUSE", 1) != 0;
    if (g_pause && !mag006_pause_exe_caller(ret)) { log_line("  MAG-029: MAG-006 caller list full - refused"); return false; }
    mag006_watch_caller(ret, g_calls);
    log_line("  MAG-029: effect-unit program step call gva=0x%08X (return 0x%08X): %s", rt_to_gva_exe(at + 0x0E),
             rt_to_gva_exe(ret), g_pause ? "paused on render-only invocations via MAG-006" : "counted only (MAG-029-PAUSE=0)");
    return true;
}

// Every ~10 s of invocations: program steps per invocation of each kind. Stock pacing steps each program once per
// tick; at N passes an unpaused render-only step makes it N.
static void post() {
    int k = pacing_kind();
    if (k >= 0 && k < 4) g_inv[k]++;
    static uint32_t tick = 0;
    if (++tick % (300 * (uint32_t)rate_mult()) != 0) return;
    LONG c[4];
    for (int i = 0; i < 4; i++) c[i] = InterlockedExchange(&g_calls[i], 0);
    if (c[INV_LOGIC] || c[INV_BUILD] || c[INV_BACKLOG] || c[INV_STOCK])
        log_line("MAG-029: program steps per invocation: logic %.2f (%u inv), render-only %.2f (%u inv, %s), stock %.2f "
                 "(%u inv); %d passes per tick",
                 g_inv[INV_LOGIC] ? (double)c[INV_LOGIC] / g_inv[INV_LOGIC] : 0.0, g_inv[INV_LOGIC],
                 g_inv[INV_BUILD] ? (double)c[INV_BUILD] / g_inv[INV_BUILD] : 0.0, g_inv[INV_BUILD],
                 g_pause ? "paused" : "running",
                 g_inv[INV_STOCK] ? (double)c[INV_STOCK] / g_inv[INV_STOCK] : 0.0, g_inv[INV_STOCK], rate_mult());
    for (int i = 0; i < 4; i++) g_inv[i] = 0;
}

Row row_MAG029 = {
    "MAG-029",
    "battle effect-unit particle programs (FUN_008008b0): render-only draws do not step the program (orbs, breath)",
    1, install, nullptr, post, 0
};
