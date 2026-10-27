// SPDX-License-Identifier: Apache-2.0
// jobs_ffx.dll entry: the shared loader loads this DLL when the host is FFX.exe. The init thread waits for the
// Steam DRM to unpack .text, resolves every signature (fail-safe: one required miss = nothing installed), then
// starts the Lua host.
#include "jobs.h"

namespace jobs {

static bool probe_ready() {
    __try { return sigs_text_ready(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static DWORD WINAPI init_thread(LPVOID) {
    config_load();
    log_open();
    if (!g_cfg.enabled) { log_line("jobs disabled in jobs.ini - nothing installed."); return 0; }
    log_line("jobs init: FFX.exe base=%p", exe_base());

    const int budget_ms = g_cfg.unpack_timeout_s * 1000;
    int waited = 0;
    while (!probe_ready()) {
        if (waited >= budget_ms) {
            log_line("jobs gave up waiting for the unpacked .text (%d s) - nothing installed.", g_cfg.unpack_timeout_s);
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
        log_line("jobs: a required signature failed verification - NOTHING INSTALLED (fail-safe).");
        return 0;
    }
    lua_start();
    log_line("jobs ready.");
    return 0;
}

} // namespace jobs

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(hinst);
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return TRUE;
    const wchar_t* name = exe;
    for (const wchar_t* p = exe; *p; p++) if (*p == L'\\' || *p == L'/') name = p + 1;
    if (lstrcmpiW(name, L"FFX.exe") != 0) return TRUE;   // not FFX: stay inert
    HANDLE t = CreateThread(nullptr, 0, jobs::init_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return TRUE;
}
