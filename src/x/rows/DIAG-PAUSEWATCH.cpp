// SPDX-License-Identifier: Apache-2.0
// Diagnostic DIAG-PAUSEWATCH (observe only, default off): a hardware write watchpoint (DR0) on the particle pause
// flag DAT_0230fd34, game thread only. Every write that changes the value is recorded from a vectored handler;
// pre() logs the first few per writer (code address, value, invocation kind, caller chain) and a per-writer count
// summary. Writes from fps60's own save/restore (ppp_pause.cpp, MAG-006, MAG-018) are counted, not listed.
// At each magic DLL unload and when a logic invocation starts with the flag at 1, the last writer is logged.
// Why: the flag stayed 1 after Death (magic_0098) and Life (magic_0114) never finished (U 20/10/2026, twice).
// Details: docs/fps60/rows/DIAG-PAUSEWATCH.md
#include "../fps60.h"
#include <intrin.h>
#include <stdio.h>

extern volatile int32_t* g_ppp_pause;            // ppp_pause.cpp (resolved by MAG-006 / FX-010)

static volatile DWORD g_tid = 0;
static void* g_veh = nullptr;
static bool  g_armed = false;
static uint8_t* g_self = nullptr;                // fps60_ffx.dll base
static uint32_t g_self_size = 0;

struct Ev { uint32_t eip, ret[3]; int32_t val; int kind; LONG inv; };
static Ev g_ring[256];
static volatile LONG g_w = 0, g_r = 0;
static volatile LONG g_inv = 0, g_dropped = 0, g_self_writes = 0;
static Ev g_last_ext = {};                        // last change by code outside fps60

struct Writer { uint32_t eip; LONG to1, to0, shown; };
static Writer g_writers[32];

static bool in_self(uint32_t a) { return a - (uint32_t)(uintptr_t)g_self < g_self_size; }

static LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || GetCurrentThreadId() != g_tid) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    static int32_t prev = -1;
    int32_t v = *g_ppp_pause;
    if (v == prev) return EXCEPTION_CONTINUE_EXECUTION;
    prev = v;
    uint32_t eip = c->Eip;
    if (in_self(eip)) { g_self_writes++; return EXCEPTION_CONTINUE_EXECUTION; }
    Ev e = {};
    e.eip = eip; e.val = v; e.kind = pacing_kind(); e.inv = g_inv;
    uint32_t bp = c->Ebp;
    for (int i = 0; i < 3; i++) {
        uint32_t f[2];
        if (!safe_copy(f, (void*)(uintptr_t)bp, 8)) break;
        e.ret[i] = f[1];
        if (f[0] <= bp) break;
        bp = f[0];
    }
    g_last_ext = e;
    LONG w = g_w;
    if (w - g_r >= 256) { g_dropped++; return EXCEPTION_CONTINUE_EXECUTION; }
    g_ring[w & 255] = e;
    g_w = w + 1;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void where(uint32_t a, char* out, size_t cap) {
    HMODULE m = nullptr;
    if (!a || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)(uintptr_t)a, &m) || !m) {
        _snprintf_s(out, cap, _TRUNCATE, "0x%08X", a);
        return;
    }
    char path[MAX_PATH] = "";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* n = strrchr(path, '\\');
    n = n ? n + 1 : path;
    uint32_t off = a - (uint32_t)(uintptr_t)m;
    if ((uint8_t*)m == exe_base()) _snprintf_s(out, cap, _TRUNCATE, "FFX.exe gva 0x%08X", 0x400000u + off);
    else if (_strnicmp(n, "magic", 5) == 0) _snprintf_s(out, cap, _TRUNCATE, "%s gva 0x%08X", n, 0x10000000u + off);
    else _snprintf_s(out, cap, _TRUNCATE, "%s+0x%X", n, off);
}

static void ev_text(const Ev& e, char* out, size_t cap) {
    char a[96], r0[96], r1[96], r2[96];
    where(e.eip, a, sizeof(a)); where(e.ret[0], r0, sizeof(r0)); where(e.ret[1], r1, sizeof(r1)); where(e.ret[2], r2, sizeof(r2));
    _snprintf_s(out, cap, _TRUNCATE, "-> %ld by %s (inv %ld kind %d; callers %s < %s < %s)", (long)e.val, a, e.inv, e.kind,
                r0, r1, r2);
}

