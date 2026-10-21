// SPDX-License-Identifier: Apache-2.0
// CLK-013: SyncData cutscenes (re/ledger/CLK.md CLK-013) pin the scene to a wall-clock timeline. Sg_WaitFrameSync
// fetches one timeline entry per invocation (Sg_FetchSyncEntry 0x821D60), spins until its time, and adds backlog
// invocations to catch up. At 60 Hz render-only invocations consume entries too, so the scene's logic falls behind
// the voice (U 17/10/2026: cutscene before Yunalesca, voice out of sync and never back, also reported by other
// users without turbo).
// Fix: hold stock 30 Hz pacing while the SyncData mode flag DAT_012fb8a0 is 1 (CLK-013=1; CLK-013-HOLD=0 only logs).
// Default on again (21/10/2026, user: lips out of sync with the voice in a cutscene); was off 17/10-21/10.
// CLK-013-DIAG=1 logs the entries fetched per invocation kind every 120 invocations while the mode is on.
// Details: docs/fps60/rows/CLK-013.md
#include "../fps60.h"

static volatile uint32_t* g_mode = nullptr;   // DAT_012fb8a0: 1 = SyncData timeline active
static const float* g_target = nullptr;       // DAT_012fb8a4 (ms)
static const float* g_elapsed = nullptr;      // DAT_012fb8a8 (ms)
static void (__cdecl* g_fetch_tramp)() = nullptr;
static bool  g_active = false, g_holding = false;
static int   g_hold = 1;                      // CLK-013-HOLD: 0 = log only (the scene stays at 60 Hz)
static int   g_diag = 0;
static LONG  g_fetch[4], g_inv[4], g_n = 0;

static void __cdecl h_fetch() {
    InterlockedIncrement(&g_fetch[pacing_kind() & 3]);
    g_fetch_tramp();
}

static void release_hold() {
    g_active = false;
    if (!g_holding) return;
    g_holding = false;
    pacing_hold_stock("CLK-013 (SyncData scene)", false);
}

static void pre() {
    bool on = *g_mode == 1;
    if (on == g_active) return;
    g_active = on;
    log_line("CLK-013: SyncData timeline %s (target %.0f ms, elapsed %.0f ms)", on ? "starts" : "ends", *g_target, *g_elapsed);
    if (!g_hold || on == g_holding) return;
    g_holding = on;
    pacing_hold_stock("CLK-013 (SyncData scene)", on);
}

static void post() {
    if (!g_diag || !g_active) return;
    g_inv[pacing_kind() & 3]++;
    if (++g_n < 120) return;
    log_line("CLK-013: last %ld invocations L/B/K/S %ld/%ld/%ld/%ld, entries fetched %ld/%ld/%ld/%ld; elapsed-target %.1f ms",
             g_n, g_inv[0], g_inv[1], g_inv[2], g_inv[3], g_fetch[0], g_fetch[1], g_fetch[2], g_fetch[3], *g_elapsed - *g_target);
    g_n = 0;
    for (int i = 0; i < 4; i++) { g_inv[i] = 0; InterlockedExchange(&g_fetch[i], 0); }
}

static bool install() {
    // Sg_FetchSyncEntry: PUSH &threshold; PUSH &entry; CALL read; CMP EAX,999; JNZ; FLDZ; PUSH 0x78; FST [target] ...
    uint8_t* f = sig_find_unique("CLK-013 Sg_FetchSyncEntry",
        "55 8B EC 51 8D 45 FC 50 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? C7 45 FC 00 00 00 00 E8 ?? ?? ?? ?? 83 C4 0C "
        "3D E7 03 00 00 75 ?? D9 EE 6A 78 D9 15 ?? ?? ?? ?? 6A 00 D9 15 ?? ?? ?? ?? 68 ?? ?? ?? ?? D9 15 ?? ?? ?? ?? "
        "C7 05 ?? ?? ?? ?? 00 00 00 00", 0x821D60);
    if (!f) return false;
    uint8_t* entry = read_abs(f + 0xE);          // DAT_012fb8ac
    uint8_t* mode = read_abs(f + 0x47);          // DAT_012fb8a0 (MOV [mode],0 at the sentinel)
    if (read_abs(f + 0x2E) != entry - 8 || read_abs(f + 0x36) != entry - 4 || mode != entry - 0xC) {
        log_line("  CLK-013: SyncData globals do not line up (entry %p, mode %p) - refused", entry, mode);
        return false;
    }
    g_mode = (volatile uint32_t*)mode;
    g_target = (const float*)(entry - 8);
    g_elapsed = (const float*)(entry - 4);
    g_diag = config_row("CLK-013-DIAG", 0);
    g_hold = config_row("CLK-013-HOLD", 1);
    if (!hook_entry("CLK-013 Sg_FetchSyncEntry", f, (void*)&h_fetch, (void**)&g_fetch_tramp)) return false;
    log_line("  CLK-013: Sg_FetchSyncEntry gva=0x%08X (expected 0x00821D60), mode gva=0x%08X (0x012FB8A0); %s; diag=%d",
             rt_to_gva_exe(f), rt_to_gva_exe(mode), g_hold ? "stock pacing while a SyncData scene runs" : "log only (HOLD=0)", g_diag);
    return true;
}

Row row_CLK013 = {
    "CLK-013",
    "hold stock pacing while a SyncData (wall-clock synced) cutscene runs (voice sync)",
    1, install, pre, post, 0, release_hold
};
