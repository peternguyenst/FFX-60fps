// SPDX-License-Identifier: Apache-2.0
// Dev hot reload (FPS60_DEV builds only; port of the FFX mod's reload.cpp): Num 9 (X2-DEV-RELOAD-KEY, as in the FFX
// mod; X2-MENU-002's Num 9 toggle is compiled out of dev builds) loads fps60x2_next.dll (a fresh dev build copied next
// to FFX-2.exe) in place of this copy, without restarting the game. This copy goes passive and stays mapped, so a
// thread still inside one of its detours finishes normally:
//   1. pacing back to stock (the game's own divider, exec 1)
//   2. g_disabled = 1: every detour of this copy passes through; no new magic-DLL hooks (x2_magic_mods.cpp)
//   3. Row::stop: rows undo state they hold between invocations or outside their hooks (held poses, the Config page
//      array, the crash handlers)
//   4. all MinHook entry hooks disabled (original prologues back), every patch_u32 write reverted (the runner's CALL
//      Sg_MainLoop included), so the new copy's signatures see stock bytes
//   5. log closed; the new copy (loaded from a unique copy fps60x2_live_<tick>.dll, so fps60x2_next.dll stays writable)
//      opens it in append mode (hot_copy()) and installs from scratch on its own init thread, as at game start; the
//      game runs stock until it is ready. A fresh game start deletes the live copies.
// Runs between invocations on the game thread (end of x2_post). Magic DLLs loaded at that moment keep running unhooked
// until they unload; reload outside spell animations.
#include "x2.h"
#include <stdio.h>

#ifdef FPS60_DEV
namespace x2 {

static bool g_key_was_down = false;
static int  g_reload_key = -1;

static bool key_edge() {
    bool first = g_reload_key < 0;
    if (first) g_reload_key = config_row("X2-DEV-RELOAD-KEY", VK_NUMPAD9);
    if (g_reload_key <= 0) return false;
    bool down = (GetAsyncKeyState(g_reload_key) & 0x8000) != 0;
    if (first) g_key_was_down = down;                    // a key still held from the reload that loaded this copy is no press
    bool edge = down && !g_key_was_down;
    g_key_was_down = down;
    if (!edge) return false;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void reload_poll() {
    if (g_disabled || !key_edge()) return;
    wchar_t dir[MAX_PATH], next[MAX_PATH], live[MAX_PATH];
    game_dir(dir, MAX_PATH);
    if (!dir[0]) return;
    _snwprintf_s(next, MAX_PATH, _TRUNCATE, L"%sfps60x2_next.dll", dir);
    if (GetFileAttributesW(next) == INVALID_FILE_ATTRIBUTES) {
        log_line("hot reload: no fps60x2_next.dll next to FFX-2.exe - nothing to load");
        return;
    }
    _snwprintf_s(live, MAX_PATH, _TRUNCATE, L"%sfps60x2_live_%lu.dll", dir, GetTickCount());
    if (!CopyFileW(next, live, FALSE)) {
        log_line("hot reload: copy to %ls failed (%lu)", live, GetLastError());
        return;
    }
    log_line("hot reload: switching this copy off");
    pacing_leave_now("hot reload");
    InterlockedExchange(&g_disabled, 1);
    rows_stop_all();
    hook_disable_all();
    patch_revert_all();
    log_line("hot reload: this copy is passive; loading %ls", live);
    log_close();
    if (!LoadLibraryW(live)) {
        DWORD err = GetLastError();
        log_reopen();
        log_line("hot reload: LoadLibrary failed (%lu) - the game runs stock until restart", err);
    }
}

} // namespace x2
#endif
