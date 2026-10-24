// SPDX-License-Identifier: Apache-2.0
// Particle programs step on every draw, so at 60 fps they run twice as fast. On render-only frames,
// DLL callers step with the engine's particle pause flag set: the particles draw but don't advance.
// Details: docs/fps60/rows/MAG-006.md
#include "../fps60.h"
#include <string.h>
#include <intrin.h>
#include <stdio.h>

int      ppp_step_paused(int (__cdecl* tramp)(void*, uint32_t), void* prog, uint32_t flags);
bool     ppp_resolve_pause(const char* row, const uint8_t* cmp_at);
bool     ppp_hook_spawn_call(const char* row, uint8_t* site);

typedef int(__cdecl* ppp_step_t)(void* prog, uint32_t flags);

static uint8_t*   g_entry = nullptr;
static ppp_step_t g_tramp = nullptr;
static bool       g_patched = false;
static volatile LONG g_paused_calls = 0;

// Only DLL callers are paused; EXE callers (OPU, BG list) keep stock behaviour (pausing them crashed).
static volatile LONG g_exe_calls = 0;

// A program that has never advanced steps unpaused: a paused first step draws with unset state (crash).
static volatile LONG g_fresh_calls = 0;
static int g_fresh_on = 1;                       // [rows] MAG-006-FRESH (bisect switch)
static int g_unpause = 0;                        // [rows] MAG-006-UNPAUSE: diagnostic, DLL programs unpaused (2x)
extern volatile int32_t* g_ppp_pause;

// Diagnostic: which EXE call sites step particle programs on render-only invocations (game
// thread only, so no locking). Logged by post() every ~10 s while it changes.
struct CallerCount { uint8_t* ret; uint32_t n; };
static CallerCount g_callers[16];
static bool g_callers_dirty = false;

// EXE call sites another row asked to pause like DLL callers (return addresses; init thread, before the hook).
static uint8_t* g_pause_exe[4];
static int      g_npause_exe = 0;
static volatile LONG g_exe_paused_calls = 0;

uint8_t* mag006_entry() { return row_MAG006.on ? g_entry : nullptr; }
bool mag006_pause_exe_caller(uint8_t* ret) {
    if (g_npause_exe >= 4) return false;
    g_pause_exe[g_npause_exe++] = ret;
    return true;
}
static bool listed(const uint8_t* ret) {
    for (int i = 0; i < g_npause_exe; i++) if (g_pause_exe[i] == ret) return true;
    return false;
}

static void note_exe_caller(uint8_t* ret) {
    for (int i = 0; i < 16; i++) {
        if (g_callers[i].ret == ret) { g_callers[i].n++; return; }
        if (!g_callers[i].ret) { g_callers[i].ret = ret; g_callers[i].n = 1; g_callers_dirty = true; return; }
    }
}

// One EXE call site a row counts per invocation kind, paused or not (MAG-029 diagnostic).
static uint8_t*       g_watch_ret = nullptr;
static volatile LONG* g_watch_counts = nullptr;
void mag006_watch_caller(uint8_t* ret, volatile LONG* counts) { g_watch_counts = counts; g_watch_ret = ret; }

