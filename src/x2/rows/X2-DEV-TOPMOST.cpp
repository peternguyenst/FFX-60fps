// SPDX-License-Identifier: Apache-2.0
// X2-DEV-TOPMOST (test tool): a key keeps the game window on top of other windows, or lets it go again.
//
// Numpad 6 by default (X2-DEV-TOPMOST-KEY). Works in windowed and borderless modes (exclusive fullscreen has no "on
// top"). No game code involved. ini [rows]: X2-DEV-TOPMOST=1, X2-DEV-TOPMOST-KEY=<virtual-key code, decimal;
// 102 = numpad 6>, X2-DEV-TOPMOST-START=1 (on top from the start).
#include "../x2.h"

namespace x2 {

extern Row row_DEVTOPMOST;

static int g_key = 0x66;
static bool g_on = false, g_held = false, g_want_start = false;

static BOOL CALLBACK find_wnd(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    *(HWND*)lp = h;
    return FALSE;
}

static HWND game_window() {
    HWND h = nullptr;
    EnumWindows(find_wnd, (LPARAM)&h);
    return h;
}

static void set_on(bool on) {
    HWND h = game_window();
    if (!h) return;
    SetWindowPos(h, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_on = on;
    log_line("X2-DEV-TOPMOST: game window %s", on ? "always on top" : "normal");
}

static void pre() {
    if (g_want_start) { g_want_start = false; set_on(true); }
    bool down = (GetAsyncKeyState(g_key) & 0x8000) != 0;
    if (down) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        down = pid == GetCurrentProcessId();
    }
    if (down && !g_held) set_on(!g_on);
    g_held = down;
}

static bool install() {
    g_key = config_row("X2-DEV-TOPMOST-KEY", 0x66);
    g_want_start = config_row("X2-DEV-TOPMOST-START", 0) != 0;
    log_line("  X2-DEV-TOPMOST: key 0x%X toggles always-on-top%s", g_key, g_want_start ? " (on at start)" : "");
    return true;
}

Row row_DEVTOPMOST = {
    "X2-DEV-TOPMOST",
    "test tool: a key (X2-DEV-TOPMOST-KEY, default numpad 6) toggles the game window always-on-top",
    1, install, pre, nullptr, 0
};

} // namespace x2
