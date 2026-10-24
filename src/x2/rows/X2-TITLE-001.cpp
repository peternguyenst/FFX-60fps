// SPDX-License-Identifier: Apache-2.0
// X2-TITLE-001: the title screen ("New Game / Load") runs at its normal speed.
//
// The title screen is not one of the game's menus but a field map (map id 0x17), so X2-MENU-001 never gives it stock
// pacing and it ran at double speed. This row holds stock pacing while the map id (gva 0x00D61748) is 0x17. Every map
// change is logged, which gives map ids for bug reports. Evidence: re/ledger/MENU.md §8.
#include "../x2.h"

namespace x2 {

static volatile int32_t* g_map = nullptr;
static int32_t g_title = 0x17;
static int32_t g_last = -12345;
static bool    g_holding = false;
extern Row row_TITLE001;

static bool install() {
    // HD layer FUN_00466ef0: CMP dword [map id],0x17; JNZ; CALL turbo off
    uint8_t* at = sig_find("X2-TITLE-001 HD layer title-map test", "83 3D ?? ?? ?? ?? 17 75 49 E8", 0x466FCD);
    if (!at) return false;
    g_map = (volatile int32_t*)abs_operand("map id", at + 2);
    if (!g_map) return false;
    g_title = config_row("X2-TITLE-001-MAP", 0x17);
    log_line("  X2-TITLE-001: map id gva=0x%08X (expected 0x00D61748) = 0x%X; stock pacing while it is 0x%X",
             rt_to_gva((const void*)g_map), *g_map, g_title);
    return true;
}

static void pre() {
    int32_t m = *g_map;
    if (m != g_last) {
        log_line("X2-TITLE-001: map id 0x%X (was 0x%X)", m, g_last);
        g_last = m;
    }
    bool title = m == g_title;
    if (title == g_holding) return;
    g_holding = title;
    pacing_hold_stock("X2-TITLE-001 (title map)", title);
}

Row row_TITLE001 = {
    "X2-TITLE-001",
    "title screen (map 0x17) at stock pacing; logs map-id changes",
    1, install, pre, nullptr, 0
};

} // namespace x2