static int __cdecl h_ppp_step(void* prog, uint32_t flags) {
    if (fx012_failed(prog)) return 1;            // FX-012: init failed (pool full), ended instead of stepped
    if (g_watch_ret && (uint8_t*)_ReturnAddress() == g_watch_ret) InterlockedIncrement(&g_watch_counts[pacing_kind() & 3]);
    if (row_MAG006.on && !g_disabled && pacing_kind() == INV_BUILD) {
        uint8_t* ret = (uint8_t*)_ReturnAddress();
        uint8_t* base = exe_base();
        if (ret >= base && ret < base + exe_size() && !listed(ret)) {
            InterlockedIncrement(&g_exe_calls);
            note_exe_caller(ret);
            return g_tramp(prog, flags);
        }
        if (g_fresh_on && *(int32_t*)((uint8_t*)prog + 8) == 0 && *(int32_t*)((uint8_t*)prog + 0xC) == 0) {
            InterlockedIncrement(&g_fresh_calls);
            return g_tramp(prog, flags);
        }
        if (g_unpause) {
            int32_t saved = *g_ppp_pause;
            *g_ppp_pause = 0;
            int r = g_tramp(prog, flags);
            *g_ppp_pause = saved;
            return r;
        }
        InterlockedIncrement(&g_paused_calls);
        if (ret >= base && ret < base + exe_size() && InterlockedIncrement(&g_exe_paused_calls) == 1)
            log_line("MAG-006: first paused step from a listed EXE caller (return gva 0x%08X)", rt_to_gva_exe(ret));
        return ppp_step_paused(g_tramp, prog, flags);
    }
    return g_tramp(prog, flags);
}

static bool install() {
    // Ppp_ProgramStep: prologue up to its spawn CALL at +0x1D
    g_entry = sig_find_unique("MAG-006 Ppp_ProgramStep",
        "55 8B EC F6 45 0C 10 56 8B 75 08 89 35 ?? ?? ?? ?? 74 09 56 E8 ?? ?? ?? ?? 83 C4 04 56 E8", 0x712250);
    if (!g_entry) return false;
    if (!ppp_resolve_pause("MAG-006", g_entry + 0x3A)) return false;
    g_fresh_on = config_row("MAG-006-FRESH", 1);
    g_unpause = config_row("MAG-006-UNPAUSE", 0);
    if (g_unpause) log_line("  MAG-006: DIAG UNPAUSE - DLL programs step unpaused on render-only invocations (2x)");
    log_line("  MAG-006: v4 fresh-program exception %s (MAG-006-FRESH)", g_fresh_on ? "ON" : "OFF");
    // Spawn skip is a separate switch: it crashed together with the pause.
    if (config_row("MAG-006-SPAWN", 0)) {
        if (!ppp_hook_spawn_call("MAG-006", g_entry + 0x1D)) return false;
    } else {
        log_line("  MAG-006: spawn skip OFF (MAG-006-SPAWN=0)");
    }
    log_line("  MAG-006: Ppp_ProgramStep gva=0x%08X, entry hooked on the game thread",
             rt_to_gva_exe(g_entry));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("MAG-006", g_entry, (void*)&h_ppp_step, (void**)&g_tramp)) {
        log_line("MAG-006: entry hook failed - row off (the spawn stub stays, it is harmless alone)");
        row_MAG006.on = 0;
        return;
    }
    log_line("MAG-006: Ppp_ProgramStep entry hooked on the game thread (tramp %p)", (void*)g_tramp);
}

static void post() {
    static uint32_t tick = 0;
    if (++tick % 600 == 0 && g_callers_dirty) {
        g_callers_dirty = false;
        char line[512]; int k = 0;
        for (int i = 0; i < 16 && g_callers[i].ret; i++)
            k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " 0x%08X:%u", rt_to_gva_exe(g_callers[i].ret), g_callers[i].n);
        log_line("MAG-006 diag: EXE callers of Ppp_ProgramStep on render-only invocations (return gva:count):%s", line);
    }
    static LONG reported = 0;
    if (!reported && g_paused_calls > 0) {
        reported = 1;
        log_line("MAG-006: first paused DLL Ppp_ProgramStep on a render-only invocation (EXE callers passed: %ld)", g_exe_calls);
    }
    static LONG fresh_seen = 0;
    if (g_fresh_calls != fresh_seen && tick % 300 == 0) {
        fresh_seen = g_fresh_calls;
        log_line("MAG-006 v4: %ld fresh DLL programs stepped unpaused on render-only invocations so far", fresh_seen);
    }
}

Row row_MAG006 = {
    "MAG-006",
    "DLL particle programs: pause flag on render-only invocations (draw still runs); EXE callers untouched",
    1, install, pre, post, 0
};
