// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-CRASH (diagnostic, default 1 since 22/10/2026: the mod is in progress, user): U 18/10/2026: the game closed itself while a commsphere recording was viewed
// (log ends 0.0 s after magic_0760.dll loaded; no mod fault line, no Windows crash record or dump). Logs:
// - every first-chance fault of the kinds below (vectored handler, first in the chain): code, faulting module + offset
//   (FFX-2.exe also as Ghidra VA), the accessed address, registers, and up to 12 stack words that point into code
//   modules (return-address candidates). Faults inside the mod's own __try blocks are logged too (they are handled
//   afterwards). Capped at 40 lines.
// - ExitProcess / TerminateProcess(own process): the exit code and the caller chain, so a silent exit through the
//   game's own error path shows who asked for it.
// - every 60 s (checked after each invocation): private bytes, working set, free address space (total, largest block) and the
//   handle count. U 22/10/2026 (friend): crashes after ~30 min battles / back-to-back cups, log ends with no fault
//   line; this shows whether anything grows toward exhaustion before such a crash.
// - X2-DIAG-CRASH-DUMP (default 1 since 24/10/2026: the mod is in dev, user): on a fatal crash, the loaded modules
//   (base, size) and a minidump fps60x2_crash_<date>-<time>.dmp next to FFX-2.exe (newest 5 kept). The game's own
//   filter (FUN_00ab6200, set in FUN_00abe730: S) writes CoreDump.dmp only while DAT_00db25b8 is set, without heap or
//   globals (type 0x1824), and every crash (FFX's too, same folder) overwrites it. The mod's filter stays on top
//   (SetUnhandledExceptionFilter hooked, the game's filter kept and called after the dump). 1 = thread stacks,
//   referenced heap, .data/.bss of every module (FFX-2.exe .data is 17 MB), handles, memory map; 2 = full memory
//   (hundreds of MB); 0 = off. Heap corruption (0xC0000374) skips the filters, so the vectored handler writes it.
//   The dump is written by a helper thread (the faulting thread may be out of stack).
// Observe only: the handler always returns EXCEPTION_CONTINUE_SEARCH and the exit hooks call the originals.
#include "../x2.h"
#include <stdio.h>
#include <psapi.h>                 // K32* (kernel32, PSAPI_VERSION 2)
#include <dbghelp.h>               // types only; MiniDumpWriteDump comes from System32 dbghelp (ours is the loader)

namespace x2 {

extern Row row_DIAGCRASH;

static PVOID g_veh = nullptr;
static volatile LONG g_logged = 0;
typedef VOID (WINAPI* exitp_t)(UINT);
typedef BOOL (WINAPI* termp_t)(HANDLE, UINT);
static exitp_t g_exit_tramp = nullptr;
static termp_t g_term_tramp = nullptr;
static void* g_exit_fn = nullptr;
static void* g_term_fn = nullptr;

typedef BOOL (WINAPI* minidump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
typedef LPTOP_LEVEL_EXCEPTION_FILTER (WINAPI* setuef_t)(LPTOP_LEVEL_EXCEPTION_FILTER);
static int g_dump = 1;                               // X2-DIAG-CRASH-DUMP
static minidump_t g_minidump = nullptr;
static setuef_t g_setuef_tramp = nullptr;
static void* g_setuef_fn = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER g_game_uef = nullptr;   // the filter the game (or anyone else) asked for
static volatile LONG g_dumped = 0;

// "FFX-2.exe+0x123 (gva 0x00523)" / "magic_0760.dll+0x1A2B" / "?"
static void where(const void* p, char* out, size_t cap) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)p, &m) || !m) {
        _snprintf_s(out, cap, _TRUNCATE, "%p", p);
        return;
    }
    wchar_t w[MAX_PATH];
    DWORD n = GetModuleFileNameW(m, w, MAX_PATH);
    const wchar_t* b = w;
    for (DWORD i = 0; i < n; i++) if (w[i] == L'\\' || w[i] == L'/') b = w + i + 1;
    uint32_t off = (uint32_t)((const uint8_t*)p - (const uint8_t*)m);
    if (m == (HMODULE)exe_base()) _snprintf_s(out, cap, _TRUNCATE, "%S+0x%X (gva 0x%08X)", b, off, rt_to_gva(p));
    else _snprintf_s(out, cap, _TRUNCATE, "%S+0x%X", b, off);
}

