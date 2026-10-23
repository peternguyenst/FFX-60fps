// SPDX-License-Identifier: Apache-2.0
// UI-020: the HD save/load list input handler FUN_006ef320 (7 callers in the save/load menu, 0x6E-0x6F) keeps one
// hold counter per input, DAT_00ce8184..DAT_00ce81a0 (up, down, left, right, two buttons, then two press-edge
// counters), and reports an input when its counter is 1 (press) or a multiple of 5 above 10 (held repeat). It runs
// once per invocation, so at N passes a held direction repeats N/2 times as fast as at stock (U 24/10/2026: the title
// Load Game cursor moves between slots much faster at 120 fps; D re/captures/fps60/loadmenu120: 0xCE8184 changes on
// render-only invocations). The press edges (FUN_00630cf0, BL1-002) are only valid for one invocation, so the call
// cannot be skipped. On render-only invocations the handler still runs, but a counter that was already held is put
// back (no advance), and only a fresh press or an edge is reported.
// Details: docs/fps60/rows/UI-020.md
#include "../fps60.h"

typedef int(__cdecl* input_t)(void);

static uint8_t*  g_entry = nullptr;
static input_t   g_tramp = nullptr;
static int32_t*  g_cnt = nullptr;                // the 8 counters, DAT_00ce8184
static bool      g_patched = false;
static volatile LONG g_held = 0;

static bool fires(int32_t c) { return c == 1 || (c > 10 && c % 5 == 0); }

// int(void), plain RET (0x6EF5F0): cdecl, no arguments.
static int __cdecl h_input() {
    if (g_disabled || !row_UI020.on || pacing_kind() != INV_BUILD) return g_tramp();
    int32_t before[8];
    for (int i = 0; i < 8; i++) before[i] = g_cnt[i];
    int r = g_tramp();
    // Level counters (0..5): a held input keeps its count; a fresh press (0 -> 1) or a release stands.
    bool fresh[8] = {};
    for (int i = 0; i < 6; i++) {
        if (before[i] == 0) { fresh[i] = g_cnt[i] == 1; continue; }
        if (g_cnt[i] != 0) {
            if (g_cnt[i] != before[i] && InterlockedIncrement(&g_held) == 1)
                log_line("UI-020: first held save/load list input kept on a render-only invocation");
            g_cnt[i] = before[i];
        }
    }
    // The game's own priority order: 1..6 for the level counters, then 7/8 for the edge counters (left as it set them).
    (void)r;
    for (int i = 0; i < 6; i++) if (fresh[i] && fires(g_cnt[i])) return i + 1;
    if (fires(g_cnt[6])) return 7;
    if (fires(g_cnt[7])) return 8;
    return 0;
}

static bool install() {
    // FUN_006ef320: CALL stick getter; PUSH 0x1000; MOV [DAT_00ce81a4],EAX; CALL level test; ...; MOV [DAT_00ce8184],0
    g_entry = sig_find_unique("UI-020 save/load list input FUN_006ef320",
        "E8 ?? ?? ?? ?? 68 00 10 00 00 A3 ?? ?? ?? ?? E8 ?? ?? ?? ?? D9 05 ?? ?? ?? ?? 83 C4 04 85 C0 75 1B "
        "A1 ?? ?? ?? ?? D8 58 04 DF E0 F6 C4 41 74 0E C7 05", 0x6EF320);
    if (!g_entry) return false;
    uint8_t* stick = read_abs(g_entry + 0x0B);
    int32_t* cnt = (int32_t*)read_abs(g_entry + 0x32);
    if (!cnt || stick != (uint8_t*)cnt + 0x20) {
        log_line("  UI-020: counter block %p / stick pointer %p do not line up - refused", (void*)cnt, (void*)stick);
        return false;
    }
    g_cnt = cnt;
    log_line("  UI-020: FUN_006ef320 at gva=0x%08X, counters gva=0x%08X (entry hooked on the game thread)",
             rt_to_gva_exe(g_entry), rt_to_gva_exe(cnt));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("UI-020", g_entry, (void*)&h_input, (void**)&g_tramp)) {
        log_line("UI-020: entry hook failed - row off");
        row_UI020.on = 0;
        return;
    }
    log_line("UI-020: FUN_006ef320 entry hooked on the game thread (tramp %p)", (void*)g_tramp);
}

Row row_UI020 = {
    "UI-020",
    "save/load list input FUN_006ef320: held-direction repeat counts logic ticks only (presses and edges kept)",
    1, install, pre, nullptr, 0
};
