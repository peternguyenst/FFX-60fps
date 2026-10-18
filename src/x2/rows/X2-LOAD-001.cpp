// SPDX-License-Identifier: Apache-2.0
// X2-LOAD-001 (FFX-2 only): stock pacing while the game is loading. re/ledger/LOAD-DECK.md (S + D).
// Mechanism (D 18/10/2026, Steam Deck / Proton): during a load the game logic only polls (Sg_MainLoop ~0.03 ms) while
// its data arrives in the background, and the main thread spends most of each frame waiting in PApplication::frame's
// command-buffer reservation FUN_008823f0 (0xAB76EE, waits for the render side when its ring is full: 10.3 s of a 15 s
// save load). The background work needs main-thread idle time between frames; at our 60 Hz the frame loop leaves
// almost none (save load 14 s; stock 2 s; fast with pacing=0; every other row ruled out by a bisect). The PC has the
// headroom and is unaffected. Skipping renders or presents at 60 Hz did not help; only the frame-loop rate does.
// 1) Load mode (default on): while App_Animate / Sg_AppRender (0x42F4C0 / 0x42F7F0) see app+0x3AD = FUN_0044cd70()
//    ([0x00DD10D0] != 0 and (byte +0x70 != 0 or int +0xE0 >= 0)), Sg_MainLoop does not run, so pacing_pre never sees
//    those frames; post() of an invocation that leaves the game in load mode puts the stock divider and exec 1 back
//    (pacing_leave_now). Logs each load and its length.
// 2) X2-LOAD-001-IDLEHOLD (default on under Wine / Proton, off on Windows): 20 logic invocations in a row under 0.1 ms
//    (the poll-wait; normal frames are 1-3 ms on the Deck) start a stock-pacing hold, released after 30 invocations in a
//    row >= 0.3 ms (frame work again), cap 20 s; after a cap it re-arms only once a busy frame was seen (a paused screen
//    also polls). U 18/10: save load 15.5 -> 1.5 s.
// Diagnostics (default 0): X2-LOAD-001-TRACE=N logs N invocations after a map-id change ([0x00D61748], X2-TITLE-001's
// site 0x466FCD) or a stock invocation at the game's own divider 1 (kind, ms, gap, backlog, divider, exec, HD state block
// [0x00DCED88] [0]/+0x338/+0x33C, PS2-menu flags [0x016BC55C]/[0x016BC560]); X2-LOAD-001-WAITS=1 hooks ntdll
// NtWaitForSingleObject / NtWaitForMultipleObjects / NtDelayExecution and sums the game thread's waits during the trace
// per three FFX-2.exe return addresses.
#include "../x2.h"
#include <winternl.h>

