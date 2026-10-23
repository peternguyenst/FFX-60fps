// SPDX-License-Identifier: Apache-2.0
// fps60_ffx2.dll entry: the shared loader loads this DLL when the host is FFX-2.exe. The init thread waits for the
// Steam DRM to unpack .text, resolves every signature (fail-safe: one required miss = nothing installed), then
// hooks the runner's CALL Sg_MainLoop. The divider hook installs on the game thread at the first invocation.
#include "x2.h"
#include <stdio.h>

namespace x2 {

static bool probe_ready() {
    __try { return sigs_text_ready(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static DWORD WINAPI init_thread(LPVOID) {
    config_load();
    log_open();
    if (!hot_copy()) {   // fresh start: copies left by dev hot reloads of an earlier session are deleted
        wchar_t dir[MAX_PATH], pat[MAX_PATH], f[MAX_PATH];
        game_dir(dir, MAX_PATH);
        _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%sfps60x2_live_*.dll", dir);
        WIN32_FIND_DATAW fd;
        HANDLE h = dir[0] ? FindFirstFileW(pat, &fd) : INVALID_HANDLE_VALUE;
        if (h != INVALID_HANDLE_VALUE) {
            do { _snwprintf_s(f, MAX_PATH, _TRUNCATE, L"%s%s", dir, fd.cFileName); DeleteFileW(f); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    } else {
        log_line("x2: hot reload - this copy installs in place of the previous one");
    }
    if (!g_cfg.enabled) { log_line("x2 disabled in fps60x2.ini - nothing installed."); return 0; }
    log_line("x2 init: FFX-2.exe base=%p, pacing=%d exec_method=%d", exe_base(), g_cfg.pacing, g_cfg.exec_method);

    const int budget_ms = g_cfg.unpack_timeout_s * 1000;
    int waited = 0;
    while (!probe_ready()) {
        if (waited >= budget_ms) {
            log_line("x2 gave up waiting for the unpacked .text (%d s) - nothing installed.", g_cfg.unpack_timeout_s);
            return 0;
        }
        Sleep(250);
        waited += 250;
    }
    log_line("text section ready after ~%d ms", waited);

    bool ok = false;
    __try { ok = sigs_resolve_all(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; log_line("FAULT during signature resolution"); }
    if (!ok) {
        log_line("x2: a required signature failed verification - NOTHING INSTALLED (fail-safe).");
        return 0;
    }
    __try { rows_install_all(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { log_line("FAULT during row install - NOTHING INSTALLED"); return 0; }
    if (!hooks_install_mainloop()) {
        log_line("x2: main loop hook FAILED - nothing installed.");
        return 0;
    }
    log_line("x2 ready (pacing %s).", g_cfg.pacing ? "60 Hz: divider 1 + exec alternation" : "stock");
    return 0;
}

} // namespace x2

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(hinst);
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return TRUE;
    const wchar_t* name = exe;
    for (const wchar_t* p = exe; *p; p++) if (*p == L'\\' || *p == L'/') name = p + 1;
    if (lstrcmpiW(name, L"FFX-2.exe") != 0) return TRUE;   // not FFX-2: stay inert
    HANDLE t = CreateThread(nullptr, 0, x2::init_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return TRUE;
}
