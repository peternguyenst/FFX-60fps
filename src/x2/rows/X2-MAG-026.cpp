// SPDX-License-Identifier: Apache-2.0
// X2-MAG-026: "family X" effect records move and animate at their normal speed.
//
// Family-X effect records (0x510-byte nodes) step inside their own draw callback: position, their particle program,
// model animation and sprite. X2-MAG-020 keeps the state handlers at logic rate but still calls each record's draw on
// render-only invocations, so flights and animations ran at double speed. On render-only invocations:
//   X2-MAG-026-PPP  the record's particle step (return address 0x7E958C) is paused through X2-MAG-006's list of game
//                   call sites to pause
//   X2-MAG-026-REC  each record's fields (+0x0C..+0x280 and +0x3B0..+0x510) are put back after its draw (X2-MAG-020
//                   calls mag026_draw_record); the particle block in between is left to the pause
// Evidence: re/ledger/FAMX.md; signatures tools/sigs_x2_famx.txt.
#include "../x2.h"
#include <string.h>

namespace x2 {

typedef void(__cdecl* node_fn_t)(uint8_t* node);

enum { REC_SIZE = 0x510, PROG_LO = 0x280, PROG_HI = 0x3B0, SENTINEL = 0xBC };

static int g_rec = 1;
static volatile LONG g_held = 0;
extern Row row_MAG026;

// Called by X2-MAG-020's draw-only walk on render-only invocations. Head and tail sentinels (0xBC bytes, the tail at
// head+0xBC: FUN_007ea8b0 "list empty" test) are drawn as they are.
void mag026_draw_record(uint8_t* head, uint8_t* nd, void* cb) {
    node_fn_t fn = (node_fn_t)cb;
    if (!g_rec || !row_MAG026.on || nd == head || nd == head + SENTINEL) { fn(nd); return; }
    static uint8_t save[REC_SIZE];
    memcpy(save, nd, REC_SIZE);
    fn(nd);
    memcpy(nd + 0x0C, save + 0x0C, PROG_LO - 0x0C);
    memcpy(nd + PROG_HI, save + PROG_HI, REC_SIZE - PROG_HI);
    InterlockedIncrement(&g_held);
}

static bool install() {
    if (!row_on("X2-MAG-020")) { log_line("  X2-MAG-026: needs X2-MAG-020 (its draw-only node walk) - refused"); return false; }
    g_rec = config_row("X2-MAG-026-REC", 1);
    if (config_row("X2-MAG-026-PPP", 1)) {
        uint8_t* ppp = mag006_entry();
        if (!ppp) { log_line("  X2-MAG-026: X2-MAG-026-PPP needs X2-MAG-006 (its Ppp_ProgramStep hook) - refused"); return false; }
        // FUN_007e93e0: PUSH 0x30; PUSH EBX; CALL Ppp_ProgramStep; ADD ESP,0x28; TEST EAX,EAX; JZ; PUSH EBX; CALL kill
        uint8_t* at = sig_find("X2-MAG-026 record step call (FUN_007e93e0)",
            "6A 30 53 E8 ?? ?? ?? ?? 83 C4 28 85 C0 74 0F 53 E8", 0x7E9584);
        if (!at) return false;
        if (call_target(at + 3) != ppp) { log_line("  X2-MAG-026: CALL at %p is not Ppp_ProgramStep - refused", at + 3); return false; }
        if (!mag006_pause_exe_caller(at + 8)) { log_line("  X2-MAG-026: X2-MAG-006 caller list full - refused"); return false; }
        log_line("  X2-MAG-026: record program step call gva=0x%08X (return 0x%08X, expected 0x007E958C) paused on "
                 "render-only invocations via X2-MAG-006", rt_to_gva(at + 3), rt_to_gva(at + 8));
    }
    log_line("  X2-MAG-026: record fields restored after render-only draws: %s", g_rec ? "yes" : "no (X2-MAG-026-REC=0)");
    return true;
}

static void post() {
    static DWORD last = 0;
    static LONG seen = 0;
    DWORD now = GetTickCount();
    if (now - last >= 10000 && g_held != seen) {
        last = now;
        seen = g_held;
        log_line("X2-MAG-026: family-X record draws held on render-only invocations so far: %ld", seen);
    }
}

Row row_MAG026 = {
    "X2-MAG-026",
    "family-X effect records (FUN_007ea5e0): render-only draws do not step programs, motion or animation",
    1, install, nullptr, post, 0
};

} // namespace x2
