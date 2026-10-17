// SPDX-License-Identifier: Apache-2.0
// fps60_ffx.dll entry: loaded by the shared loader (dbghelp.dll, src/loader) when the host is FFX.exe. Starts fps60
// once the Steam DRM has unpacked the game code (gives up after unpack_timeout_s, leaving the game untouched), and
// adds the crash line to the game's dump through the loader's mod_minidump_pre call.
// Details: docs/fps60/proxy.md
#include "fps60.h"
#include <stdio.h>

typedef BOOL(WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);

static MiniDumpWriteDump_t resolve_real_minidump() {
    static MiniDumpWriteDump_t real = nullptr;
    if (real) return real;

    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return nullptr;
    lstrcatW(path, L"\\dbghelp.dll");

    HMODULE m = LoadLibraryW(path);
    if (!m) return nullptr;
    real = (MiniDumpWriteDump_t)GetProcAddress(m, "MiniDumpWriteDump");
    return real;
}

// DIAG-HANGDUMP resolves it at install, so the watchdog never has to load dbghelp under a held loader lock.
void* proxy_real_minidump() { return (void*)resolve_real_minidump(); }

// Crash line in fps60.log before the game writes its dump (the dump itself can come out empty):
// exception, registers, return-address candidates on the stack, and row state (MAG-021 window).
static void describe(uint32_t a, char* out, size_t cap) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)a, &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* b = name;
        for (const char* q = name; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
        uint32_t ghidra = (m == (HMODULE)exe_base()) ? 0x400000u : 0x10000000u;
        _snprintf_s(out, cap, _TRUNCATE, "%s gva 0x%08X", b, ghidra + (a - (uint32_t)(uintptr_t)m));
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "0x%08X (no module)", a);
    }
}

static void log_crash(void* exception_param) {
    struct MDEI { DWORD ThreadId; EXCEPTION_POINTERS* ExceptionPointers; BOOL ClientPointers; };
    MDEI* ei = (MDEI*)exception_param;
    // ClientPointers: same process, still readable
    if (!ei || !ei->ExceptionPointers) { log_line("CRASH: game writes a dump (no exception info)"); return; }
    EXCEPTION_RECORD* er = ei->ExceptionPointers->ExceptionRecord;
    CONTEXT* c = ei->ExceptionPointers->ContextRecord;
    char where[160];
    describe((uint32_t)(uintptr_t)er->ExceptionAddress, where, sizeof(where));
    log_line("CRASH: code 0x%08lX at %s (info %08X %08X), thread %lu, inv kind %d, %s", er->ExceptionCode, where,
             er->NumberParameters > 0 ? (uint32_t)er->ExceptionInformation[0] : 0,
             er->NumberParameters > 1 ? (uint32_t)er->ExceptionInformation[1] : 0, ei->ThreadId, pacing_kind(), mag021_state());
    log_line("CRASH: eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX",
             c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    uint32_t* sp = (uint32_t*)(uintptr_t)c->Esp;
    int shown = 0;
    for (int i = 0; i < 512 && shown < 24; i++) {
        uint32_t v = 0;
        if (!safe_read_u32(sp + i, &v)) break;
        MEMORY_BASIC_INFORMATION mb;
        if (v < 0x10000 || !VirtualQuery((void*)(uintptr_t)v, &mb, sizeof(mb)) || mb.Type != MEM_IMAGE ||
            !(mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;
        describe(v, where, sizeof(where));
        log_line("CRASH:   [esp+0x%03X] %s", i * 4, where);
        shown++;
    }
}

// Set in DllMain when the host is FFX.exe (the loader only loads this DLL there; kept as a guard).
static bool g_host_ffx = false;

// Called by the loader (SEH-guarded) before the game's crash dump is written.
extern "C" void __cdecl mod_minidump_pre(void* exception_param, int* dump_type) {
    if (!g_host_ffx) return;
    __try { log_crash(exception_param); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Diagnostic ([fps60] crash_dump_mem=1): the game's crash dump also gets heap and module data
    // (MiniDumpWithDataSegs 0x1 | WithPrivateReadWriteMemory 0x200 | WithFullMemoryInfo 0x800).
    if (g_cfg.crash_dump_mem && dump_type) *dump_type |= 0x1 | 0x200 | 0x800;
}

static bool probe_ready() {
    __try { return sigs_text_ready(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static DWORD WINAPI init_thread(LPVOID) {
    log_open();
    config_load();
    if (!hot_copy()) {   // fresh start (base DLL): no copy of a dev hot reload is loaded
        wchar_t dir[MAX_PATH], pat[MAX_PATH], f[MAX_PATH];
        game_dir(dir, MAX_PATH);
        _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%sfps60_live_*.dll", dir);
        WIN32_FIND_DATAW fd;
        HANDLE h = dir[0] ? FindFirstFileW(pat, &fd) : INVALID_HANDLE_VALUE;
        if (h != INVALID_HANDLE_VALUE) {
            do { _snwprintf_s(f, MAX_PATH, _TRUNCATE, L"%s%s", dir, fd.cFileName); DeleteFileW(f); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    } else {
        log_line("fps60: hot reload - this copy installs in place of the previous one");
    }
    rate_init();
    log_line("fps60 init: exe base=%p size=0x%X, enabled=%d pacing=%d fps=%d rollback=%d", exe_base(), exe_size(),
             g_cfg.enabled, g_cfg.pacing, g_cfg.fps, g_cfg.rollback);
    if (!g_cfg.enabled) {
        log_line("fps60 disabled in fps60.ini - nothing installed.");
        return 0;
    }

    const int budget_ms = g_cfg.unpack_timeout_s * 1000;
    int waited = 0;
    while (!probe_ready()) {
        if (waited >= budget_ms) {
            log_line("fps60 gave up waiting for the unpacked .text (%d s) - nothing installed.", g_cfg.unpack_timeout_s);
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
        log_line("fps60: a required signature failed verification - NOTHING INSTALLED (fail-safe).");
        return 0;
    }
    if (g_cfg.pacing && !g_set_divider_orig) {
        log_line("fps60: SKS_SetFrameDivider not resolved - 60 Hz pacing impossible, NOTHING INSTALLED (fail-safe).");
        return 0;
    }

    hook_init();
    __try { rows_install_all(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { log_line("FAULT during row install - NOTHING INSTALLED"); return 0; }
    rollback_report();
    __try { capture_init(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { capture_on_fault("capture init", GetExceptionCode()); }

    if (!hooks_install_mainloop()) {
        log_line("fps60: main loop hook FAILED - nothing installed.");
        return 0;
    }
    log_line("fps60 ready (pacing %s).", g_cfg.pacing ? "60 Hz: divider 1 + exec alternation" : "stock");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);

        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return TRUE;
        const wchar_t* name = exe;
        for (const wchar_t* p = exe; *p; p++) if (*p == L'\\' || *p == L'/') name = p + 1;
        if (lstrcmpiW(name, L"FFX.exe") != 0) return TRUE;   // not FFX: stay inert
        g_host_ffx = true;

        HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