static bool is_code(const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static void log_stack(const char* tag, const uint32_t* sp) {
    char line[1024];
    int k = _snprintf_s(line, sizeof(line), _TRUNCATE, "X2-DIAG-CRASH:   %s stack:", tag);
    int found = 0;
    __try {
        for (int i = 0; i < 256 && found < 12 && k > 0; i++) {
            const void* v = (const void*)(uintptr_t)sp[i];
            if (!is_code(v)) continue;
            char w[160];
            where(v, w, sizeof(w));
            int r = _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " [sp+%X] %s;", i * 4, w);
            if (r < 0) break;
            k += r;
            found++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    log_line("%s", line);
}

// Loaded modules outside the Windows folder (game, magic DLLs, mod, overlays), several per line.
static void log_modules() {
    HMODULE mods[512];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
    wchar_t win[MAX_PATH];
    UINT wn = GetWindowsDirectoryW(win, MAX_PATH);
    char line[1024];
    int k = 0, n = (int)(need / sizeof(HMODULE));
    if (n > 512) n = 512;
    for (int i = 0; i < n; i++) {
        wchar_t w[MAX_PATH];
        DWORD len = GetModuleFileNameW(mods[i], w, MAX_PATH);
        if (!len || (wn && _wcsnicmp(w, win, wn) == 0)) continue;
        const wchar_t* b = w;
        for (DWORD j = 0; j < len; j++) if (w[j] == L'\\' || w[j] == L'/') b = w + j + 1;
        MODULEINFO mi = {};
        K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
        if (k == 0) k = _snprintf_s(line, sizeof(line), _TRUNCATE, "X2-DIAG-CRASH:   modules:");
        int r = _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %S %p+%lX;", b, mi.lpBaseOfDll,
                            (unsigned long)mi.SizeOfImage);
        if (r < 0 || k + r > 900) { log_line("%s", line); k = 0; if (r < 0) i--; continue; }
        k += r;
    }
    if (k > 0) log_line("%s", line);
}

// Keep the newest `keep` crash dumps (names sort by time).
static void prune_dumps(const wchar_t* dir, int keep) {
    wchar_t pat[MAX_PATH];
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%sfps60x2_crash_*.dmp", dir);
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
        wchar_t p[MAX_PATH];
        _snwprintf_s(p, MAX_PATH, _TRUNCATE, L"%s%s", dir, names[i]);
        DeleteFileW(p);
    }
}

struct DumpJob { EXCEPTION_POINTERS* ep; DWORD tid; };

static DWORD WINAPI dump_thread(void* arg) {
    DumpJob* j = (DumpJob*)arg;
    log_modules();
    if (!g_minidump) { log_line("X2-DIAG-CRASH: no dump (System32 MiniDumpWriteDump not resolved)"); return 0; }
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    game_dir(dir, MAX_PATH);
    prune_dumps(dir, 4);
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%sfps60x2_crash_%04u%02u%02u-%02u%02u%02u.dmp", dir, t.wYear, t.wMonth,
                 t.wDay, t.wHour, t.wMinute, t.wSecond);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log_line("X2-DIAG-CRASH: dump file not created (error %lu)", GetLastError());
        return 0;
    }
    int type = MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpScanMemory | MiniDumpWithUnloadedModules |
               MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo |
               MiniDumpWithThreadInfo;
    if (g_dump >= 2) type |= MiniDumpWithFullMemory;
    MINIDUMP_EXCEPTION_INFORMATION mei = { j->tid, j->ep, FALSE };
    ULONGLONG t0 = GetTickCount64();
    BOOL ok = g_minidump(GetCurrentProcess(), GetCurrentProcessId(), h, (MINIDUMP_TYPE)type, j->ep ? &mei : nullptr,
                         nullptr, nullptr);
    DWORD err = ok ? 0 : GetLastError();
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    CloseHandle(h);
    if (ok) log_line("X2-DIAG-CRASH: dump %S (%lu KB, type 0x%X, %llu ms)", path, (unsigned long)(sz.QuadPart >> 10),
                     type, GetTickCount64() - t0);
    else log_line("X2-DIAG-CRASH: dump %S FAILED (error 0x%lX)", path, err);
    return 0;
}

