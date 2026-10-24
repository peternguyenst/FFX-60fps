// SPDX-License-Identifier: Apache-2.0
// FX-013: bigger effect pools, so effects that make more draws or particles at 120-240 fps fit instead of being cut
// (FX-011 / FX-012 drop what does not fit; they stay as the net).
// - Draw packets (W2A-001): FUN_00728660 hands out 0x214-byte packets from 6 pools of 0x156, buffers DAT_00cec1a8[6]
//   allocated once at startup (FUN_00728750 from 0x820860, never freed) and walked by count (FUN_0071ed10). The row
//   gives the pools buffers of FX-013-PACKETS x 0x156 packets and raises the 6 `CMP [count],0x156` in the allocator.
//   The per-pool byte sum limit (100000) is kept.
// - Particle (PPP) pool: the main pool is a 0x40000-byte array in FFX.exe's data (descriptor DAT_00cec22c, set up per
//   scene by FUN_007169d0 -> FUN_0072c480). When it is full the allocators try the second pool (descriptor
//   DAT_00cec23c), which the game only makes for some scenes (heap block DAT_00cec224 of n * 8 KB, freed at scene
//   end). The row sets the second pool up after every main-pool setup with its own FX-013-PPP-KB buffer, allocated
//   top-down: frees pick the pool by `block <= main pool end`, so the second pool must lie above the main one. The
//   game's DAT_00cec224 is left alone (the game frees only its own block).
// Details: docs/fps60/rows/FX-013.md
#include "../fps60.h"

typedef void(__cdecl* pinit_t)(uint32_t* desc, void* mem, int size);
typedef void(__cdecl* pmain_t)(uint32_t* desc, void* scene, void* mem, int size);

enum { POOLS = 6, STOCK_PACKETS = 0x156, PACKET = 0x214 };

static uint8_t*  g_alloc = nullptr;              // FUN_00728660
static uint8_t** g_bases = nullptr;              // DAT_00cec1a8[6]
static uint8_t*  g_pinit = nullptr;              // FUN_0072c480
static pinit_t   g_pinit_tramp = nullptr;
static uint8_t*  g_pmain = nullptr;              // FUN_007169d0: current-pool setup (sets up the main pool, then main[5] = 0)
static pmain_t   g_pmain_tramp = nullptr;
static uint32_t* g_main = nullptr;               // DAT_00cec22c
static uint32_t* g_second = nullptr;             // DAT_00cec23c
static uint8_t*  g_ppp_buf = nullptr;
static int       g_ppp_size = 0, g_mult = 4;
static bool      g_patched = false, g_packets_on = false;
static volatile LONG g_setups = 0;
static int       g_cap = STOCK_PACKETS;

int draw_packet_cap() { return g_cap; }          // FX-011's log

// FUN_0072c480 cdecl(desc, mem, size), plain RET 0x72C4CE: the game's own second-pool setup gets our buffer.
static void __cdecl h_pinit(uint32_t* desc, void* mem, int size) {
    if (row_FX013.on && !g_disabled && g_ppp_buf && desc == g_second) { g_pinit_tramp(desc, g_ppp_buf, g_ppp_size); return; }
    g_pinit_tramp(desc, mem, size);
}

// FUN_007169d0 cdecl(desc, scene, mem, size), plain RET 0x716A23. It ends with desc[5] = 0, which for the main pool
// is the second pool's memory pointer (DAT_00cec240): the second pool has to be set up after it returns.
static void __cdecl h_pmain(uint32_t* desc, void* scene, void* mem, int size) {
    g_pmain_tramp(desc, scene, mem, size);
    if (!row_FX013.on || g_disabled || !g_ppp_buf || desc != g_main) return;
    if ((uintptr_t)g_ppp_buf <= (uintptr_t)desc[2]) return;   // must lie above the main pool's end
    g_pinit_tramp(g_second, g_ppp_buf, g_ppp_size);
    LONG n = InterlockedIncrement(&g_setups);
    if (n == 1 || n % 20 == 0)
        log_line("FX-013: particle second pool set up (%d KB at %p, main pool %d KB; setup %ld)", g_ppp_size / 1024, g_ppp_buf,
                 size / 1024, n);
}

