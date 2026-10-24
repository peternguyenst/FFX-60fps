// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-CRASH (diagnostic, on while the mod is in development): explains crashes and silent exits.
//
// It logs:
//   - every first-chance fault of the serious kinds: code, module + offset (FFX-2.exe also as Ghidra address), the
//     accessed address, registers and the stack words that look like return addresses (at most 40 lines)
//   - ExitProcess / TerminateProcess of the game itself, with the exit code and who called it (a silent exit through
//     the game's own error path)
//   - every 60 s: memory use, free address space and handle count (does anything grow toward a crash?)
// X2-DIAG-CRASH-DUMP (1): on a fatal crash, the loaded modules and a minidump fps60x2_crash_<date>-<time>.dmp next to
//   FFX-2.exe (newest 5 kept). The game's own CoreDump.dmp is thin and overwritten by every crash. 1 = stacks,
//   referenced heap, every module's data; 2 = full memory (hundreds of MB); 0 = off. Our crash filter stays on top
//   (the game's filter is kept and called after the dump); heap corruption skips the filters, so the fault handler
//   writes that dump. A helper thread writes it (the crashed thread may be out of stack). Read one with
//   tools/crash_report.py (ffx2-fps60 repo).
// X2-DIAG-CRASH-SPELL (1): every effect start logs its spell DLL and the battle command's name, and the crash report
//   (and the dump) list the last ones. The game's single call of the effect start (gva 0x631FAA) is retargeted; the
//   start itself runs unchanged.
// Observe only: the fault handler always lets the fault go on, and the exit hooks call the originals.
// Evidence: re/ledger/DIAG-CRASH.md.
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

typedef void (__cdecl* effstart_t)(uint32_t, uint8_t*, uint8_t*, uint8_t*, int);
typedef uint8_t* (__cdecl* cmdget_t)(uint32_t, uint8_t**);
static effstart_t g_effstart = nullptr;            // FUN_00616ba0
static cmdget_t g_cmdget = nullptr;                // FUN_00625130
struct Spell { uint8_t* base; char dll[20]; uint32_t cmd; char name[40]; ULONGLONG tick; };
static Spell g_sp[8];                              // last effect start per DLL base (game thread writes)
static char g_comment[1024];                       // dump comment stream (filled by the FATAL report)

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

// Game text -> ASCII for the log: single-byte glyphs 0x30 + index through the start of the us table
// (ffx2sjistbl_us.bin: digits, space, ASCII punctuation, A-Z, a-z), two-byte glyphs and the rest as '?', control
// bytes (< 0x2B) skipped, 0 ends.
static void decode_name(const uint8_t* p, char* out, size_t cap) {
    static const char G[] = "0123456789 !\"#$%&'()*+,-./:;<=>?ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
    size_t n = 0;
    for (int k = 0; k < 64 && n + 1 < cap && p[k]; ) {
        uint8_t b = p[k];
        if (b >= 0x30) { out[n++] = (size_t)(b - 0x30) < sizeof(G) - 1 ? G[b - 0x30] : '?'; k++; }
        else if (b >= 0x2B) { out[n++] = '?'; k += 2; }
        else k++;
    }
    out[n] = 0;
}

static void dll_name(const void* p, char* out, size_t cap) {
    HMODULE m = nullptr;
    wchar_t w[MAX_PATH];
    out[0] = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)p, &m) || !m) return;
    DWORD n = GetModuleFileNameW(m, w, MAX_PATH);
    const wchar_t* b = w;
    for (DWORD i = 0; i < n; i++) if (w[i] == L'\\' || w[i] == L'/') b = w + i + 1;
    _snprintf_s(out, cap, _TRUNCATE, "%S", b);
}