// Once per process; the faulting thread waits for the helper thread (up to 60 s).
static void write_dump(EXCEPTION_POINTERS* ep, const char* why) {
    if (g_dump <= 0 || InterlockedExchange(&g_dumped, 1)) return;
    DWORD c = ep ? ep->ExceptionRecord->ExceptionCode : 0;
    char w[160] = "?";
    if (ep) where(ep->ExceptionRecord->ExceptionAddress, w, sizeof(w));
    log_line("X2-DIAG-CRASH: FATAL (%s) exception 0x%08lX at %s, thread %lu - writing dump", why, c, w,
             GetCurrentThreadId());
    DumpJob j = { ep, GetCurrentThreadId() };
    HANDLE th = CreateThread(nullptr, 1 << 20, dump_thread, &j, 0, nullptr);
    if (th) { WaitForSingleObject(th, 60000); CloseHandle(th); }
    else dump_thread(&j);
}

static LONG WINAPI uef(EXCEPTION_POINTERS* ep) {
    write_dump(ep, "unhandled");
    LPTOP_LEVEL_EXCEPTION_FILTER next = g_game_uef;
    return next ? next(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// Anyone (the game's WinMain, overlays) setting a filter is chained behind the mod's.
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI h_setuef(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    if (f == uef) return g_setuef_tramp(f);
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_game_uef;
    g_game_uef = f;
    char w[160] = "null";
    if (f) where((const void*)f, w, sizeof(w));
    log_line("X2-DIAG-CRASH: SetUnhandledExceptionFilter(%s) chained behind the dump filter", w);
    return prev;
}

static LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_ILLEGAL_INSTRUCTION && c != EXCEPTION_PRIV_INSTRUCTION &&
        c != EXCEPTION_INT_DIVIDE_BY_ZERO && c != EXCEPTION_STACK_OVERFLOW && c != EXCEPTION_ARRAY_BOUNDS_EXCEEDED &&
        c != 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN (fast fail) */ && c != 0xC0000374 /* heap corruption */)
        return EXCEPTION_CONTINUE_SEARCH;
    if (c == 0xC0000374) write_dump(ep, "heap corruption");   // never reaches the filters
    if (InterlockedIncrement(&g_logged) > 40) return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* x = ep->ContextRecord;
    char w[160];
    where(ep->ExceptionRecord->ExceptionAddress, w, sizeof(w));
    ULONG_PTR rw = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[0] : 0;
    ULONG_PTR addr = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    log_line("X2-DIAG-CRASH: exception 0x%08lX at %s, %s 0x%08lX (thread %lu, inv kind %d)", c, w,
             rw == 0 ? "read" : rw == 1 ? "write" : "exec", (unsigned long)addr, GetCurrentThreadId(), pacing_kind());
    log_line("X2-DIAG-CRASH:   EAX %08lX EBX %08lX ECX %08lX EDX %08lX ESI %08lX EDI %08lX EBP %08lX ESP %08lX",
             x->Eax, x->Ebx, x->Ecx, x->Edx, x->Esi, x->Edi, x->Ebp, x->Esp);
    log_stack("fault", (const uint32_t*)(uintptr_t)x->Esp);
    return EXCEPTION_CONTINUE_SEARCH;
}

static VOID WINAPI h_exit(UINT code) {
    uint32_t sp_probe = 0;
    log_line("X2-DIAG-CRASH: ExitProcess(%u) on thread %lu", code, GetCurrentThreadId());
    log_stack("exit", &sp_probe);
    g_exit_tramp(code);
}

static BOOL WINAPI h_term(HANDLE h, UINT code) {
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) {
        uint32_t sp_probe = 0;
        log_line("X2-DIAG-CRASH: TerminateProcess(self, %u) on thread %lu", code, GetCurrentThreadId());
        log_stack("terminate", &sp_probe);
    }
    return g_term_tramp(h, code);
}

