// SPDX-License-Identifier: Apache-2.0
// X2-DEV-BISECT (test tool): F8 turns the battle / effect rows off one at a time, to find which row causes a visual bug
// without restarting.
//
// Each F8 press moves to the next step and is logged: none -> X2-MAG-006 off -> X2-UI-001 -> X2-LOOP-004 ->
// X2-LOOP-005 -> X2-MAG-009 -> none. Not in the list: X2-MAG-008 and X2-MAG-025 (switching them off mid-battle made a
// spell overflow the effect heap), rows whose patches do not check their on switch (X2-FX-010-DRAWOPS, X2-FX-012) and
// the crash guards. The switch happens in post() of the last row, after every other row's post(), so a row is never
// switched between its own pre() and post(). Patches stay in place; a row that is off passes straight through.
// X2-DEV-BISECT-AUTO=<seconds> (off): while an X2-DIAG-FTBLINK-DLL spell is loaded, the steps advance by themselves
// every <seconds>, with a baseline step (everything on) before each test step, so one cast tests every row; everything
// goes back when the DLL unloads. The current step is in g_bisect_step (X2-DIAG-CAPTURE logs it).
#include "../x2.h"
#include "x2_smooth.h"

namespace x2 {

extern Row row_MAG006, row_UI001, row_LOOP004, row_LOOP005, row_MAG009;
bool ftblink_watching();
static Row* const g_list[] = { &row_MAG006, &row_UI001, &row_LOOP004, &row_LOOP005, &row_MAG009 };
static const int N = (int)(sizeof(g_list) / sizeof(g_list[0]));
static int  g_was_on[N];
static int  g_step = 0;                          // 0 = all as installed, k = row k-1 off
static bool g_prev = false;

// AUTO: test steps 1..NAUTO, the baseline 0 before each one
// X2-DEV-BISECT-AUTO-SET=2 (capture 19:10: the blink is logic vs render-only content, sign set per cast): the rows
// that change what a render-only frame draws and that the first set left out: FX-010-DRAWOPS (+TAIL3) and FX-012
// (byte patches, switched through their set_live), and the two FX rows together.
static const char* const AUTO_NAME[] = { "baseline", "SMOOTH all off", "SMOOTH-001 off", "SMOOTH-002 off",
                                         "SMOOTH-003 off", "MAG-006 off", "UI-001 off", "LOOP-004 off", "LOOP-005 off",
                                         "MAG-009 off", "FX-010-DRAWOPS off", "FX-012 off", "FX-010 + FX-012 off" };
enum { NAUTO = 9, NAUTO2 = 3 };
static int g_auto_ms = 0, g_auto_i = 0;          // position in the cycle 0..2*n-1 (odd = test step)
static int g_auto_set = 1;
static ULONGLONG g_auto_t = 0;
static bool g_auto_active = false;
volatile int g_bisect_step = 0;                  // 0 = baseline, 1..12 = test step (X2-DIAG-CAPTURE)
void fx010_set_live(bool on);
void fx012_set_live(bool on);

// X2-DEV-BISECT-AUTO-SET=3 (outer repo re/ledger/HDNODE.md §0: the HD camera takes the first commit per HD frame, the
// game's raw view on logic frames, X2-SMOOTH-001's on render-only ones; a view that zigzags would pulse the sphere's
// screen coverage): SMOOTH-001 off, then all SMOOTH off.
static int auto_n() { return g_auto_set == 2 ? NAUTO2 : g_auto_set == 3 ? 2 : NAUTO; }
static int auto_step() {
    if (!(g_auto_i & 1)) return 0;
    int j = g_auto_i / 2;
    return g_auto_set == 2 ? 10 + j : g_auto_set == 3 ? (j == 0 ? 2 : 1) : j + 1;
}
static void auto_apply(int s) {
    smooth_set_live(s != 1);
    smooth_set_off_row(s >= 2 && s <= 4 ? s - 1 : 0);
    for (int i = 0; i < N; i++) g_list[i]->on = g_was_on[i] && s != 5 + i;
    fx010_set_live(s != 10 && s != 12);
    fx012_set_live(s != 11 && s != 12);
    g_bisect_step = s;
}

static bool focused() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static bool install() {
    for (int i = 0; i < N; i++) g_was_on[i] = g_list[i]->on;   // rows install before this one (registry order)
    g_auto_ms = config_row("X2-DEV-BISECT-AUTO", 0) * 1000;
    { int s = config_row("X2-DEV-BISECT-AUTO-SET", 1); g_auto_set = (s == 2 || s == 3) ? s : 1; }
    log_line("  X2-DEV-BISECT: F8 cycles the battle rows off one at a time%s",
             g_auto_ms ? "; AUTO: steps every few seconds while the watched magic DLL is loaded" : "");
    return true;
}

static void post() {
    if (g_auto_ms) {
        bool w = ftblink_watching();
        ULONGLONG now = GetTickCount64();
        if (w && !g_auto_active) {
            g_auto_active = true; g_auto_t = now;
            auto_apply(auto_step());
            log_line("X2-DEV-BISECT-AUTO: step %d %s (DLL loaded)", auto_step(), AUTO_NAME[auto_step()]);
        }
        if (!w && g_auto_active) { g_auto_active = false; auto_apply(0); log_line("X2-DEV-BISECT-AUTO: all as installed (DLL unloaded)"); }
        if (g_auto_active && now - g_auto_t >= (ULONGLONG)g_auto_ms) {
            g_auto_t = now;
            g_auto_i = (g_auto_i + 1) % (2 * auto_n());
            auto_apply(auto_step());
            log_line("X2-DEV-BISECT-AUTO: step %d %s", auto_step(), AUTO_NAME[auto_step()]);
        }
        return;
    }
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
