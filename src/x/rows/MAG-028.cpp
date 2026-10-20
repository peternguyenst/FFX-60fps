// SPDX-License-Identifier: Apache-2.0
// MAG-028: the particle pause flag DAT_0230fd34 back to the value the game's own setter asked for, at the start of
// every logic invocation, while magic DLLs are involved. Every magic DLL that writes the flag writes (exec==0) from
// its draw (131 DLLs, at least three code shapes; magic_0098 Death: *f = 0; if (exec == 0) *f = 1 at 0x1000334C),
// so at stock (exec never 0) a logic invocation always starts with the flag at the script's value. Under fps60 a
// render-only draw leaves it at 1; if the DLL then unloads, every particle program stays frozen (Death then Life:
// magic_0114's records never finish, the battle waits for its end signal; D DIAG-PAUSEWATCH 20/10/2026).
// The script/debug setter FUN_0072a750 (cdecl(int), plain RET) is hooked to know what the game asked for.
// Details: docs/fps60/rows/MAG-028.md
#include "../fps60.h"
#include <stdio.h>

extern volatile int32_t* g_ppp_pause;            // ppp_pause.cpp (resolved by MAG-006 / FX-010)

static uint8_t* g_setter = nullptr;              // FUN_0072a750
static void (__cdecl* g_setter_tramp)(int) = nullptr;
static volatile int32_t g_wanted = 0;            // last value the game's setter wrote
static volatile LONG g_loaded = 0;               // magic DLLs loaded now
static volatile bool g_dirty = false;            // a magic DLL was loaded since the flag was last 0 at a logic start
static volatile LONG g_resets = 0;
static bool g_hooked = false;

static void __cdecl h_setter(int v) {
    g_wanted = v;
    g_setter_tramp(v);
}

static void on_load(uint8_t*, uint32_t, const char*) { InterlockedIncrement(&g_loaded); g_dirty = true; }
static void on_unload(uint8_t*) { InterlockedDecrement(&g_loaded); }

static bool install() {
    if (!g_ppp_pause) { log_line("  MAG-028: particle pause flag not resolved (needs MAG-006 or FX-010) - row off"); return false; }
    // FUN_0072a750: PUSH EBP; MOV EBP,ESP; MOV EAX,[EBP+8]; MOV [flag],EAX; POP EBP; RET
    uint32_t f = (uint32_t)(uintptr_t)g_ppp_pause;
    char pat[80];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "55 8B EC 8B 45 08 A3 %02X %02X %02X %02X 5D C3",
                f & 0xFF, (f >> 8) & 0xFF, (f >> 16) & 0xFF, f >> 24);
    int hits = 0;
    g_setter = sig_scan_module(exe_base(), exe_size(), pat, &hits);
    if (hits != 1) { log_line("  MAG-028: pause setter FUN_0072a750 found %d times - row off", hits); return false; }
    if (!magic_mods_subscribe("MAG-028", on_load, on_unload)) return false;
    log_line("  MAG-028: pause setter gva=0x%08X (expected 0x0072A750), flag gva=0x%08X", rt_to_gva_exe(g_setter),
             rt_to_gva_exe((const void*)g_ppp_pause));
    return true;
}

static void pre() {
    if (!g_hooked) {
        g_hooked = true;
        if (!hook_entry("MAG-028", g_setter, (void*)&h_setter, (void**)&g_setter_tramp)) {
            log_line("MAG-028: setter hook failed - row off");
            row_MAG028.on = 0;
            return;
        }
        log_line("MAG-028: FUN_0072a750 hooked on the game thread (flag now %ld)", (long)*g_ppp_pause);
    }
    if (g_disabled || pacing_kind() == INV_BUILD) return;
    int32_t v = *g_ppp_pause;
    if (v == g_wanted) {
        if (v == 0 && g_loaded == 0) g_dirty = false;
        return;
    }
    if (!g_dirty || g_wanted != 0 || v != 1) return;  // not a magic-DLL leftover: leave it
    *g_ppp_pause = 0;
    LONG n = InterlockedIncrement(&g_resets);
    if (g_loaded == 0 && (n <= 20 || n % 100 == 0))
        log_line("MAG-028: particle pause flag left at 1 with no magic DLL loaded - reset to 0 (%ld resets so far)", n);
    else if (n == 1)
        log_line("MAG-028: first reset of the particle pause flag at a logic invocation (magic DLL loaded)");
}

static void stop() {
    if (g_hooked && g_setter) hook_remove("MAG-028", g_setter);
    g_hooked = false;
}

Row row_MAG028 = {
    "MAG-028",
    "particle pause flag back to the game's own value at each logic invocation (magic DLLs leave it at 1)",
    1, install, pre, nullptr, 0, stop
};
