// SPDX-License-Identifier: Apache-2.0
// X2-MAG-031: a spell cannot leave all particles paused behind it (fixes battle soft-locks).
//
// Some spell DLLs write the global particle pause flag from their draw, from the exec flag: magic_0424 writes 1 when
// exec is 0, magic_0840 writes -1. In the stock game exec is never 0 at a draw, so the flag ends at 0. At 60 fps the
// DLL's last draw before it unloads can be a render-only one, which leaves the flag set; nothing else clears it, so
// every later particle effect stays paused, the next effect never finishes and the battle waits forever.
// The fix: after a spell DLL unloads, the next invocation (on the game thread) puts a non-zero flag back to 0, unless
// a battle script set it on purpose (pppFpStopStatus) after that unload. Evidence: re/ledger/HANG-0073.md.
#include "../x2.h"

namespace x2 {

extern Row row_MAG031;

typedef void(__cdecl* set_t)(int);
static uint8_t* g_fn = nullptr;
static set_t    g_orig = nullptr;
static volatile int32_t* g_flag = nullptr;
static volatile LONG g_pending = 0;             // set by a DLL unload (loader thread), consumed in pre()
static volatile LONG g_script_set = 0;          // pppFpStopStatus called since the last unload
static volatile LONG g_cleared = 0;

static void __cdecl h_set(int v) {
    InterlockedExchange(&g_script_set, 1);
    g_orig(v);
}

static void on_load(uint8_t*, uint32_t, const char*) {}
static void on_unload(uint8_t*) {
    InterlockedExchange(&g_script_set, 0);
    InterlockedExchange(&g_pending, 1);
}

static bool install() {
    g_fn = sig_find("X2-MAG-031 pppFpStopStatus",
        "55 8B EC 56 8B 75 08 56 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 08 89 35 ?? ?? ?? ?? 5E 5D C3", 0x810EF0);
    if (!g_fn) return false;
    g_flag = (volatile int32_t*)abs_operand("particle pause flag", g_fn + 0x17);
    if (!g_flag) return false;
    if (!magic_mods_subscribe("X2-MAG-031", on_load, on_unload)) return false;
    log_line("  X2-MAG-031: pppFpStopStatus gva=0x%08X, flag gva=0x%08X (expected 0x00810EF0 / 0x01CF579C)", rt_to_gva(g_fn),
             rt_to_gva((const void*)g_flag));
    return true;
}

static void pre() {
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        if (!hook_entry("X2-MAG-031", g_fn, (void*)&h_set, (void**)&g_orig))
            log_line("X2-MAG-031: setter hook failed - a script-set flag is not told apart (cleared too)");
    }
    if (!InterlockedExchange(&g_pending, 0)) return;
    int32_t v = *g_flag;
    if (v == 0 || g_script_set) return;
    *g_flag = 0;
    if (InterlockedIncrement(&g_cleared) <= 20)
        log_line("X2-MAG-031: particle pause flag left at %d by an unloaded magic DLL - cleared (%ld so far)", v, g_cleared);
}

Row row_MAG031 = {
    "X2-MAG-031",
    "particle pause flag left set (1 or -1) by a magic DLL's last render-only draw is cleared after its unload (battle soft-lock)",
    1, install, pre, nullptr, 0
};

} // namespace x2