// Game thread suspended from a helper thread: DR0 = &flag, write, 4 bytes, local enable (param 0 = disarm).
static DWORD WINAPI arm(LPVOID on) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, g_tid);
    if (!h) return 1;
    bool ok = false;
    if (SuspendThread(h) != (DWORD)-1) {
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &c)) {
            c.Dr0 = on ? (DWORD)(uintptr_t)g_ppp_pause : 0;
            c.Dr7 = (c.Dr7 & ~0x000F0003u) | (on ? (1u | (1u << 16) | (3u << 18)) : 0u);
            ok = SetThreadContext(h, &c) != 0;
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    return ok ? 0 : 2;
}

static void on_unload(uint8_t* base) {
    if (!g_armed) return;
    char t[400];
    ev_text(g_last_ext, t, sizeof(t));
    log_line_try("DIAG-PAUSEWATCH: magic DLL @%p unloads with the flag at %ld; last outside change %s", base,
                 g_ppp_pause ? (long)*g_ppp_pause : -1L, t);
}

static bool install() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&install, &self);
    if (!self) return false;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)self;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)((uint8_t*)self + dos->e_lfanew);
    g_self = (uint8_t*)self;
    g_self_size = nt->OptionalHeader.SizeOfImage;
    magic_mods_subscribe("DIAG-PAUSEWATCH", nullptr, on_unload);
    log_line("  DIAG-PAUSEWATCH: hardware write watch on the particle pause flag, armed at the first invocation");
    return true;
}

static void pre() {
    LONG inv = InterlockedIncrement(&g_inv);
    if (!g_armed) {
        if (!g_ppp_pause) return;                 // MAG-006 / FX-010 resolve it
        g_tid = GetCurrentThreadId();
        g_veh = AddVectoredExceptionHandler(1, veh);
        HANDLE t = CreateThread(nullptr, 0, arm, (LPVOID)1, 0, nullptr);
        DWORD rc = 3;
        if (t) { WaitForSingleObject(t, 2000); GetExitCodeThread(t, &rc); CloseHandle(t); }
        g_armed = true;
        log_line("DIAG-PAUSEWATCH: watch on gva 0x%08X %s (flag now %ld)", rt_to_gva_exe((const void*)g_ppp_pause),
                 rc == 0 ? "armed" : "FAILED", (long)*g_ppp_pause);
        return;
    }
    char t[400];
    while (g_r != g_w) {
        Ev e = g_ring[g_r & 255];
        g_r = g_r + 1;
        int i = 0;
        while (i < 32 && g_writers[i].eip && g_writers[i].eip != e.eip) i++;
        if (i == 32) continue;
        Writer& w = g_writers[i];
        w.eip = e.eip;
        if (e.val) w.to1++; else w.to0++;
        if (w.shown < 4) {
            w.shown++;
            ev_text(e, t, sizeof(t));
            log_line("DIAG-PAUSEWATCH: flag %s", t);
        }
    }
    // 30 logic invocations in a row (~1 s) starting with the flag at 1: who set it (once per stretch).
    static int stuck = 0;
    if (pacing_kind() != INV_BUILD) {
        if (*g_ppp_pause == 1) {
            if (++stuck == 30) {
                ev_text(g_last_ext, t, sizeof(t));
                log_line("DIAG-PAUSEWATCH: flag at 1 for 30 logic invocations (inv %ld); last outside change %s "
                         "(fps60 save/restore writes so far %ld)", inv, t, g_self_writes);
            }
        } else stuck = 0;
    }
    if (inv % 1800 == 0) {
        for (int i = 0; i < 32 && g_writers[i].eip; i++) {
            where(g_writers[i].eip, t, sizeof(t));
            log_line("DIAG-PAUSEWATCH: writer %s: ->1 %ld, ->0 %ld", t, g_writers[i].to1, g_writers[i].to0);
        }
        if (g_dropped) log_line("DIAG-PAUSEWATCH: %ld events dropped (ring full)", g_dropped);
    }
}

// Dev hot reload: clear DR0 on the game thread before the handler goes, or the next write faults.
static void stop() {
    if (g_armed && g_tid) {
        if (GetCurrentThreadId() == g_tid) {
            HANDLE t = CreateThread(nullptr, 0, arm, nullptr, 0, nullptr);
            if (t) { WaitForSingleObject(t, 2000); CloseHandle(t); }
        } else arm(nullptr);
    }
    g_armed = false;
    if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }
}

Row row_DIAG_PAUSEWATCH = {
    "DIAG-PAUSEWATCH",
    "diagnostic: hardware write watch on the particle pause flag (who leaves it at 1); observe only",
    0, install, pre, nullptr, 0, stop
};
