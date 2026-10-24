// SPDX-License-Identifier: Apache-2.0
// fps60_ffx.dll entry: loaded by the shared loader (dbghelp.dll, src/loader) when the host is FFX.exe. Starts fps60
// once the Steam DRM has unpacked the game code (gives up after unpack_timeout_s, leaving the game untouched), and
// adds the crash line to the game's dump through the loader's mod_minidump_pre call.
// Details: docs/fps60/proxy.md
#include "fps60.h"
#include <stdio.h>
#include <dbghelp.h>
#include <tlhelp32.h>

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
    // EBP frame chain: exact callers wherever the functions keep a frame (the stack scan above also lists stale values).
    uint32_t bp = c->Ebp;
    for (int i = 0; i < 16 && bp >= 0x10000; i++) {
        uint32_t next = 0, ret = 0;
        if (!safe_read_u32((void*)(uintptr_t)bp, &next) || !safe_read_u32((void*)(uintptr_t)(bp + 4), &ret)) break;
        describe(ret, where, sizeof(where));
        log_line("CRASH:   frame %2d ebp=%08X ret %s", i, bp, where);
        if (next <= bp) break;
        bp = next;
    }
    // 32 bytes at every register that points to readable memory (the record or table a faulting read went through).
    const struct { const char* n; DWORD v; } regs[] = {
        { "eax", c->Eax }, { "ebx", c->Ebx }, { "ecx", c->Ecx }, { "edx", c->Edx }, { "esi", c->Esi }, { "edi", c->Edi },
        { "ebp", c->Ebp } };
    for (auto& r : regs) {
        uint8_t b[32];
        if (r.v < 0x10000 || !safe_copy(b, (void*)(uintptr_t)r.v, sizeof(b))) continue;
        char hex[3 * 32 + 1];
        int k = 0;
        for (int i = 0; i < 32; i++) k += _snprintf_s(hex + k, sizeof(hex) - k, _TRUNCATE, "%02X%s", b[i], (i % 4 == 3) ? " " : "");
        describe(r.v, where, sizeof(where));
        log_line("CRASH:   [%s] %s: %s", r.n, where, hex);
    }
    // Magic DLLs loaded now (their bases turn runtime addresses into Ghidra VAs, base 0x10000000).
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = { sizeof(me) };
        for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
            if (!_wcsnicmp(me.szModule, L"magic_", 6) || !_wcsnicmp(me.szModule, L"fps60", 5))
                log_line("CRASH:   module %ls at %p size 0x%X", me.szModule, me.modBaseAddr, me.modBaseSize);
        CloseHandle(snap);
    }
    char w[200];
    where_text(g_where, w, sizeof(w));
    log_line("CRASH:   %d fps (%d passes per tick), game thread at %s", rate_fps(), rate_mult(), w);
}

// The mod's own dump, fps60_crash_<time>.dmp (the game's CoreDump.dmp is overwritten by the next crash): thread
// stacks, memory the registers and stacks point to, and the data sections of FFX.exe and the magic DLLs (game state
// and effect records). Other modules' data is left out to keep the file small.
static BOOL CALLBACK crash_dump_cb(PVOID, const PMINIDUMP_CALLBACK_INPUT in, PMINIDUMP_CALLBACK_OUTPUT out) {
    switch (in->CallbackType) {
    case ModuleCallback: {
        const wchar_t* path = in->Module.FullPath;
        const wchar_t* b = path;
        for (const wchar_t* q = path; q && *q; q++) if (*q == L'\\' || *q == L'/') b = q + 1;
        bool keep = (uintptr_t)in->Module.BaseOfImage == (uintptr_t)exe_base() || (b && !_wcsnicmp(b, L"magic_", 6));
        if (!keep) out->ModuleWriteFlags &= ~ModuleWriteDataSeg;
        return TRUE;
    }
    case IncludeModuleCallback: case IncludeThreadCallback: case ThreadCallback: case ThreadExCallback:
        return TRUE;
    default:
        return FALSE;
    }
}

// Keep the newest `keep` crash dumps (names sort by time). Same as FFX-2's X2-DIAG-CRASH.
static void prune_dumps(const wchar_t* dir, int keep) {
    wchar_t pat[MAX_PATH];
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%sfps60_crash_*.dmp", dir);
    wchar_t names[64][48];
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (n < 64 && wcslen(fd.cFileName) < 48) wcscpy_s(names[n++], 48, fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    for (int i = 1; i < n; i++)                    // insertion sort, oldest first
        for (int j = i; j > 0 && wcscmp(names[j - 1], names[j]) > 0; j--) {
            wchar_t t[48];
            wcscpy_s(t, 48, names[j]); wcscpy_s(names[j], 48, names[j - 1]); wcscpy_s(names[j - 1], 48, t);
        }
    for (int i = 0; i + keep < n; i++) {
        wchar_t q[MAX_PATH];
        _snwprintf_s(q, MAX_PATH, _TRUNCATE, L"%s%s", dir, names[i]);
        DeleteFileW(q);
    }
}

struct DumpJob { void* exception_param; HANDLE file; BOOL ok; DWORD err; };

// On a helper thread: the crashed thread may be short of stack.
static DWORD WINAPI dump_thread(LPVOID p) {
    typedef BOOL(WINAPI* md_t)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    DumpJob* j = (DumpJob*)p;
    md_t md = (md_t)resolve_real_minidump();
    if (!md) { j->err = ERROR_PROC_NOT_FOUND; return 0; }
    int type = MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpScanMemory | MiniDumpWithUnloadedModules |
               MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo |
               MiniDumpWithThreadInfo;
    MINIDUMP_CALLBACK_INFORMATION cb = { crash_dump_cb, nullptr };
    j->ok = md(GetCurrentProcess(), GetCurrentProcessId(), j->file, type, j->exception_param, nullptr, &cb);
    j->err = j->ok ? 0 : GetLastError();
    return 0;
}

static void write_crash_dump(void* exception_param) {
    if (!resolve_real_minidump()) return;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    game_dir(dir, MAX_PATH);
    if (!dir[0]) return;
    prune_dumps(dir, 4);                           // with this one: the newest 5
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%sfps60_crash_%04u%02u%02u-%02u%02u%02u.dmp", dir, t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { log_line("CRASH: own dump not written (create %lu)", GetLastError()); return; }
    DumpJob j = { exception_param, f, FALSE, WAIT_TIMEOUT };
    HANDLE th = CreateThread(nullptr, 0, dump_thread, &j, 0, nullptr);
    if (th) { WaitForSingleObject(th, 60000); CloseHandle(th); }
    else dump_thread(&j);
    BOOL ok = j.ok;
    DWORD err = j.err;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(f, &sz);
    CloseHandle(f);
    log_line("CRASH: own dump %ls %s (%lld KB, err %lu)", path + lstrlenW(dir), ok ? "written" : "FAILED",
             sz.QuadPart / 1024, err);
}

// Set in DllMain when the host is FFX.exe (the loader only loads this DLL there; kept as a guard).
static bool g_host_ffx = false;

// Called by the loader (SEH-guarded) before the game's crash dump is written.
extern "C" void __cdecl mod_minidump_pre(void* exception_param, int* dump_type) {
    if (!g_host_ffx) return;
    // After a dev hot reload this copy is passive and its log is closed: the copy it loaded reports.
    if (HMODULE next = reload_next_module()) {
        typedef void(__cdecl* pre_t)(void*, int*);
        if (pre_t f = (pre_t)GetProcAddress(next, "mod_minidump_pre")) { f(exception_param, dump_type); return; }
    }
    __try { log_crash(exception_param); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { write_crash_dump(exception_param); }
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
