// SPDX-License-Identifier: Apache-2.0
// X2-FX-012: the particle op pppop_flash FUN_008435a0 (calc slot +8, no draw op) queues the full-screen tint render
// command (FUN_0067f210 -> FUN_00449290 -> FUN_004d0e10, executed by the HD frame, whose FUN_004aec60 zeroes the tint
// alpha again after every frame). Its whole body sits behind `if (pause == 0)`, so on render-only invocations, where
// X2-MAG-006 steps DLL programs paused, no tint is queued: the white wash shows on logic frames only (30 Hz flicker).
// U 17/10/2026: Full Throttle (magic_0681) "light flash" from Paine, screenshots of alternate frames; D: X2-DIAG-FLASH,
// tint alpha at the HD frame non-zero on logic invocations only. Fix: NOP the JNE, so paused steps queue the same tint
// (the colour comes from particle data +0xA0..+0xA6, which paused steps do not change). The op writes nothing else that
// lasts except the draw slot pointer [0xD67F48] = FUN_0067f210's return, which is what a logic step writes too.
// Evidence: outer repo re/ledger/LENS-GLOW.md §5 (S + D).
#include "../x2.h"

namespace x2 {

extern Row row_FX012;
static uint8_t* g_fn = nullptr;

static bool install() {
    // PUSH EBP; MOV EBP,ESP; CMP [pause],0; MOV EAX,[EBP+0x10]; MOV EAX,[EAX+0xC]; MOV ECX,[EAX+4]; MOV EAX,[EBP+8];
    // JNE end; MOVZX EDX,word [ECX+EAX+0xA2]; PUSH ESI; MOVZX ESI,word [ECX+EAX+0xA4]
    g_fn = sig_find("X2-FX-012 pppop_flash",
        "55 8B EC 83 3D ?? ?? ?? ?? 00 8B 45 10 8B 40 0C 8B 48 04 8B 45 08 0F 85 ?? ?? ?? ?? 0F B7 94 01 A2 00 00 00 56 "
        "0F B7 B4 01 A4 00 00 00", 0x8435A0);
    if (!g_fn) return false;
    uint8_t* mag006 = mag006_entry();
    uint8_t* pause = abs_operand("particle pause flag", g_fn + 5);
    // the JNE goes to the shared epilogue POP EBP; RET (0x84368D)
    uint8_t* end = g_fn + 0x1C + *(int32_t*)(g_fn + 0x18);
    if (!pause || !sig_match(end, "5D C3")) {
        log_line("  X2-FX-012: pppop_flash gate not as proven - off");
        return false;
    }
    if (!mag006) log_line("  X2-FX-012: X2-MAG-006 is off (no paused steps) - patch is a no-op");
    log_line("  X2-FX-012: pppop_flash gva=0x%08X (0x008435A0), pause flag gva=0x%08X (0x01CF579C), JNE at +0x16 -> 0x%08X",
             rt_to_gva(g_fn), rt_to_gva(pause), rt_to_gva(end));
    return true;
}

static uint32_t g_orig16 = 0, g_orig18 = 0;
static bool g_applied = false, g_live = true;

static void pre() {
    static bool done = false;
    if (done || !g_fn) return;
    done = true;
    // game thread, between invocations: the op is not running while the 6 bytes are written
    g_orig16 = *(uint32_t*)(g_fn + 0x16);
    g_orig18 = *(uint32_t*)(g_fn + 0x18);
    if (!patch_u32(g_fn + 0x18, 0x90909090u) || !patch_u32(g_fn + 0x16, 0x90909090u)) {
        log_line("X2-FX-012: patch failed");
        return;
    }
    g_applied = true;
    log_line("X2-FX-012: pppop_flash gate NOPed (full-screen flash queued on paused render-only steps too)");
}

// X2-DEV-BISECT-AUTO (game thread, between invocations): stock gate back (off) or NOPed again (on).
void fx012_set_live(bool on) {
    if (!g_applied || on == g_live) return;
    g_live = on;
    if (on) { patch_u32(g_fn + 0x18, 0x90909090u); patch_u32(g_fn + 0x16, 0x90909090u); }
    else { patch_u32(g_fn + 0x18, g_orig18); patch_u32(g_fn + 0x16, g_orig16); }
}

Row row_FX012 = {
    "X2-FX-012",
    "pppop_flash queues its full-screen tint on paused render-only steps too (magic light flashes, 30 Hz flicker)",
    1, install, pre, nullptr, 0
};

} // namespace x2
