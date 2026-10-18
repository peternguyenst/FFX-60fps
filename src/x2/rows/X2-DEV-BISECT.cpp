// SPDX-License-Identifier: Apache-2.0
// X2-DEV-BISECT (test tool, default 1 while testing): F8 (game window focused) turns the battle/effect rows off one at
// a time, so a visual bug can be pinned to a row without restarting: none -> X2-TEXANIM-001 -> X2-TASK-001 ->
// X2-MAG-025 -> X2-MAG-016 -> X2-MAG-020 -> X2-MAG-026 -> X2-CHR-001 -> X2-CHR-002 -> X2-MAG-032 -> X2-MAG-008 ->
// X2-MAG-006 -> X2-UI-001 -> X2-LOOP-004 -> X2-LOOP-005 -> X2-MAG-009 off -> none. Each press is logged. Rows whose
// patches do not check .on at runtime (FX-010-DRAWOPS, FX-012) and crash guards (MAG-019/027/030/031) are not listed.
// The toggle runs in post() of the last registered row, after every other row's post(), so a row is never switched
// between its own pre() and post(). Patches stay in place; a row that is off passes through (its stubs check .on).
// Used for the hit-effect blink (U 12/10/2026: "frame 1 flash, frame 2 off, frame 3 flash").
#include "../x2.h"

namespace x2 {

extern Row row_TEXANIM001, row_TASK001, row_MAG025, row_MAG016, row_MAG020, row_MAG026, row_CHR001, row_CHR002,
    row_MAG032, row_MAG008, row_MAG006, row_UI001, row_LOOP004, row_LOOP005, row_MAG009;
static Row* const g_list[] = { &row_TEXANIM001, &row_TASK001, &row_MAG025, &row_MAG016, &row_MAG020, &row_MAG026,
    &row_CHR001, &row_CHR002, &row_MAG032, &row_MAG008, &row_MAG006, &row_UI001, &row_LOOP004, &row_LOOP005,
    &row_MAG009 };
static const int N = (int)(sizeof(g_list) / sizeof(g_list[0]));
static int  g_was_on[N];
static int  g_step = 0;                          // 0 = all as installed, k = row k-1 off
static bool g_prev = false;

static bool focused() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static bool install() {
    for (int i = 0; i < N; i++) g_was_on[i] = g_list[i]->on;   // rows install before this one (registry order)
    log_line("  X2-DEV-BISECT: F8 cycles the battle rows off one at a time");
    return true;
}

static void post() {
    bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    bool edge = down && !g_prev;
    g_prev = down;
    if (!edge || !focused()) return;
    if (g_step > 0) g_list[g_step - 1]->on = g_was_on[g_step - 1];    // previous row back as installed
    g_step = (g_step + 1) % (N + 1);
    if (g_step == 0) { log_line("F8: X2-DEV-BISECT: all rows as installed"); return; }
    Row* r = g_list[g_step - 1];
    if (!g_was_on[g_step - 1]) { log_line("F8: X2-DEV-BISECT: %s was not installed - nothing to turn off", r->id); return; }
    r->on = 0;
    log_line("F8: X2-DEV-BISECT: step %d/%d: %s OFF (others as installed)", g_step, N, r->id);
}

Row row_DEVBISECT = {
    "X2-DEV-BISECT",
    "test tool: F8 turns battle rows off one at a time",
    1, install, nullptr, post, 0
};

} // namespace x2
