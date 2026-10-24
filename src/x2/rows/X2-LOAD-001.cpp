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
// 3) X2-LOAD-001-BATTLEHOLD (default on under Wine / Proton): stock pacing from the battle setup to the running battle,
//    i.e. while the battle state [0x00DF68A0] (X2-SR-003's site 0x61E5E2) is in 13..25 after a battle start, until it
//    reaches 1; cap 15 s. D 19/10 Deck trace: states 0 -> 4 -> 6 -> 7 -> 9 -> 39 (encounter transition, 5.3 s, kept at
//    60 Hz) -> 13 (13 ms load spike) -> 15 .. 24 -> 25 (1.7 s; the party appears ~2 s after the enemies, U) -> 1;
//    the main thread waits 7.6 ms per frame in the command ring as during the save load.
// Diagnostics (default 0): X2-LOAD-001-TRACE=N logs N invocations after a map-id change ([0x00D61748], X2-TITLE-001's
// site 0x466FCD) or a stock invocation at the game's own divider 1 (kind, ms, gap, backlog, divider, exec, HD state block
// [0x00DCED88] [0]/+0x338/+0x33C, PS2-menu flags [0x016BC55C]/[0x016BC560]); X2-LOAD-001-WAITS=1 hooks ntdll
// NtWaitForSingleObject / NtWaitForMultipleObjects / NtDelayExecution and sums the game thread's waits during the trace
// per three FFX-2.exe return addresses. The trace also starts on a battle-state change ([0x00DF68A0], X2-SR-003's site
// 0x61E5E2; U 19/10 Deck: at battle start the party appears ~2 s after the enemies, PC fine) and logs that state.
#include "../x2.h"
#include <winternl.h>

