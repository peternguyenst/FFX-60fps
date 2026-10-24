// SPDX-License-Identifier: Apache-2.0
// X2-UI-014: the minimap's player arrow pulses at its normal speed.
//
// The minimap draw (Ui_MinimapDraw) runs on every invocation and adds 1 to the arrow's pulse counter each time, so the
// arrow pulsed twice as fast. On render-only invocations the draw still runs and the counter is put back afterwards.
// Evidence: re/ledger/PORT-B.md §3.
#include "../x2.h"

namespace x2 {

typedef void(__cdecl* draw_t)(uint8_t* obj, int mode);   // cdecl(obj, mode), plain RET, callers clean

static uint8_t* g_entry = nullptr;
static draw_t   g_tramp = nullptr;
static bool     g_patched = false;
static volatile LONG g_held = 0;
extern Row row_UI014;

static void __cdecl h_draw(uint8_t* obj, int mode) {
    uint32_t saved = 0;
    bool hold = !g_disabled && render_only() && obj && safe_read_u32(obj + 0x74, &saved);
    g_tramp(obj, mode);
    if (hold) {
        *(uint32_t*)(obj + 0x74) = saved;
        if (InterlockedIncrement(&g_held) == 1) log_line("X2-UI-014: first minimap arrow pulse held on a render-only invocation");
    }
}

static bool install() {
    // aligned-stack prologue, identical to FFX
    g_entry = sig_find("X2-UI-014 Ui_MinimapDraw",
        "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 68 03 00 00 A1 ?? ?? ?? ?? 33 C5",
        0x7CB920);
    if (!g_entry) return false;
    // the pulse increment this row compensates: MOV EAX,[obj copy]; INC dword [EAX+0x74] at +0x12AA
    if (!sig_match(g_entry + 0x12A4, "8B 85 E4 FC FF FF FF 40 74 E8")) {
        log_line("  X2-UI-014: no INC [EAX+0x74] at +0x12AA - refused");
        return false;
    }
    log_line("  X2-UI-014: Ui_MinimapDraw gva=0x%08X (entry hooked on the game thread), pulse INC at gva=0x%08X",
             rt_to_gva(g_entry), rt_to_gva(g_entry + 0x12AA));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("X2-UI-014", g_entry, (void*)&h_draw, (void**)&g_tramp)) {
        log_line("X2-UI-014: entry hook failed - row off");
        row_UI014.on = 0;
    }
}

Row row_UI014 = {
    "X2-UI-014",
    "minimap player-arrow pulse (Ui_MinimapDraw obj+0x74) steps once per logic tick",
    1, install, pre, nullptr, 0
};

} // namespace x2