namespace x2 {

typedef int (__cdecl* loading_t)();
static loading_t g_loading = nullptr;            // FUN_0044cd70 (cdecl, no args, plain RET)
static bool  g_in = false;
static DWORD g_t0 = 0;
static LONG  g_n = 0;
extern Row row_LOAD001;
static volatile int32_t* g_map = nullptr;
static int32_t g_lastmap = 0x7FFFFFFF;
static int   g_trace = 0, g_tleft = 0;
static LARGE_INTEGER g_qf, g_tstart, g_tprev;
static uint8_t** g_hds = nullptr;                // &DAT_00dced88
static volatile int32_t* g_m1 = nullptr;         // 0x016BC55C
static volatile int32_t* g_m2 = nullptr;         // 0x016BC560
static bool  g_held = false;
static int   g_busy = 0;
static DWORD g_hold_t0 = 0;
static int   g_idlehold = 1;
static int   g_waits = 0;
static DWORD g_game_tid = 0;
struct WaitSite { uint32_t gva, gva2, gva3; int kind; LONG n; double ms; int64_t last_to; };
static WaitSite g_ws[48];
static int g_nws = 0;
typedef LONG (NTAPI* ntwso_t)(HANDLE, BOOLEAN, PLARGE_INTEGER);
typedef LONG (NTAPI* ntwmo_t)(ULONG, const HANDLE*, int, BOOLEAN, PLARGE_INTEGER);
typedef LONG (NTAPI* ntdel_t)(BOOLEAN, PLARGE_INTEGER);
static ntwso_t g_ntwso = nullptr;
static ntwmo_t g_ntwmo = nullptr;
static ntdel_t g_ntdel = nullptr;
static bool waits_live() { return g_waits && g_tleft > 0 && GetCurrentThreadId() == g_game_tid; }
// first three FFX-2.exe return addresses on the stack (D 19:56: the top one is the wait wrapper FUN_0088d3d0)
static void exe_callers(uint32_t* g) {
    void* fr[32];
    g[0] = g[1] = g[2] = 0;
    USHORT n = RtlCaptureStackBackTrace(1, 32, fr, nullptr);
    int k = 0;
    for (USHORT i = 0; i < n && k < 3; i++) if (in_image(fr[i])) g[k++] = rt_to_gva(fr[i]);
}
static void wait_note(int kind, double ms, PLARGE_INTEGER to) {
    uint32_t g[3];
    exe_callers(g);
    int i = 0;
    for (; i < g_nws; i++) if (g_ws[i].gva == g[0] && g_ws[i].gva2 == g[1] && g_ws[i].gva3 == g[2] && g_ws[i].kind == kind) break;
    if (i == g_nws) { if (g_nws >= 48) return; g_ws[g_nws++] = { g[0], g[1], g[2], kind, 0, 0.0, 0 }; }
    g_ws[i].n++; g_ws[i].ms += ms; g_ws[i].last_to = to ? to->QuadPart : -1;
}
static double qpc_ms(const LARGE_INTEGER& a) { LARGE_INTEGER b; QueryPerformanceCounter(&b); return (b.QuadPart - a.QuadPart) * 1000.0 / g_qf.QuadPart; }
static LONG NTAPI h_ntwso(HANDLE h, BOOLEAN al, PLARGE_INTEGER to) {
    if (!waits_live()) return g_ntwso(h, al, to);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    LONG r = g_ntwso(h, al, to);
    __try { wait_note(1, qpc_ms(t), to); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}
static LONG NTAPI h_ntwmo(ULONG c, const HANDLE* hs, int wt, BOOLEAN al, PLARGE_INTEGER to) {
    if (!waits_live()) return g_ntwmo(c, hs, wt, al, to);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    LONG r = g_ntwmo(c, hs, wt, al, to);
    __try { wait_note(2, qpc_ms(t), to); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}
static LONG NTAPI h_ntdel(BOOLEAN al, PLARGE_INTEGER to) {
    if (!waits_live()) return g_ntdel(al, to);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    LONG r = g_ntdel(al, to);
    __try { wait_note(3, qpc_ms(t), to); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}
static void waits_dump() {
    static const char* const K[4] = { "?", "WaitSingle", "WaitMulti", "Delay" };
    log_line("LOADWAITS: %d sites (game thread, during the trace)", g_nws);
    for (int i = 0; i < g_nws; i++)
        if (g_ws[i].ms >= 20.0)
        log_line("  %s callers gva=0x%08X < 0x%08X < 0x%08X n=%ld total %.1f ms avg %.2f ms last timeout %lld (100 ns, <0 relative)",
                 K[g_ws[i].kind], g_ws[i].gva, g_ws[i].gva2, g_ws[i].gva3, g_ws[i].n, g_ws[i].ms, g_ws[i].n ? g_ws[i].ms / g_ws[i].n : 0.0, (long long)g_ws[i].last_to);
    g_nws = 0;
}
static int   g_idle = 0;                         // consecutive cheap logic invocations
static const char* g_hold_why = "";

static bool  g_rearm_block = false;              // after a cap release: wait for a busy frame

static void hold_start(const char* why) {
    if (g_held) return;
    g_held = true;
    g_busy = 0;
    g_hold_t0 = GetTickCount();
    g_hold_why = why;
    pacing_hold_stock("X2-LOAD-001 hold", true);
    log_line("X2-LOAD-001: stock-pacing hold (%s)", why);
}

static void post() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double inv_ms = (now.QuadPart - g_tstart.QuadPart) * 1000.0 / g_qf.QuadPart;
    if (inv_ms >= 0.3) g_rearm_block = false;
    if (g_idlehold && !g_held && !g_rearm_block && pacing_kind() == INV_LOGIC) {
        g_idle = inv_ms < 0.1 ? g_idle + 1 : 0;
        if (g_idle >= 20) { g_idle = 0; hold_start("poll-wait: 20 cheap logic invocations"); }
    }
    if (g_held) {
        g_busy = inv_ms >= 0.3 ? g_busy + 1 : 0;
        DWORD el = GetTickCount() - g_hold_t0;
        if (g_busy >= 30 || el > 20000) {
            g_held = false;
            g_rearm_block = g_busy < 30;
            pacing_hold_stock("X2-LOAD-001 hold", false);
            log_line("X2-LOAD-001: hold (%s) released after %lu ms (%s)", g_hold_why, el, g_busy >= 30 ? "frame work again" : "cap");
        }
    }
    if (g_tleft > 0) {
        static const char* const K[4] = { "L", "R", "B", "S" };
        int st = -1, b8 = -1, bc = -1, m1 = -1, m2 = -1;
        __try {
            uint8_t* h = g_hds ? *g_hds : nullptr;
            if (h) { st = *(int32_t*)h; b8 = h[0x338]; bc = h[0x33C]; }
            if (g_m1) m1 = *g_m1;
            if (g_m2) m2 = *g_m2;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        log_line("LOADTRACE %s inv %.2f ms gap %.2f ms backlog %d div %u exec %d | hd %d %d %d menu %d %d", K[pacing_kind() & 3],
                 (now.QuadPart - g_tstart.QuadPart) * 1000.0 / g_qf.QuadPart,
                 (g_tstart.QuadPart - g_tprev.QuadPart) * 1000.0 / g_qf.QuadPart,
                 *g_backlog_count, *g_divider, *g_exec_byte, st, b8, bc, m1, m2);
        if (--g_tleft == 0) { log_line("LOADTRACE end"); if (g_waits) waits_dump(); }
    }
    bool l = false;
    __try { l = g_loading() != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!l) return;
    pacing_leave_now("X2-LOAD-001 (load mode)");
    if (!g_in) {
        g_in = true;
        g_t0 = GetTickCount();
        if (InterlockedIncrement(&g_n) <= 50) log_line("X2-LOAD-001: load mode entered - stock pacing for the load");
    }
}

static void pre() {
    g_game_tid = GetCurrentThreadId();
    if (g_trace) {
        static bool was_d1 = false;
        bool d1 = pacing_kind() == INV_STOCK && *g_divider == 1;
        if (d1 && !was_d1 && g_tleft == 0) { g_tleft = g_trace; log_line("LOADTRACE: stock invocation at divider 1 (menu at 60 fps), next %d invocations", g_trace); }
        was_d1 = d1;
    }
    if (g_map) {
        int32_t m = *g_map;
        if (m != g_lastmap) {
            if (g_lastmap != 0x7FFFFFFF) {
                if (g_trace) { g_tleft = g_trace; log_line("LOADTRACE: map 0x%X -> 0x%X, next %d invocations", g_lastmap, m, g_trace); }
            }
            g_lastmap = m;
        }
    }
    g_tprev = g_tstart;
    QueryPerformanceCounter(&g_tstart);
    if (!g_in) return;
    g_in = false;
    if (g_n <= 50) log_line("X2-LOAD-001: load over after %lu ms", GetTickCount() - g_t0);
}

static bool install() {
    // FUN_0044cd70: MOV EAX,[0x00dd10d0]; TEST EAX,EAX; JZ; CMP byte [EAX+0x70],0; JNZ; CMP dword [EAX+0xE0],0; JGE
    uint8_t* fn = sig_find("X2-LOAD-001 load-mode test FUN_0044cd70",
        "A1 ?? ?? ?? ?? 85 C0 74 1E 80 78 70 00 75 0F 83 B8 E0 00 00 00 00 7D 06", 0x44CD70);
    if (!fn) return false;
    g_loading = (loading_t)fn;
    g_trace = config_row("X2-LOAD-001-TRACE", 0);
    g_idlehold = config_row("X2-LOAD-001-IDLEHOLD", on_wine() ? 1 : 0);
    g_waits = g_trace ? config_row("X2-LOAD-001-WAITS", 0) : 0;
    if (g_waits) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        void* a = nt ? (void*)GetProcAddress(nt, "NtWaitForSingleObject") : nullptr;
        void* b = nt ? (void*)GetProcAddress(nt, "NtWaitForMultipleObjects") : nullptr;
        void* c = nt ? (void*)GetProcAddress(nt, "NtDelayExecution") : nullptr;
        bool ok = a && b && c && hook_entry("X2-LOAD-001-WAITS NtWaitForSingleObject", a, (void*)&h_ntwso, (void**)&g_ntwso)
               && hook_entry("X2-LOAD-001-WAITS NtWaitForMultipleObjects", b, (void*)&h_ntwmo, (void**)&g_ntwmo)
               && hook_entry("X2-LOAD-001-WAITS NtDelayExecution", c, (void*)&h_ntdel, (void**)&g_ntdel);
        log_line("  X2-LOAD-001-WAITS: ntdll wait hooks %s", ok ? "on" : "FAILED");
        if (!ok) g_waits = 0;
    }
    QueryPerformanceFrequency(&g_qf);
    QueryPerformanceCounter(&g_tstart);
    if (g_trace) {
        uint8_t* at = sig_find("X2-LOAD-001 map id (title-map test)", "83 3D ?? ?? ?? ?? 17 75 49 E8", 0x466FCD);
        g_map = at ? (volatile int32_t*)abs_operand("map id", at + 2) : nullptr;
        uint8_t* hs = sig_find("X2-LOAD-001 HD state block (list clear test)",
            "A3 ?? ?? ?? ?? 80 B8 38 03 00 00 00 75 0E 80 B8 3C 03 00 00 00 75 05 83 38 01 74 07 8B CE E8", 0x464FEF);
        g_hds = hs ? (uint8_t**)abs_operand("HD state ptr", hs + 1) : nullptr;
        uint8_t* mf = sig_find("X2-LOAD-001 menu early-out 0x60550C",
            "83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 01", 0x60550C);
        if (mf) { g_m1 = (volatile int32_t*)abs_operand("menu flag 1", mf + 2); g_m2 = (volatile int32_t*)abs_operand("menu flag 2", mf + 0xF); }
    }
    log_line("  X2-LOAD-001: load-mode test gva=0x%08X (expected 0x0044CD70); IDLEHOLD %s%s", rt_to_gva(fn),
             g_idlehold ? "on" : "off", on_wine() ? " (Wine / Proton)" : "");
    return true;
}

Row row_LOAD001 = {
    "X2-LOAD-001",
    "loads at stock pacing: the game's load-mode frames (no Sg_MainLoop) get the stock divider and exec 1",
    1, install, pre, post, 0
};

} // namespace x2