namespace x2 {

typedef int (__cdecl* load_mode_test_fn)();
static load_mode_test_fn g_load_mode_test = nullptr;            // FUN_0044cd70 (cdecl, no args, plain RET)
static bool  g_in_load_mode = false;
static DWORD g_load_start_ms = 0;
static LONG  g_loads_seen = 0;
extern Row row_LOAD001;
static volatile int32_t* g_map_id = nullptr;
static int32_t g_last_map_id = 0x7FFFFFFF;
static int   g_opt_trace = 0, g_trace_left = 0;
static LARGE_INTEGER g_qpc_freq, g_inv_start, g_prev_inv_start;
static uint8_t** g_hd_state = nullptr;                // &DAT_00dced88
static volatile int32_t* g_menu_flag1 = nullptr;         // 0x016BC55C
static volatile int32_t* g_menu_flag2 = nullptr;         // 0x016BC560
static volatile int8_t*  g_battle_state = nullptr;        // battle state 0x00DF68A0
static int   g_opt_battlehold = 0;
static bool  g_battle_held = false;
static DWORD g_battle_hold_start_ms = 0;
static bool  g_idle_held = false;
static int   g_busy_invs = 0;
static DWORD g_idle_hold_start_ms = 0;
static int   g_opt_idlehold = 1;
static int   g_opt_waits = 0;
static DWORD g_game_thread_id = 0;
struct WaitSite { uint32_t gva, gva2, gva3; int kind; LONG n; double ms; int64_t last_to; };
static WaitSite g_wait_sites[48];
static int g_wait_site_count = 0;
typedef LONG (NTAPI* nt_wait_single_fn)(HANDLE, BOOLEAN, PLARGE_INTEGER);
typedef LONG (NTAPI* nt_wait_multi_fn)(ULONG, const HANDLE*, int, BOOLEAN, PLARGE_INTEGER);
typedef LONG (NTAPI* nt_delay_fn)(BOOLEAN, PLARGE_INTEGER);
static nt_wait_single_fn g_nt_wait_single = nullptr;
static nt_wait_multi_fn g_nt_wait_multi = nullptr;
static nt_delay_fn g_nt_delay = nullptr;
static bool waits_live() { return g_opt_waits && g_trace_left > 0 && GetCurrentThreadId() == g_game_thread_id; }
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
    for (; i < g_wait_site_count; i++) if (g_wait_sites[i].gva == g[0] && g_wait_sites[i].gva2 == g[1] && g_wait_sites[i].gva3 == g[2] && g_wait_sites[i].kind == kind) break;
    if (i == g_wait_site_count) { if (g_wait_site_count >= 48) return; g_wait_sites[g_wait_site_count++] = { g[0], g[1], g[2], kind, 0, 0.0, 0 }; }
    g_wait_sites[i].n++; g_wait_sites[i].ms += ms; g_wait_sites[i].last_to = to ? to->QuadPart : -1;
}
static double qpc_ms(const LARGE_INTEGER& a) { LARGE_INTEGER b; QueryPerformanceCounter(&b); return (b.QuadPart - a.QuadPart) * 1000.0 / g_qpc_freq.QuadPart; }
static LONG NTAPI hook_nt_wait_single(HANDLE h, BOOLEAN al, PLARGE_INTEGER to) {
    if (!waits_live()) return g_nt_wait_single(h, al, to);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    LONG r = g_nt_wait_single(h, al, to);
    __try { wait_note(1, qpc_ms(t), to); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}
static LONG NTAPI hook_nt_wait_multi(ULONG c, const HANDLE* hs, int wt, BOOLEAN al, PLARGE_INTEGER to) {
    if (!waits_live()) return g_nt_wait_multi(c, hs, wt, al, to);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    LONG r = g_nt_wait_multi(c, hs, wt, al, to);
    __try { wait_note(2, qpc_ms(t), to); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}
static LONG NTAPI hook_nt_delay(BOOLEAN al, PLARGE_INTEGER to) {
    if (!waits_live()) return g_nt_delay(al, to);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    LONG r = g_nt_delay(al, to);
    __try { wait_note(3, qpc_ms(t), to); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}
static void waits_dump() {
    static const char* const K[4] = { "?", "WaitSingle", "WaitMulti", "Delay" };
    log_line("LOADWAITS: %d sites (game thread, during the trace)", g_wait_site_count);
    for (int i = 0; i < g_wait_site_count; i++)
        if (g_wait_sites[i].ms >= 20.0)
        log_line("  %s callers gva=0x%08X < 0x%08X < 0x%08X n=%ld total %.1f ms avg %.2f ms last timeout %lld (100 ns, <0 relative)",
                 K[g_wait_sites[i].kind], g_wait_sites[i].gva, g_wait_sites[i].gva2, g_wait_sites[i].gva3, g_wait_sites[i].n, g_wait_sites[i].ms, g_wait_sites[i].n ? g_wait_sites[i].ms / g_wait_sites[i].n : 0.0, (long long)g_wait_sites[i].last_to);
    g_wait_site_count = 0;
}
static int   g_idle_invs = 0;                         // consecutive cheap logic invocations
static const char* g_hold_why = "";

static bool  g_rearm_block = false;              // after a cap release: wait for a busy frame

// Config page switches (X2-CFG-MENU): turning a hold off releases it now.
static void idle_release();
static void battle_release();
bool load_idlehold_get()   { return g_opt_idlehold != 0; }
void load_idlehold_set(bool on) { g_opt_idlehold = on; if (!on) idle_release(); }
bool load_battlehold_get() { return g_opt_battlehold != 0; }
void load_battlehold_set(bool on) { g_opt_battlehold = on; if (!on) battle_release(); }

static void idle_hold_start(const char* why) {
    if (g_idle_held) return;
    g_idle_held = true;
    g_busy_invs = 0;
    g_idle_hold_start_ms = GetTickCount();
    g_hold_why = why;
    pacing_hold_stock("X2-LOAD-001 hold", true);
    log_line("X2-LOAD-001: stock-pacing hold (%s)", why);
}

static void idle_release() {
    if (!g_idle_held) return;
    g_idle_held = false;
    pacing_hold_stock("X2-LOAD-001 hold", false);
    log_line("X2-LOAD-001: hold (%s) released (option turned off)", g_hold_why);
}
static void battle_release() {
    if (!g_battle_held) return;
    g_battle_held = false;
    pacing_hold_stock("X2-LOAD-001 battle setup", false);
    log_line("X2-LOAD-001-BATTLEHOLD: released (option turned off)");
}

static void post() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double inv_ms = (now.QuadPart - g_inv_start.QuadPart) * 1000.0 / g_qpc_freq.QuadPart;
    if (inv_ms >= 0.3) g_rearm_block = false;
    if (g_opt_idlehold && !g_idle_held && !g_rearm_block && pacing_kind() == INV_LOGIC) {
        g_idle_invs = inv_ms < 0.1 ? g_idle_invs + 1 : 0;
        if (g_idle_invs >= 20) { g_idle_invs = 0; idle_hold_start("poll-wait: 20 cheap logic invocations"); }
    }
    if (g_idle_held) {
        g_busy_invs = inv_ms >= 0.3 ? g_busy_invs + 1 : 0;
        DWORD el = GetTickCount() - g_idle_hold_start_ms;
        if (g_busy_invs >= 30 || el > 20000) {
            g_idle_held = false;
            g_rearm_block = g_busy_invs < 30;
            pacing_hold_stock("X2-LOAD-001 hold", false);
            log_line("X2-LOAD-001: hold (%s) released after %lu ms (%s)", g_hold_why, el, g_busy_invs >= 30 ? "frame work again" : "cap");
        }
    }
    if (g_trace_left > 0) {
        static const char* const K[4] = { "L", "R", "B", "S" };
        int st = -1, b8 = -1, bc = -1, m1 = -1, m2 = -1;
        __try {
            uint8_t* h = g_hd_state ? *g_hd_state : nullptr;
            if (h) { st = *(int32_t*)h; b8 = h[0x338]; bc = h[0x33C]; }
            if (g_menu_flag1) m1 = *g_menu_flag1;
            if (g_menu_flag2) m2 = *g_menu_flag2;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        log_line("LOADTRACE %s inv %.2f ms gap %.2f ms backlog %d div %u exec %d | hd %d %d %d menu %d %d bst %d", K[pacing_kind() & 3],
                 (now.QuadPart - g_inv_start.QuadPart) * 1000.0 / g_qpc_freq.QuadPart,
                 (g_inv_start.QuadPart - g_prev_inv_start.QuadPart) * 1000.0 / g_qpc_freq.QuadPart,
                 *g_backlog_count, *g_divider, *g_exec_byte, st, b8, bc, m1, m2, g_battle_state ? (int)*g_battle_state : -1);
        if (--g_trace_left == 0) { log_line("LOADTRACE end"); if (g_opt_waits) waits_dump(); }
    }
    bool l = false;
    __try { l = g_load_mode_test() != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!l) return;
    pacing_leave_now("X2-LOAD-001 (load mode)");
    if (!g_in_load_mode) {
        g_in_load_mode = true;
        g_load_start_ms = GetTickCount();
        if (InterlockedIncrement(&g_loads_seen) <= 50) log_line("X2-LOAD-001: load mode entered - stock pacing for the load");
    }
}

static void pre() {
    g_game_thread_id = GetCurrentThreadId();
    if (g_opt_trace) {
        static bool was_d1 = false;
        bool d1 = pacing_kind() == INV_STOCK && *g_divider == 1;
        if (d1 && !was_d1 && g_trace_left == 0) { g_trace_left = g_opt_trace; log_line("LOADTRACE: stock invocation at divider 1 (menu at 60 fps), next %d invocations", g_opt_trace); }
        was_d1 = d1;
    }
    if (g_opt_battlehold && g_battle_state) {
        int b = *g_battle_state;
        if (!g_battle_held && b >= 13 && b <= 25) {
            g_battle_held = true;
            g_battle_hold_start_ms = GetTickCount();
            pacing_hold_stock("X2-LOAD-001 battle setup", true);
            log_line("X2-LOAD-001-BATTLEHOLD: battle setup (state %d) - stock pacing until the battle runs", b);
        } else if (g_battle_held && (b < 13 || b > 25 || GetTickCount() - g_battle_hold_start_ms > 15000)) {
            g_battle_held = false;
            pacing_hold_stock("X2-LOAD-001 battle setup", false);
            log_line("X2-LOAD-001-BATTLEHOLD: released after %lu ms (state %d%s)", GetTickCount() - g_battle_hold_start_ms, b,
                     (b >= 13 && b <= 25) ? ", cap" : "");
        }
    }
    if (g_opt_trace && g_battle_state) {
        static int last = -1000;
        int b = *g_battle_state;
        if (b != last) {
            if (last != -1000 && g_trace_left == 0) { g_trace_left = g_opt_trace; log_line("LOADTRACE: battle state %d -> %d, next %d invocations", last, b, g_opt_trace); }
            else if (last != -1000) log_line("LOADTRACE: battle state %d -> %d", last, b);
            last = b;
        }
    }
    if (g_map_id) {
        int32_t m = *g_map_id;
        if (m != g_last_map_id) {
            if (g_last_map_id != 0x7FFFFFFF) {
                if (g_opt_trace) { g_trace_left = g_opt_trace; log_line("LOADTRACE: map 0x%X -> 0x%X, next %d invocations", g_last_map_id, m, g_opt_trace); }
            }
            g_last_map_id = m;
        }
    }
    g_prev_inv_start = g_inv_start;
    QueryPerformanceCounter(&g_inv_start);
    if (!g_in_load_mode) return;
    g_in_load_mode = false;
    if (g_loads_seen <= 50) log_line("X2-LOAD-001: load over after %lu ms", GetTickCount() - g_load_start_ms);
}

static bool install() {
    // FUN_0044cd70: MOV EAX,[0x00dd10d0]; TEST EAX,EAX; JZ; CMP byte [EAX+0x70],0; JNZ; CMP dword [EAX+0xE0],0; JGE
    uint8_t* fn = sig_find("X2-LOAD-001 load-mode test FUN_0044cd70",
        "A1 ?? ?? ?? ?? 85 C0 74 1E 80 78 70 00 75 0F 83 B8 E0 00 00 00 00 7D 06", 0x44CD70);
    if (!fn) return false;
    g_load_mode_test = (load_mode_test_fn)fn;
    g_opt_trace = config_row("X2-LOAD-001-TRACE", 0);
    g_opt_idlehold = config_row("X2-LOAD-001-IDLEHOLD", on_wine() ? 1 : 0);
    g_opt_battlehold = config_row("X2-LOAD-001-BATTLEHOLD", on_wine() ? 1 : 0);
    g_opt_waits = g_opt_trace ? config_row("X2-LOAD-001-WAITS", 0) : 0;
    if (g_opt_waits) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        void* a = nt ? (void*)GetProcAddress(nt, "NtWaitForSingleObject") : nullptr;
        void* b = nt ? (void*)GetProcAddress(nt, "NtWaitForMultipleObjects") : nullptr;
        void* c = nt ? (void*)GetProcAddress(nt, "NtDelayExecution") : nullptr;
        bool ok = a && b && c && hook_entry("X2-LOAD-001-WAITS NtWaitForSingleObject", a, (void*)&hook_nt_wait_single, (void**)&g_nt_wait_single)
               && hook_entry("X2-LOAD-001-WAITS NtWaitForMultipleObjects", b, (void*)&hook_nt_wait_multi, (void**)&g_nt_wait_multi)
               && hook_entry("X2-LOAD-001-WAITS NtDelayExecution", c, (void*)&hook_nt_delay, (void**)&g_nt_delay);
        log_line("  X2-LOAD-001-WAITS: ntdll wait hooks %s", ok ? "on" : "FAILED");
        if (!ok) g_opt_waits = 0;
    }
    QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&g_inv_start);
    {                                               // also for a runtime switch-on from the Config page
        uint8_t* bs = sig_find("X2-LOAD-001 battle state (teardown test)",
            "80 3D ?? ?? ?? ?? 26 74 52 A0 ?? ?? ?? ?? 3C 05 74 49 3C 06 0F 85", 0x61E5E2);
        g_battle_state = bs ? (volatile int8_t*)abs_operand("battle state", bs + 2) : nullptr;
    }
    if (g_opt_trace) {
        uint8_t* at = sig_find("X2-LOAD-001 map id (title-map test)", "83 3D ?? ?? ?? ?? 17 75 49 E8", 0x466FCD);
        g_map_id = at ? (volatile int32_t*)abs_operand("map id", at + 2) : nullptr;
        uint8_t* hs = sig_find("X2-LOAD-001 HD state block (list clear test)",
            "A3 ?? ?? ?? ?? 80 B8 38 03 00 00 00 75 0E 80 B8 3C 03 00 00 00 75 05 83 38 01 74 07 8B CE E8", 0x464FEF);
        g_hd_state = hs ? (uint8_t**)abs_operand("HD state ptr", hs + 1) : nullptr;
        uint8_t* mf = sig_find("X2-LOAD-001 menu early-out 0x60550C",
            "83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 01", 0x60550C);
        if (mf) { g_menu_flag1 = (volatile int32_t*)abs_operand("menu flag 1", mf + 2); g_menu_flag2 = (volatile int32_t*)abs_operand("menu flag 2", mf + 0xF); }

    }
    log_line("  X2-LOAD-001: load-mode test gva=0x%08X (expected 0x0044CD70); IDLEHOLD %s, BATTLEHOLD %s%s", rt_to_gva(fn),
             g_opt_idlehold ? "on" : "off", g_opt_battlehold && g_battle_state ? "on" : "off", on_wine() ? " (Wine / Proton)" : "");
    return true;
}

Row row_LOAD001 = {
    "X2-LOAD-001",
    "loads at stock pacing: the game's load-mode frames (no Sg_MainLoop) get the stock divider and exec 1",
    1, install, pre, post, 0
};

} // namespace x2