static void __cdecl h_effstart(uint32_t a, uint8_t* scene, uint8_t* entry, uint8_t* action, int kind) {
    uint32_t cmd = 0xFFFF;
    char nm[40] = "";
    __try {
        cmd = *(uint16_t*)(action + 0xA4);
        uint8_t* name = nullptr;
        if (kind != 0xF && g_cmdget) {
            g_cmdget(cmd, &name);
            if (name) decode_name(name, nm, sizeof(nm));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_effstart(a, scene, entry, action, kind);
    uint32_t tbl = 0;
    safe_read_u32(entry + 0x18, &tbl);
    char dll[20];
    dll_name((const void*)(uintptr_t)tbl, dll, sizeof(dll));
    HMODULE base = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(uintptr_t)tbl, &base);
    Spell* s = &g_sp[0];
    for (Spell& e : g_sp) {
        if (e.base == (uint8_t*)base) { s = &e; break; }
        if (e.tick < s->tick) s = &e;
    }
    s->base = (uint8_t*)base;
    strncpy_s(s->dll, dll, _TRUNCATE);
    s->cmd = cmd;
    strncpy_s(s->name, nm, _TRUNCATE);
    s->tick = GetTickCount64();
    log_line("X2-DIAG-CRASH: effect %s: command 0x%04X \"%s\"%s", dll[0] ? dll : "?", cmd, nm,
             kind == 0xF ? " (kind 0xF, no command)" : "");
}

// FATAL report: the last effect start of every magic DLL still loaded, newest first (also the dump comment).
static void log_spells() {
    ULONGLONG now = GetTickCount64();
    int k = _snprintf_s(g_comment, sizeof(g_comment), _TRUNCATE, "fps60x2 crash; effects loaded:");
    bool any = false;
    bool used[8] = {};
    for (;;) {
        int best = -1;
        for (int i = 0; i < 8; i++)
            if (!used[i] && g_sp[i].base && (best < 0 || g_sp[i].tick > g_sp[best].tick)) best = i;
        if (best < 0) break;
        used[best] = true;
        const Spell& s = g_sp[best];
        char now_dll[20];
        dll_name(s.base, now_dll, sizeof(now_dll));
        if (_stricmp(now_dll, s.dll) != 0) continue;              // unloaded (or another DLL at that base)
        log_line("X2-DIAG-CRASH:   spell: %s = command 0x%04X \"%s\" (started %.1f s ago)", s.dll, s.cmd, s.name,
                 (now - s.tick) / 1000.0);
        int r = _snprintf_s(g_comment + k, sizeof(g_comment) - k, _TRUNCATE, " %s 0x%04X \"%s\";", s.dll, s.cmd, s.name);
        if (r > 0) k += r;
        any = true;
    }
    if (!any) log_line("X2-DIAG-CRASH:   spell: no effect DLL loaded");
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
    if (g_effstart) log_spells();
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
    MINIDUMP_USER_STREAM us = { CommentStreamA, (ULONG)strlen(g_comment) + 1, g_comment };
    MINIDUMP_USER_STREAM_INFORMATION usi = { 1, &us };
    ULONGLONG t0 = GetTickCount64();
    BOOL ok = g_minidump(GetCurrentProcess(), GetCurrentProcessId(), h, (MINIDUMP_TYPE)type, j->ep ? &mei : nullptr,
                         g_comment[0] ? &usi : nullptr, nullptr);
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
    if (InterlockedExchange(&g_dumped, 1)) return;
    DWORD c = ep ? ep->ExceptionRecord->ExceptionCode : 0;
    char w[160] = "?";
    if (ep) where(ep->ExceptionRecord->ExceptionAddress, w, sizeof(w));
    log_line("X2-DIAG-CRASH: FATAL (%s) exception 0x%08lX at %s, thread %lu%s", why, c, w, GetCurrentThreadId(),
             g_dump > 0 ? " - writing dump" : "");
    if (g_dump <= 0) {
        if (g_effstart) log_spells();
        return;
    }
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

    if (config_row("X2-DIAG-CRASH-SPELL", 1)) {
        uint8_t* site = sig_find("X2-DIAG-CRASH effect start call 0x631FAA",
            "FF 75 F4 56 53 FF 75 FC 57 E8 ?? ?? ?? ?? 83 C4 1C BB 05 00 00 00", 0x631FA1);
        uint8_t* start = site ? call_target(site + 9) : nullptr;
        // FUN_00616ba0: PUSH EBP; MOV EBP,ESP; PUSH ECX; MOV AL,[..]; PUSH EBX; MOV EBX,[EBP+8]; PUSH ESI; MOV ESI,[EBP+0x10]
        if (start && !sig_match(start, "55 8B EC 51 A0 ?? ?? ?? ?? 53 8B 5D 08 56 8B 75 10")) start = nullptr;
        uint8_t* get = sig_find("X2-DIAG-CRASH command getter call 0x616C47",
            "6A 00 FF 75 10 E8 ?? ?? ?? ?? 83 C4 08 F7 40 14 00 00 80 00", 0x616C42);
        get = get ? call_target(get + 5) : nullptr;
        // FUN_00625130: PUSH EBP; MOV EBP,ESP; PUSH EBX; PUSH ESI; MOV ESI,[EBP+0xC]; PUSH EDI; MOV EDI,[EBP+8]
        if (get && !sig_match(get, "55 8B EC 53 56 8B 75 0C 57 8B 7D 08")) get = nullptr;
        if (start && get) {
            g_effstart = (effstart_t)start;
            g_cmdget = (cmdget_t)get;
            if (!patch_call_rel32(site + 9, start, (void*)&h_effstart)) g_effstart = nullptr;
        }
        log_line("  X2-DIAG-CRASH-SPELL: effect start names %s", g_effstart ? "logged" : "OFF (signature or patch failed)");
    }

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

// Dev hot reload: the vectored handler and the dump filter go (the next copy installs its own; both would dump).
static void stop() {
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
    if (g_setuef_tramp) g_setuef_tramp(g_game_uef);
    else if (g_dump > 0) SetUnhandledExceptionFilter(g_game_uef);
}

Row row_DIAGCRASH = {
    "X2-DIAG-CRASH",
    "diagnostic: log faults (module+offset, registers, stack), who calls ExitProcess / TerminateProcess, and a "
    "minidump on a fatal crash (X2-DIAG-CRASH-DUMP)",
    1, install, nullptr, post, 0, 0, stop
};

} // namespace x2
