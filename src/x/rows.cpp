// SPDX-License-Identifier: Apache-2.0
// Row registry: one entry per ledger-row mitigation, each toggled by its row ID in [rows] and logging one
// install line. How to add a row: README.md "Adding a row".
#include "fps60.h"
#include <string.h>

// ---------------------------------------------------------------- row table
// Order follows re/TRIAGE.md.
static Row* g_rows[] = {
    &row_RATE001,                                // before everything: sets the host frame rate
    &row_MAG008,
    &row_W2A002,
    &row_CLK002,
    &row_CLK013,
    &row_LOOP005,
    &row_LOOP004,
    &row_MAG006,
    &row_MAG009,
    &row_MAG016,
    &row_MAG017,
    &row_MAG018,
    &row_MAG019,
    &row_MAG020,
    &row_MAG021,
    &row_MAG022,
    &row_MAG023,
    &row_MAG024,
    &row_MAG025,
    &row_MAG026,
    &row_MAG027,
    &row_MAG029,                                 // after MAG-006 (its Ppp_ProgramStep entry)
    &row_MAG030,
    &row_DIAGFRAMETIME,
    &row_UI001,
    &row_UI017,
    &row_UISPH001,
    &row_UI020,
    &row_UISPH002,
    &row_UI014,
    &row_UIBTLL1,
    &row_TEXANIM001,
    &row_SR003,
    &row_DEVTURBO,
    &row_DEVSTOCK,
    &row_DEVDMG,
    &row_PERF001,
    &row_PERF002,
    &row_BOOST001,
    &row_QOLSENSOR,
    &row_QOLAP,
    &row_QOLRETRY,
    &row_QOLSAVEICON,
    &row_CFGMENU,                                // after the rows it switches
    &row_FX010,
    &row_MAG028,                                 // after MAG-006 / FX-010 (they resolve the pause flag)
    &row_DIAG_BTLSIG,
    &row_DIAG_DRAWSTEP,
    &row_DIAG_TEXTDUMP,
    &row_DIAG_HANGDUMP,
    &row_DIAG_PAUSEWATCH,
    &row_DIAG_DYNGEO,
    &row_LAB001,
    &row_SMOOTH001,                              // after LOOP-004/LOOP-005 (install reads row_on)
    &row_SMOOTH002,
    &row_SMOOTH003,
    nullptr
};

