// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-CRASH (diagnostic, default 0): U 18/10/2026: the game closed itself while a commsphere recording was viewed
// (log ends 0.0 s after magic_0760.dll loaded; no mod fault line, no Windows crash record or dump). Logs:
// - every first-chance fault of the kinds below (vectored handler, first in the chain): code, faulting module + offset
//   (FFX-2.exe also as Ghidra VA), the accessed address, registers, and up to 12 stack words that point into code
//   modules (return-address candidates). Faults inside the mod's own __try blocks are logged too (they are handled
//   afterwards). Capped at 40 lines.
// - ExitProcess / TerminateProcess(own process): the exit code and the caller chain, so a silent exit through the
//   game's own error path shows who asked for it.
// - every ~60 s (3600 invocations): private bytes, working set, free address space (total, largest block) and the
//   handle count. U 22/10/2026 (friend): crashes after ~30 min battles / back-to-back cups, log ends with no fault
//   line; this shows whether anything grows toward exhaustion before such a crash.
// Observe only: the handler always returns EXCEPTION_CONTINUE_SEARCH and the exit hooks call the originals.
#include "../x2.h"
#include <stdio.h>
#include <psapi.h>                 // K32* (kernel32, PSAPI_VERSION 2)

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

static LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c != EXCEPTION_ACCESS_VIOLATION && c != EXCEPTION_ILLEGAL_INSTRUCTION && c != EXCEPTION_PRIV_INSTRUCTION &&
        c != EXCEPTION_INT_DIVIDE_BY_ZERO && c != EXCEPTION_STACK_OVERFLOW && c != EXCEPTION_ARRAY_BOUNDS_EXCEEDED &&
        c != 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN (fast fail) */ && c != 0xC0000374 /* heap corruption */)
        return EXCEPTION_CONTINUE_SEARCH;
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
    static uint32_t n = 0;
    if (++n % 3600 == 1) log_mem();
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
    return true;
}

Row row_DIAGCRASH = {
    "X2-DIAG-CRASH",
    "diagnostic: log faults (module+offset, registers, stack) and who calls ExitProcess / TerminateProcess",
    0, install, nullptr, post, 0
};

} // namespace x2
