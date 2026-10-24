// SPDX-License-Identifier: Apache-2.0
// X2-FX-012: full-screen flashes (white washes from particle effects) show on every frame, not every second one.
//
// The particle op pppop_flash queues the full-screen tint for the frame, but only when particles are not paused. On
// render-only invocations X2-MAG-006 steps spell particles paused, so the tint showed on logic frames only: a 30 Hz
// flicker (Full Throttle's light flash). The fix removes that one test (the JNE is NOPed), so paused steps queue the
// same tint; its colour comes from particle data that paused steps do not change, and the op writes nothing else that
// a logic step would not write too. Evidence: re/ledger/LENS-GLOW.md §5.
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
