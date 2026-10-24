// SPDX-License-Identifier: Apache-2.0
// X2-MAG-019: fixes a crash when a particle effect is drawn before its materials are loaded.
//
// A particle draw can reach an effect instance whose materials are not filled in yet; the game's lazy shader lookup
// (FUN_004b9d50) then reads garbage and crashes. The 60 fps cadence of paused draws makes this happen (Blitz Ace's
// DLL in FFX-2, Thunder in FFX). The fix: while the instance's data, material count, material array or first material
// name is missing, the lookup returns 1 ("failed") without running. The game's own failure path then frees the
// half-built instance, and a later draw sets it up again. (Returning 0, "go on", crashed one step later.) Counted per
// invocation kind. Evidence: re/ledger/CRASH-0749.md.
#include "../x2.h"
#include <string.h>

namespace x2 {

typedef int(__fastcall* lookup_t)(void* self, void* edx, int a);   // thiscall(this, a), RET 4

static uint8_t* g_fn = nullptr;
static lookup_t g_tramp = nullptr;
static bool     g_patched = false;
static volatile LONG g_trips[4] = {0, 0, 0, 0};
extern Row row_MAG019;

static bool materials_ready(uint8_t* self) {
    __try {
        uint8_t* data = *(uint8_t**)(self + 0x1C);
        if (!data) return false;
        if (*(int32_t*)(data + 0x84) < 1) return false;
        uint8_t* mat = *(uint8_t**)(data + 0x88);
        if (!mat) return false;
        const char* name = *(const char**)(mat + 0x24);
        if (!name) return false;
        volatile char c = *name;                 // unreadable (0xCDCDCDCD) -> not ready
        (void)c;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static int __fastcall h_lookup(void* self, void* edx, int a) {
    if (row_MAG019.on && !g_disabled && !materials_ready((uint8_t*)self)) {
        int k = pacing_kind() & 3;
        LONG n = InterlockedIncrement(&g_trips[k]);
        if (n <= 5 || n % 100 == 0)
            log_line("X2-MAG-019: shader setup skipped, materials not set (this %p, kind %d, trip #%ld of this kind)",
                     self, k, n);
        return 1;                                // abort: FUN_00852880 frees the instance (FUN_00456270)
    }
    return g_tramp(self, edx, a);
}

static bool install() {
    // PUSH EBP; MOV EBP,ESP; SUB ESP,0x8C; cookie; PUSH EBX/ESI/EDI; MOV EDI,ECX; PUSH "D3D11/"; ...;
    // MOV EAX,[EDI+0x1C]; MOV EAX,[EAX+0x88]; PUSH [EAX+0x24]; CALL [strstr]
    g_fn = sig_find("X2-MAG-019 VFX shader lookup FUN_004b9d50",
        "55 8B EC 81 EC 8C 00 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 53 56 57 8B F9 68 ?? ?? ?? ?? 8B 47 38 8D 5F 38 "
        "89 85 74 FF FF FF 8B 47 1C 8B 80 88 00 00 00 FF 70 24 FF 15", 0x4B9D50);
    if (!g_fn) return false;
    const char* s = (const char*)abs_operand("X2-MAG-019 \"D3D11/\" string", g_fn + 0x19);
    char buf[7] = {};
    if (!s || !safe_copy(buf, s, 6) || memcmp(buf, "D3D11/", 6) != 0) {
        log_line("  X2-MAG-019: pushed string is not \"D3D11/\" - refused");
        return false;
    }
    log_line("  X2-MAG-019: shader lookup gva=0x%08X (expected 0x004B9D50); skipped while materials are unset",
             rt_to_gva(g_fn));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("X2-MAG-019", g_fn, (void*)&h_lookup, (void**)&g_tramp)) {
        log_line("X2-MAG-019: entry hook failed - row off");
        row_MAG019.on = 0;
    }
}

Row row_MAG019 = {
    "X2-MAG-019",
    "VFX shader lookup (FUN_004b9d50): skipped while the sub-instance's materials are unset (strstr crash)",
    1, install, pre, nullptr, 0
};

} // namespace x2
