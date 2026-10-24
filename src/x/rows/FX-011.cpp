// SPDX-License-Identifier: Apache-2.0
// FX-011: the per-frame draw packet allocator FUN_00728660 (6 pools x 0x156 packets of 0x214 bytes, reset every
// runner iteration at 0x822856, W2A-001) returns 0 when all pools are full, and its callers write the packet without
// a test (FUN_0071b8a0 at 0x71BC7C: MOV byte ptr [EDX],1). Heavy particle effects at 120/240 fps crash there
// (U/D 24/10/2026: magic_0289 underwater boss missiles, Photon Missiles, Valefor's entrance dust). The row hands
// back a scratch packet instead: callers fill it in place and nothing links it, so the sprite is dropped, not drawn.
// It also logs the peak packets per invocation kind, to show which effect overflows and at which rate.
// Details: docs/fps60/rows/FX-011.md
#include "../fps60.h"
#include <intrin.h>
#include <string.h>
#include <stdio.h>

typedef void*(__cdecl* alloc_t)(int count);

enum { POOLS = 6, POOL_PACKETS = 0x156 };

static uint8_t*  g_entry = nullptr;
static alloc_t   g_tramp = nullptr;
static volatile int32_t* g_counts = nullptr;      // DAT_00cec1c0[6]
static bool      g_patched = false;
static __declspec(align(16)) uint8_t g_scratch[0x1000];   // packet is 0x214; margin for other callers
static volatile LONG g_dropped = 0, g_dropped_inv = 0;
static int       g_peak[4];                       // per InvKind, packets in one invocation
// Packets per call site in the current invocation, and in each kind's peak invocation (the 9 callers, W2A-001).
enum { SITES = 32 };
static void*     g_site[SITES];
static int       g_cur[SITES], g_peak_by[4][SITES];
// Calls from the sprite builder FUN_0071b8a0 (0x7EA bytes, EBP frame) are keyed by the builder's own caller
// ([EBP+4] at the allocator entry): op 1, op 9 and 9 other draw paths share it.
static uint8_t*  g_builder = nullptr;
static uint8_t*  g_entry_ebp = nullptr;

// cdecl int(int count), plain RET (0x72873F / 0x728748): the detour keeps the convention.
static void* __cdecl h_alloc(int count) {
    void* ra = _ReturnAddress();
    if (g_builder && (uint8_t*)ra >= g_builder && (uint8_t*)ra < g_builder + 0x7EA && g_entry_ebp) {
        uint32_t up = 0;
        if (safe_read_u32(g_entry_ebp + 4, &up) && up) ra = (void*)(uintptr_t)up;
    }
    for (int i = 0; i < SITES; i++) {
        if (g_site[i] == ra) { g_cur[i]++; break; }
        if (!g_site[i]) { g_site[i] = ra; g_cur[i]++; break; }
    }
    void* p = g_tramp(count);
    if (p || g_disabled || !row_FX011.on) return p;
    if (InterlockedIncrement(&g_dropped) == 1) {
        log_line("FX-011: draw packet pools full (%d packets): first packet dropped, caller gva=0x%08X count %d, inv kind %d",
                 POOLS * POOL_PACKETS, rt_to_gva_exe(_ReturnAddress()), count, pacing_kind());
    }
    InterlockedIncrement(&g_dropped_inv);
    memset(g_scratch, 0, 0x214);
    return g_scratch;
}

// MinHook detour: keep the caller's EBP (the builder's frame) for h_alloc, then the C detour on the same stack.
__declspec(naked) static void h_alloc_entry() {
    __asm {
        mov  g_entry_ebp, ebp
        jmp  h_alloc
    }
}

static bool install() {
    // FUN_00728660: PUSH EBP; MOV EBP,ESP; OR EDX,-1; CMP [pool0 count],0x156; MOV EAX,100000
    g_entry = sig_find_unique("FX-011 draw packet allocator FUN_00728660",
        "55 8B EC 83 CA FF 81 3D ?? ?? ?? ?? 56 01 00 00 B8 A0 86 01 00", 0x728660);
    if (!g_entry) return false;
    g_counts = (volatile int32_t*)read_abs(g_entry + 8);
    if (!g_counts) return false;
    // Sprite builder FUN_0071b8a0: PUSH EBP; MOV EBP,ESP; SUB ESP,0x8C; cookie; copies args 1/2 to [EBP-0x88]/[EBP-0x8C]
    g_builder = sig_find_unique("FX-011 sprite builder FUN_0071b8a0",
        "55 8B EC 81 EC 8C 00 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 8B 45 08 89 85 78 FF FF FF 8B 45 0C 89 85 74 FF FF FF 0F B7 45 18",
        0x71B8A0);
    log_line("  FX-011: FUN_00728660 at gva=0x%08X, pool counts gva=0x%08X", rt_to_gva_exe(g_entry),
             rt_to_gva_exe((void*)g_counts));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("FX-011", g_entry, (void*)&h_alloc_entry, (void**)&g_tramp)) {
        log_line("FX-011: entry hook failed - row off");
        row_FX011.on = 0;
    }
}

// After Sg_MainLoop: the counts still hold this invocation's packets (the runner resets them before the next one).
static void post() {
    if (!g_counts) return;
    int used = 0;
    for (int i = 0; i < POOLS; i++) used += g_counts[i];
    int k = pacing_kind();
    if (k >= 0 && k < 4 && used > g_peak[k]) {
        g_peak[k] = used;
        memcpy(g_peak_by[k], g_cur, sizeof(g_cur));
    }
    memset(g_cur, 0, sizeof(g_cur));
    LONG d = InterlockedExchange(&g_dropped_inv, 0);
    static uint32_t tick = 0;
    static LONG dropped_window = 0;
    dropped_window += d;
    if (++tick % (300 * (uint32_t)rate_mult()) != 0) return;
    if (g_peak[0] || g_peak[1] || g_peak[2] || g_peak[3] || dropped_window)
        log_line("FX-011: peak draw packets per invocation (of %d): logic %d, render-only %d, backlog %d, stock %d; dropped %ld",
                 POOLS * POOL_PACKETS, g_peak[0], g_peak[1], g_peak[2], g_peak[3], dropped_window);
    static const char* kind_name[4] = { "logic", "render-only", "backlog", "stock" };
    for (int k2 = 0; k2 < 4; k2++) {
        if (g_peak[k2] < 600) continue;                  // the breakdown only for heavy invocations
        char line[1024];
        int n = 0;
        for (int i = 0; i < SITES && g_site[i]; i++)
            if (g_peak_by[k2][i])
                n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " 0x%06X:%d", rt_to_gva_exe(g_site[i]), g_peak_by[k2][i]);
        log_line("FX-011:   %s peak by caller (return gva:packets):%s", kind_name[k2], line);
    }
    for (int i = 0; i < 4; i++) g_peak[i] = 0;
    memset(g_peak_by, 0, sizeof(g_peak_by));
    dropped_window = 0;
}

Row row_FX011 = {
    "FX-011",
    "draw packet pools full (FUN_00728660 returns 0): drop the sprite instead of crashing (heavy particle effects at 120/240)",
    1, install, pre, post, 0
};