static void log_mem() {
    PROCESS_MEMORY_COUNTERS_EX pm = {};
    pm.cb = sizeof(pm);
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
    uint64_t free_total = 0, free_big = 0;
    uint8_t* p = nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_FREE) {
            free_total += mbi.RegionSize;
            if (mbi.RegionSize > free_big) free_big = mbi.RegionSize;
        }
        uint8_t* n = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        if (n <= p) break;
        p = n;
    }
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    log_line("X2-DIAG-CRASH: memory private %lu MB, working set %lu MB, free VA %lu MB (largest %lu MB), handles %lu",
             (unsigned long)(pm.PrivateUsage >> 20), (unsigned long)(pm.WorkingSetSize >> 20),
             (unsigned long)(free_total >> 20), (unsigned long)(free_big >> 20), handles);
}

static void post() {
    static ULONGLONG next = 0;
    ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 60000;
    log_mem();
}

static bool install() {
    g_veh = AddVectoredExceptionHandler(1, veh);
    if (!g_veh) { log_line("  X2-DIAG-CRASH: AddVectoredExceptionHandler failed"); return false; }
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    g_exit_fn = k ? (void*)GetProcAddress(k, "ExitProcess") : nullptr;
    g_term_fn = k ? (void*)GetProcAddress(k, "TerminateProcess") : nullptr;
    bool e = g_exit_fn && hook_entry("X2-DIAG-CRASH ExitProcess", g_exit_fn, (void*)&h_exit, (void**)&g_exit_tramp);
    bool t = g_term_fn && hook_entry("X2-DIAG-CRASH TerminateProcess", g_term_fn, (void*)&h_term, (void**)&g_term_tramp);
    log_line("  X2-DIAG-CRASH: vectored handler on; exit hooks: ExitProcess %s, TerminateProcess %s", e ? "ok" : "FAILED",
             t ? "ok" : "FAILED");

    g_dump = config_row("X2-DIAG-CRASH-DUMP", 1);
    if (g_dump > 0) {
        wchar_t sys[MAX_PATH];
        UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n && n < MAX_PATH - 16) {
            wcscat_s(sys, MAX_PATH, L"\\dbghelp.dll");    // full path: the System32 copy, not the loader
            HMODULE dh = LoadLibraryW(sys);
            g_minidump = dh ? (minidump_t)GetProcAddress(dh, "MiniDumpWriteDump") : nullptr;
        }
        g_setuef_fn = k ? (void*)GetProcAddress(k, "SetUnhandledExceptionFilter") : nullptr;
        bool u = g_setuef_fn &&
                 hook_entry("X2-DIAG-CRASH SetUnhandledExceptionFilter", g_setuef_fn, (void*)&h_setuef, (void**)&g_setuef_tramp);
        LPTOP_LEVEL_EXCEPTION_FILTER prev = u ? g_setuef_tramp(uef) : SetUnhandledExceptionFilter(uef);
        if (!g_game_uef) g_game_uef = prev;
        char w[160] = "null";
        if (prev) where((const void*)prev, w, sizeof(w));
        log_line("  X2-DIAG-CRASH-DUMP=%d: crash dumps fps60x2_crash_*.dmp (%s), MiniDumpWriteDump %s, filter hook %s, "
                 "previous filter %s", g_dump, g_dump >= 2 ? "full memory" : "stacks + heap refs + data segments",
                 g_minidump ? "ok" : "MISSING", u ? "ok" : "FAILED (later filters replace the mod's)", w);
    } else {
        log_line("  X2-DIAG-CRASH-DUMP=0: no crash dump from the mod (the game's CoreDump.dmp only)");
    }
    return true;
}

Row row_DIAGCRASH = {
    "X2-DIAG-CRASH",
    "diagnostic: log faults (module+offset, registers, stack), who calls ExitProcess / TerminateProcess, and a "
    "minidump on a fatal crash (X2-DIAG-CRASH-DUMP)",
    1, install, nullptr, post, 0
};

} // namespace x2
