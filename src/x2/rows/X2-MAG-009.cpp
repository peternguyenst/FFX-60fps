// SPDX-License-Identifier: Apache-2.0
// X2-MAG-009: spells that time themselves off the frame counter keep their timing.
//
// The game's frame counter counts every invocation, so twice per tick at 60 fps, and spell DLLs time their effects
// with it. The service table spell DLLs receive when they start (gva 0xD6E5A0) is pointed at our own counter, which
// advances only on logic invocations: slot +0xC08 (the counter's address; every DLL copies it, 37 read it) and slot
// +0x3E0 (the counter getter; 12 DLLs call it). Patched at install, before any spell DLL starts.
// Evidence: re/ledger/PORT-A.md §7.
#include "../x2.h"

namespace x2 {

typedef void* (__cdecl* fc_getter_t)(int32_t* out);   // if (out) *out = counter; returns &DAT_00df6530

static volatile int32_t g_logic_fc = 0;
static uint32_t    g_fc_before = 0;
static fc_getter_t g_getter_orig = nullptr;
extern Row row_MAG009;

static void* __cdecl h_fc_getter(int32_t* out) {
    void* r = g_getter_orig(out);
    if (out && row_MAG009.on && !g_disabled) *out = g_logic_fc;
    return r;
}

static bool install() {
    if (!g_frame_counter) { log_line("  X2-MAG-009: frame counter not resolved - refused"); return false; }
    // MagicFile_Start: PUSH table; MOV [..],1; CALL [DLL start]
    uint8_t* at = sig_find("X2-MAG-009 MagicFile_Start table push",
        "0F 94 C0 50 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 01 00 00 00 FF 15", 0x8578D1);
    if (!at) return false;
    uint8_t* table = abs_operand("service table", at + 0x0A);
    if (!table) return false;
    uint32_t* slot_ptr = (uint32_t*)(table + 0xC08);
    uint32_t* slot_get = (uint32_t*)(table + 0x3E0);
    if (*slot_ptr != (uint32_t)(uintptr_t)g_frame_counter) {
        log_line("  X2-MAG-009: table+0xC08 = 0x%08X, not the frame counter %p - refused", *slot_ptr, g_frame_counter);
        return false;
    }
    uint8_t* getter = (uint8_t*)(uintptr_t)*slot_get;
    if (!in_image(getter) || !sig_match(getter, "55 8B EC 8B 4D 08 85 C9 74 07 A1") ||
        *(const uint32_t*)(getter + 11) != (uint32_t)(uintptr_t)g_frame_counter) {
        log_line("  X2-MAG-009: table+0x3E0 = %p is not the frame-counter getter - refused", getter);
        return false;
    }
    g_getter_orig = (fc_getter_t)getter;
    g_logic_fc = (int32_t)*g_frame_counter;
    if (!patch_u32(slot_ptr, (uint32_t)(uintptr_t)&g_logic_fc) || !patch_u32(slot_get, (uint32_t)(uintptr_t)&h_fc_getter))
        return false;
    log_line("  X2-MAG-009: service table gva=0x%08X: +0xC08 -> logic counter %p, +0x3E0 getter gva=0x%08X -> stub",
             rt_to_gva(table), (void*)&g_logic_fc, rt_to_gva(getter));
    return true;
}

static void pre() { g_fc_before = *g_frame_counter; }

static void post() {
    uint32_t d = *g_frame_counter - g_fc_before;
    if (!render_only() && d < 16) g_logic_fc += (int32_t)d;
}

Row row_MAG009 = {
    "X2-MAG-009",
    "magic DLLs read a logic-rate frame counter (service table +0xC08 pointer, +0x3E0 getter)",
    1, install, pre, post, 0
};

} // namespace x2
