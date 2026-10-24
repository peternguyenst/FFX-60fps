// SPDX-License-Identifier: Apache-2.0
// Dev hot reload (FPS60_DEV builds only): Num 9 in game loads fps60_next.dll (a fresh dev build copied next to
// FFX.exe) in place of this copy, without restarting the game. This copy goes passive and stays mapped, so a thread
// still inside one of its detours finishes normally:
//   1. pacing back to stock (the game's own divider, exec 1)
//   2. g_disabled = 1: every detour of this copy passes through; its watchdog idles; no new magic-DLL hooks
//   3. Row::stop: rows undo state they hold between invocations (SMOOTH-003 held poses, half-rate switch)
//   4. all MinHook entry hooks disabled (original prologues back), every journaled byte patch reverted
//   5. log closed; the new copy (loaded from a unique copy, so fps60_next.dll stays writable) opens it in append
//      mode (hot_copy(): its file is fps60_live_*.dll) and installs from scratch on its own init thread, as at game start.
// Runs between invocations on the game thread (end of fp_post). Magic DLLs loaded at that moment keep running
// unhooked until they unload; reload outside spell animations.
#include "fps60.h"
#include <stdio.h>

#ifdef FPS60_DEV
void game_dir(wchar_t* out, size_t cap);
void log_reopen();

static bool g_prev = false;
static HMODULE g_next_mod = nullptr;             // the copy this one loaded (crash reports are forwarded to it)
HMODULE reload_next_module() { return g_next_mod; }

void reload_poll() {
    if (g_disabled || !dev_key(VK_NUMPAD9, &g_prev)) return;
    wchar_t dir[MAX_PATH], next[MAX_PATH], live[MAX_PATH];
    game_dir(dir, MAX_PATH);
    if (!dir[0]) return;
    _snwprintf_s(next, MAX_PATH, _TRUNCATE, L"%sfps60_next.dll", dir);
    if (GetFileAttributesW(next) == INVALID_FILE_ATTRIBUTES) {
        log_line("Num9: hot reload: no fps60_next.dll next to FFX.exe - nothing to load");
        return;
    }
    _snwprintf_s(live, MAX_PATH, _TRUNCATE, L"%sfps60_live_%lu.dll", dir, GetTickCount());
    if (!CopyFileW(next, live, FALSE)) {
        log_line("Num9: hot reload: copy to %ls failed (%lu)", live, GetLastError());
        return;
    }
    log_line("Num9: hot reload: switching this copy off");
    pacing_shutdown();
    InterlockedExchange(&g_disabled, 1);
    rows_stop_all();
    hook_disable_all();
    patch_revert_all();
    log_line("Num9: hot reload: this copy is passive; loading %ls", live);
    log_close();
    g_next_mod = LoadLibraryW(live);
    if (!g_next_mod) {
        DWORD err = GetLastError();
        log_reopen();
        log_line("Num9: hot reload: LoadLibrary failed (%lu) - the game runs stock until restart", err);
    }
}
#else
HMODULE reload_next_module() { return nullptr; }
#endif