static bool install() {
    g_mult = config_row("FX-013-PACKETS", 4);
    if (g_mult < 1) g_mult = 1;
    if (g_mult > 16) g_mult = 16;
    int kb = config_row("FX-013-PPP-KB", 4096);
    if (kb < 256) kb = 256;
    g_ppp_size = kb * 1024;

    // Draw packets: the allocator (FX-011's signature) and its base-array read MOV EAX,[EDX*4+bases]; ADD EAX,-0x214
    g_alloc = sig_find_unique("FX-013 draw packet allocator FUN_00728660",
        "55 8B EC 83 CA FF 81 3D ?? ?? ?? ?? 56 01 00 00 B8 A0 86 01 00", 0x728660);
    uint8_t* rd = sig_find_unique("FX-013 packet base read (FUN_00728660 tail)", "8B 04 95 ?? ?? ?? ?? 05 EC FD FF FF", 0x728730);
    if (g_alloc && rd) {
        g_bases = (uint8_t**)read_abs(rd + 3);
        static const int cmp_off[POOLS] = { 0x06, 0x25, 0x42, 0x5F, 0x7C, 0x99 };   // CMP dword [count_i],0x156
        g_packets_on = g_bases != nullptr;
        for (int i = 0; i < POOLS && g_packets_on; i++)
            if (!sig_match(g_alloc + cmp_off[i], "81 3D ?? ?? ?? ?? 56 01 00 00")) {
                log_line("  FX-013: allocator compare %d not where expected - draw packets left at stock", i);
                g_packets_on = false;
            }
    }

    // Particle pool: second-pool setup CMP [DAT_00cec224],0; ...; PUSH desc; MOV [DAT_00cec224],EAX; CALL FUN_0072c480
    uint8_t* site = sig_find_unique("FX-013 second particle pool setup (FUN_0072a7f0)",
        "83 3D ?? ?? ?? ?? 00 75 2C A1 ?? ?? ?? ?? 8A 40 01 84 C0 74 20 0F B6 F0 C1 E6 0D 56 E8 ?? ?? ?? ?? 56 50 68 ?? ?? ?? ?? A3 ?? ?? ?? ?? E8",
        0x72A831);
    g_pinit = sig_find_unique("FX-013 pool setup FUN_0072c480", "55 8B EC 8B 55 08 8B 4D 0C 8B 45 10 89 4A 04", 0x72C480);
    // FUN_007169d0: PUSH EBP; MOV EBP,ESP; MOV EAX,[PTR_DAT_00c3a4cc]; PUSH ESI; MOV ESI,[EBP+8]; PUSH EDI; MOV EDI,[EBP+0xC]; MOV [EAX],ESI
    g_pmain = sig_find_unique("FX-013 current-pool setup FUN_007169d0", "55 8B EC A1 ?? ?? ?? ?? 56 8B 75 08 57 8B 7D 0C 89 30 85 FF", 0x7169D0);
    if (!g_pmain) g_pinit = nullptr;
    if (site && g_pinit) {
        g_second = (uint32_t*)read_abs(site + 0x24);
        g_main = g_second ? g_second - 4 : nullptr;   // DAT_00cec22c = DAT_00cec23c - 0x10
        g_ppp_buf = (uint8_t*)VirtualAlloc(nullptr, g_ppp_size, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
        if (!g_ppp_buf) log_line("  FX-013: %d KB for the particle pool not available - left at stock", kb);
    }
    if (!g_packets_on && !g_ppp_buf) return false;
    log_line("  FX-013: draw packets %s; particle second pool %s (main pool gva=0x%08X, second gva=0x%08X)",
             g_packets_on ? "x FX-013-PACKETS at the first invocation" : "stock",
             g_ppp_buf ? "FX-013-PPP-KB from the next scene setup" : "stock", g_main ? rt_to_gva_exe(g_main) : 0,
             g_second ? rt_to_gva_exe(g_second) : 0);
    return true;
}

// First invocation, game thread, after the runner reset the packet counts (0x822856): no packet is handed out yet,
// so the bases and the limit can change together.
static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (g_packets_on) {
        int cap = STOCK_PACKETS * g_mult;
        uint8_t* nb[POOLS] = {};
        bool ok = true;
        for (int i = 0; i < POOLS && ok; i++) {
            nb[i] = (uint8_t*)VirtualAlloc(nullptr, (SIZE_T)cap * PACKET, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            ok = nb[i] != nullptr;
        }
        static const int cmp_off[POOLS] = { 0x06, 0x25, 0x42, 0x5F, 0x7C, 0x99 };
        if (ok) {
            for (int i = 0; i < POOLS; i++) g_bases[i] = nb[i];     // old buffers stay allocated (never freed by the game)
            for (int i = 0; i < POOLS; i++) ok = patch_write(g_alloc + cmp_off[i] + 6, &cap, 4) && ok;
        }
        if (ok) {
            g_cap = cap;
            log_line("FX-013: draw packets %d per pool (%d in all, stock %d)", cap, cap * POOLS, STOCK_PACKETS * POOLS);
        } else {
            log_line("FX-013: draw packet buffers or patch failed - stock limit");
        }
    }
    if (g_ppp_buf && (!hook_entry("FX-013 pool setup", g_pinit, (void*)&h_pinit, (void**)&g_pinit_tramp) ||
                      !hook_entry("FX-013 current-pool setup", g_pmain, (void*)&h_pmain, (void**)&g_pmain_tramp))) {
        log_line("FX-013: pool setup hook failed - particle pool left at stock");
        g_ppp_buf = nullptr;
    }
}

Row row_FX013 = {
    "FX-013",
    "bigger effect pools: draw packets x FX-013-PACKETS, particle second pool FX-013-PPP-KB (fewer drops at 120-240 fps)",
    1, install, pre, nullptr, 0
};
