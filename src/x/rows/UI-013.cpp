// SPDX-License-Identifier: Apache-2.0
// UI-013: the screen distortion grid (9x9 wave, e.g. the underwater battle's water effect) FUN_00913200 draws the grid
// and then steps its two wave phases, +0xBE080 += speed*1.5 and +0xBE084 += speed (speed +0xBE090), once per
// Sg_MainLoop invocation (Sg_MainLoop tail -> FUN_00908550 -> FUN_009131d0; ledger UI.md UI-013). At N passes the
// wave ran N times faster (U 25/10/2026: underwater boss fight). Under mod pacing each invocation keeps 1/N of its
// step, so a logic tick advances the phases by the stock amount and every drawn frame moves smoothly.
// Details: docs/fps60/rows/UI-013.md
#include "../fps60.h"

typedef void(__cdecl* grid_t)(uint8_t* scene);

static uint8_t* g_entry = nullptr;
static grid_t   g_tramp = nullptr;
static bool     g_patched = false;
static volatile LONG g_scaled = 0, g_passed = 0;

// cdecl(scene), plain RET (0x913F2B).
static void __cdecl h_grid(uint8_t* scene) {
    int k = pacing_kind();
    int n = rate_mult();
    if (g_disabled || !row_UI013.on || n <= 1 || (k != INV_LOGIC && k != INV_BUILD)) {
        InterlockedIncrement(&g_passed);
        g_tramp(scene);
        return;
    }
    float a0 = 0, b0 = 0, a1 = 0, b1 = 0;
    bool ok = safe_copy(&a0, scene + 0xBE080, 4) && safe_copy(&b0, scene + 0xBE084, 4);
    g_tramp(scene);
    if (!ok || !safe_copy(&a1, scene + 0xBE080, 4) || !safe_copy(&b1, scene + 0xBE084, 4)) return;
    a1 = a0 + (a1 - a0) / (float)n;
    b1 = b0 + (b1 - b0) / (float)n;
    safe_copy(scene + 0xBE080, &a1, 4);
    safe_copy(scene + 0xBE084, &b1, 4);
    if (InterlockedIncrement(&g_scaled) == 1)
        log_line("UI-013: first distortion grid step scaled by 1/%d (inv kind %d)", n, k);
}

static bool install() {
    // FUN_00913200: prologue, cookie, FLDZ; PUSH EBX; MOV EBX,[EBP+8]; FST x2; PUSH ESI; TEST byte [EBX+0xBE078],1
    g_entry = sig_find_unique("UI-013 distortion grid FUN_00913200",
        "55 8B EC 81 EC 50 02 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC D9 EE 53 8B 5D 08 D9 95 B4 FD FF FF D9 95 D0 FD FF FF 56 F6 83 78 E0 0B 00 01",
        0x913200);
    if (!g_entry) return false;
    log_line("  UI-013: FUN_00913200 at gva=0x%08X (entry hooked on the game thread)", rt_to_gva_exe(g_entry));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("UI-013", g_entry, (void*)&h_grid, (void**)&g_tramp)) {
        log_line("UI-013: entry hook failed - row off");
        row_UI013.on = 0;
    }
}

Row row_UI013 = {
    "UI-013",
    "screen distortion grid (underwater water effect): wave phase step scaled to 1/N per invocation",
    1, install, pre, nullptr, 0
};