void rows_install_all() {
    int n = 0, on = 0;
    for (Row** p = g_rows; *p; p++) {
        Row& r = **p;
        n++;
        r.on = 0;
        if (!config_row(r.id, r.default_on)) {
            log_line("row %-10s OFF (ini) - %s", r.id, r.summary);
            continue;
        }
        bool ok = false;
        __try { ok = r.install ? r.install() : true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        r.on = r.installed = ok ? 1 : 0;
        if (ok) on++;
        log_line("row %-10s %s - %s", r.id, ok ? "ON" : "FAILED to install, stays off", r.summary);
    }
    log_line("rows: %d registered, %d on", n, on);
}

bool row_on(const char* id) {
    for (Row** p = g_rows; *p; p++) if (strcmp((*p)->id, id) == 0) return (*p)->on != 0;
    return true;                                 // not a registered row: never gated
}

bool row_set(Row& r, bool on) {
    if (on == (r.on != 0)) return on;
    if (on) {
        if (!r.installed) {
            bool ok = false;
            __try { ok = r.install ? r.install() : true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
            r.installed = ok ? 1 : 0;
            log_line("row %-10s %s at runtime - %s", r.id, ok ? "installed" : "FAILED to install, stays off", r.summary);
            if (!ok) return false;
        }
        r.on = 1;
    } else {
        if (r.stop) {
            __try { r.stop(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { log_line("rows: %s stop faulted", r.id); }
        }
        r.on = 0;
    }
    log_line("row %-10s %s at runtime", r.id, on ? "ON" : "OFF");
    return on;
}

// Diagnostic hotkey F8: turn the camera rows (LOOP-005, LOOP-004) off/on at runtime, to tell whether
// they cause a soft-lock (Ixion summon, 10/10/2026). Only rows that installed can be turned back on.
static void hotkey_camera_rows() {
    static bool prev = false;
    static int  installed[2] = { -1, -1 };
    Row* rows[2] = { &row_LOOP005, &row_LOOP004 };
    bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    bool edge = down && !prev;
    prev = down;
    if (!edge) return;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;
    for (int i = 0; i < 2; i++) if (installed[i] < 0) installed[i] = rows[i]->on;
    int v = (rows[0]->on || rows[1]->on) ? 0 : 1;
    for (int i = 0; i < 2; i++) rows[i]->on = v && installed[i];
    log_line("F8: camera rows LOOP-005=%d LOOP-004=%d", rows[0]->on, rows[1]->on);
}

#ifdef FPS60_DEV
bool dev_key(int vk, bool* prev) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !*prev;
    *prev = down;
    if (!edge) return false;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Dev hotkey Num 4: tell which field system an unsmooth effect belongs to (U 12/10/2026, boat splash). Cycles
// FX-010 + TEXANIM-001 both on -> FX-010 off (field particles step every frame: smooth but 2x) -> TEXANIM-001 off
// (texture animations every frame) -> both on. Both rows gate on their .on flag per call.
static void hotkey_fx_rows() {
    static bool prev = false;
    static int  installed[2] = { -1, -1 }, state = 0;
    Row* rows[2] = { &row_FX010, &row_TEXANIM001 };
    if (!dev_key(VK_NUMPAD4, &prev)) return;
    for (int i = 0; i < 2; i++) if (installed[i] < 0) installed[i] = rows[i]->on;
    state = (state + 1) % 3;
    rows[0]->on = installed[0] && state != 1;
    rows[1]->on = installed[1] && state != 2;
    log_line("Num4: field effect rows FX-010=%d TEXANIM-001=%d", rows[0]->on, rows[1]->on);
}

// Dev hotkey Num 6: bisect battle-side rows at runtime (U 12/10/2026: Echuilles battle light rays flicker every
// frame; not FX-010). Each press turns the next row in the list alone off (only rows that gate on .on per call and
// keep no per-cast state), then all back on.
static void hotkey_battle_rows() {
    static bool prev = false;
    static Row* rows[] = { &row_MAG006, &row_MAG025, &row_UI001, &row_MAG022, &row_MAG017, &row_MAG026 };
    enum { N = sizeof(rows) / sizeof(rows[0]) };
    static int installed[N], state = 0;
    static bool init = false;
    if (!dev_key(VK_NUMPAD6, &prev)) return;
    if (!init) { for (int i = 0; i < N; i++) installed[i] = rows[i]->on; init = true; }
    state = (state + 1) % (N + 1);
    for (int i = 0; i < N; i++) rows[i]->on = installed[i] && state != i + 1;
    if (state) log_line("Num6: only %s off (installed %d)", rows[state - 1]->id, installed[state - 1]);
    else log_line("Num6: battle rows all back on");
}
#endif

// A row hook that takes > 50 ms in one invocation is logged (spell-start pause, U 12/10/2026).
static void row_call(Row* r, void (*fn)(), const char* what) {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    fn();
    QueryPerformanceCounter(&b);
    double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
    if (ms > 50.0) log_line("rows: %s %s took %.1f ms", r->id, what, ms);
}

void rows_pre() {
    hotkey_camera_rows();
#ifdef FPS60_DEV
    hotkey_fx_rows();
    hotkey_battle_rows();
#endif
    for (Row** p = g_rows; *p; p++)
        if ((*p)->on && (*p)->pre) { where_set(W_ROW_PRE, (int)(p - g_rows)); row_call(*p, (*p)->pre, "pre"); }
}

void rows_post() {
    for (Row** p = g_rows; *p; p++)
        if ((*p)->on && (*p)->post) { where_set(W_ROW_POST, (int)(p - g_rows)); row_call(*p, (*p)->post, "post"); }
}

// Dev hot reload: each row undoes the state it holds between invocations.
void rows_stop_all() {
    for (Row** p = g_rows; *p; p++)
        if ((*p)->on && (*p)->stop) {
            __try { (*p)->stop(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { log_line("rows: %s stop faulted", (*p)->id); }
        }
}

int rows_count() { return (int)(sizeof(g_rows) / sizeof(g_rows[0])) - 1; }

const Row* row_at(int i) { return (i >= 0 && i < rows_count()) ? g_rows[i] : nullptr; }
