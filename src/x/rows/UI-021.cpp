// SPDX-License-Identifier: Apache-2.0
// UI-021: the title screen water is a 70-frame flipbook (menu/titl/GCM/Water_%02d.dds.phyre) drawn by FUN_0065b5b0
// (Sg_MainLoop -> FUN_0063e920), which steps its frame index DAT_00ccba09 (wraps at 0x46) and counter DAT_00ccba0a
// once per call, i.e. once per Sg_MainLoop invocation. At N passes the water ran N times faster (U 25/10/2026, 120 fps).
// On render-only invocations the two bytes are put back after the call: one frame per logic tick, as in stock.
// Details: docs/fps60/rows/UI-021.md
#include "../fps60.h"

typedef void(__cdecl* water_t)();

static uint8_t* g_entry = nullptr;
static uint8_t* g_frame = nullptr;   // DAT_00ccba09 (frame index), +1 = DAT_00ccba0a (counter)
static water_t  g_tramp = nullptr;
static bool     g_patched = false;
static volatile LONG g_held = 0;

// void(void), plain RET (0x65B6F2).
static void __cdecl h_water() {
    if (g_disabled || !row_UI021.on || rate_mult() <= 1 || pacing_kind() != INV_BUILD) {
        g_tramp();
        return;
    }
    uint8_t saved[2] = {0, 0};
    bool ok = safe_copy(saved, g_frame, 2);
    g_tramp();
    if (!ok) return;
    safe_copy(g_frame, saved, 2);
    if (InterlockedIncrement(&g_held) == 1)
        log_line("UI-021: title water frame held on a render-only invocation (frame %u)", saved[0]);
}

static bool install() {
    // FUN_0065b5b0: prologue, SUB ESP,0x29C, cookie; PUSH 0x100; LEA; PUSH; PUSH path; 2x MOV [EBP-..],0;
    // CALL FUN_00679820; MOVZX EAX, byte [DAT_00ccba09] (operand at +0x40)
    g_entry = sig_find_unique("UI-021 title water flipbook FUN_0065b5b0",
        "55 8B EC 81 EC 9C 02 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 68 00 01 00 00 8D 85 FC FE FF FF 50 68 ?? ?? ?? ?? "
        "C7 85 F4 FD FF FF 00 00 00 00 C7 85 F8 FD FF FF 00 00 00 00 E8 ?? ?? ?? ?? 0F B6 05",
        0x65B5B0);
    if (!g_entry) return false;
    g_frame = (uint8_t*)read_abs(g_entry + 0x40);
    // the step: MOV CL,[frame] (+0x118), INC byte [counter] (+0x11E): counter must be frame + 1
    if (g_entry[0x118] != 0x8A || g_entry[0x119] != 0x0D || (uint8_t*)read_abs(g_entry + 0x11A) != g_frame ||
        g_entry[0x11E] != 0xFE || g_entry[0x11F] != 0x05 || (uint8_t*)read_abs(g_entry + 0x120) != g_frame + 1) {
        log_line("  UI-021: frame step at +0x118 does not match - refused");
        return false;
    }
    log_line("  UI-021: FUN_0065b5b0 at gva=0x%08X, frame byte gva=0x%08X (expected 0x00CCBA09)",
             rt_to_gva_exe(g_entry), rt_to_gva_exe(g_frame));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("UI-021", g_entry, (void*)&h_water, (void**)&g_tramp)) {
        log_line("UI-021: entry hook failed - row off");
        row_UI021.on = 0;
    }
}

Row row_UI021 = {
    "UI-021",
    "title screen water flipbook: frame steps once per logic tick (held on render-only invocations)",
    1, install, pre, nullptr, 0
};
