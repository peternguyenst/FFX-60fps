// SPDX-License-Identifier: Apache-2.0
// X2-UI-001: screen fades (to and from black, flashes) last as long as in the stock game.
//
// The fade update (Sg_ScreenFadeUpdate) runs on every invocation, so fades lasted half as long at 60 fps. Its CALL
// (gva 0x605E69) goes to a hook: on render-only invocations the fade still draws, then its state is put back, so it
// steps once per logic tick. Saved state: 5 fade slots and the active mask (gva 0x116D7B8, 0x54 bytes) and its two
// outputs (0x116CBA0, 0x10 bytes; 0xD50658, 4 bytes). Evidence: re/ledger/PORT-A.md §5.
#include "../x2.h"
#include <string.h>

namespace x2 {

typedef void(__cdecl* fade_t)(void);             // void(void), single RET 0x6D3AA0

static fade_t   g_fade = nullptr;
static uint8_t* g_state = nullptr;
static uint8_t* g_out = nullptr;
static uint8_t* g_out2 = nullptr;
static const uint32_t STATE_LEN = 0x54, OUT_LEN = 0x10, OUT2_LEN = 4;
static volatile LONG g_held = 0, g_passed = 0;
extern Row row_UI001;

static void __cdecl h_fade() {
    if (!row_UI001.on || g_disabled || !render_only()) {
        InterlockedIncrement(&g_passed);
        g_fade();
        return;
    }
    uint8_t s[STATE_LEN], o[OUT_LEN], o2[OUT2_LEN];
    memcpy(s, g_state, STATE_LEN);
    memcpy(o, g_out, OUT_LEN);
    memcpy(o2, g_out2, OUT2_LEN);
    g_fade();
    memcpy(g_state, s, STATE_LEN);
    memcpy(g_out, o, OUT_LEN);
    memcpy(g_out2, o2, OUT2_LEN);
    InterlockedIncrement(&g_held);
}

static bool install() {
    // Sg_MainLoop tail: profiler store, then CALL Sg_ScreenFadeUpdate, CALL FUN_006d49b0
    uint8_t* at = sig_find("X2-UI-001 fade call (Sg_MainLoop tail)",
        "01 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 00 00 00 00 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 0F 85", 0x605E59);
    if (!at) return false;
    uint8_t* site = at + 0x10;
    uint8_t* fn = call_target(site);
    // prologue with the active-mask test CMP [mask],EBX (mask operand at +0x13)
    if (!sig_match(fn, "55 8B EC 83 EC 10 53 33 DB 89 5D FC E8 ?? ?? ?? ?? 39 1D ?? ?? ?? ?? 75 0C 39 1D")) {
        log_line("  X2-UI-001: CALL target %p is not Sg_ScreenFadeUpdate (prologue mismatch) - refused", fn);
        return false;
    }
    uint8_t* mask = abs_operand("fade mask", fn + 0x13);
    uint8_t* sb = sig_find("X2-UI-001 fade slot base", "BE ?? ?? ?? ?? DD 05 ?? ?? ?? ?? 8B DF EB 0F", 0x6D3A12);
    uint8_t* ob = sig_find("X2-UI-001 fade outputs",
        "0F B6 46 FC A3 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 01 00 00 00 D9 1D", 0x6D3BF0);
    if (!mask || !sb || !ob) return false;
    // all three inside the fade function, and the layout as proven: slot 0 countdown = state+4, mask = state+0x50
    if (sb < fn || sb > fn + 0x400 || ob < fn || ob > fn + 0x600) {
        log_line("  X2-UI-001: slot/output scans are outside Sg_ScreenFadeUpdate - refused");
        return false;
    }
    uint8_t* state = (uint8_t*)(uintptr_t)(*(const uint32_t*)(sb + 1) - 4);
    if (state + 0x50 != mask) {
        log_line("  X2-UI-001: slot base %p and mask %p disagree with the slots-then-mask layout - refused", state, mask);
        return false;
    }
    uint8_t* out2 = abs_operand("fade type out", ob + 5);
    uint8_t* out = abs_operand("fade out", ob + 0x0B);
    if (!out || !out2 || *(const uint32_t*)(ob + 0x15) != (uint32_t)(uintptr_t)(out + 0xC)) {
        log_line("  X2-UI-001: fade outputs not as proven - refused");
        return false;
    }
    g_state = state;
    g_out = out;
    g_out2 = out2;
    g_fade = (fade_t)fn;
    if (!patch_call_rel32(site, fn, (void*)&h_fade)) {
        log_line("  X2-UI-001: patch failed");
        return false;
    }
    log_line("  X2-UI-001: CALL at gva=0x%08X -> stub; fade gva=0x%08X, state gva=0x%08X+0x%X (expected 0x0116D7B8), "
             "outputs gva=0x%08X (0x0116CBA0), 0x%08X (0x00D50658)",
             rt_to_gva(site), rt_to_gva(fn), rt_to_gva(g_state), STATE_LEN, rt_to_gva(g_out), rt_to_gva(g_out2));
    return true;
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_held > 0) {
        reported = 1;
        log_line("X2-UI-001: first render-only fade update held (held=%ld passed=%ld)", g_held, g_passed);
    }
}

Row row_UI001 = {
    "X2-UI-001",
    "screen fades (Sg_ScreenFadeUpdate) step once per logic tick; render-only frames draw with state restored",
    1, install, nullptr, post, 0
};

} // namespace x2
