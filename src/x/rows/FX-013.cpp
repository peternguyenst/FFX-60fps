// SPDX-License-Identifier: Apache-2.0
// FX-013: bigger effect pools, so effects that make more draws or particles at 120-240 fps fit instead of being cut
// (FX-011 / FX-012 drop what does not fit; they stay as the net).
// - Draw packets (W2A-001): FUN_00728660 hands out 0x214-byte packets from 6 pools of 0x156, buffers DAT_00cec1a8[6]
//   allocated once at startup (FUN_00728750 from 0x820860, never freed) and walked by count (FUN_0071ed10). The row
//   gives the pools buffers of FX-013-PACKETS x 0x156 packets and raises the 6 `CMP [count],0x156` in the allocator.
//   The per-pool byte sum limit (100000) is kept.
// - Particle (PPP) pools: FUN_007169d0 makes a pool descriptor current (*PTR_DAT_00c3a4cc) and sets up its main pool
//   (the battle's DAT_00cec22c is a 0x40000-byte array in FFX.exe's data; magic DLLs have their own), then zeroes
//   desc[5], the memory of its second pool at desc+0x10. The allocators try that second pool when the main one is
//   full. The row gives each descriptor a FX-013-PPP-KB second pool after FUN_007169d0 returns, allocated top-down:
//   frees pick the pool by `block <= main pool end`, so the second pool must lie above the main one. The game's own
//   second-pool block DAT_00cec224 is left alone (the game frees only that one).
// Only above 60 fps (user 25/10/2026: not every case tested yet). The row follows rate_mult() every invocation: the
// draw pools switch between the game's buffers + limit 0x156 and ours + the raised limit (between invocations, after
// the count reset, so no packet is in use); second particle pools are added while the rate is above 60 (also
// mid-scene) but never taken away mid-scene (blocks may live there), so at 60 a scene set up later has none, as stock.
// Details: docs/fps60/rows/FX-013.md
#include "../fps60.h"
#include <intrin.h>

typedef void(__cdecl* pinit_t)(uint32_t* desc, void* mem, int size);
typedef void(__cdecl* pmain_t)(uint32_t* desc, void* scene, void* mem, int size);

enum { POOLS = 6, STOCK_PACKETS = 0x156, PACKET = 0x214, MAX_DESC = 8 };
static const int k_cmp_off[POOLS] = { 0x06, 0x25, 0x42, 0x5F, 0x7C, 0x99 };   // CMP dword [count_i],0x156

static uint8_t*   g_alloc = nullptr;             // FUN_00728660
static uint8_t**  g_bases = nullptr;             // DAT_00cec1a8[6]
static uint8_t*   g_orig_bases[POOLS];
static uint8_t*   g_new_bases[POOLS];
static int        g_big = -1;                    // draw pools now: 1 ours, 0 the game's, -1 not set
static uint8_t*   g_pinit = nullptr;             // FUN_0072c480
static pinit_t    g_pinit_tramp = nullptr;
static uint8_t*   g_pmain = nullptr;             // FUN_007169d0
static pmain_t    g_pmain_tramp = nullptr;
static uint32_t** g_cur = nullptr;               // PTR_DAT_00c3a4cc: the current pool descriptor
static uint32_t*  g_main = nullptr;              // DAT_00cec22c
static uint32_t*  g_second = nullptr;            // DAT_00cec23c
static uint8_t*   g_ppp_buf = nullptr;           // the main descriptor's second-pool buffer (allocated at install)
struct SecPool { uint32_t* desc; uint8_t* buf; LONG setups; };
static SecPool    g_sec[MAX_DESC];
static int        g_ppp_size = 0, g_mult = 4;
static bool       g_patched = false, g_packets_on = false;
static int        g_cap = STOCK_PACKETS;

int draw_packet_cap() { return g_cap; }          // FX-011's log

static bool above60() { return rate_mult() > 2; }

// FUN_0072c480 cdecl(desc, mem, size), plain RET 0x72C4CE: the game's own second-pool setup gets our buffer.
static void __cdecl h_pinit(uint32_t* desc, void* mem, int size) {
    if (row_FX013.on && !g_disabled && g_ppp_buf && desc == g_second && above60()) {
        g_pinit_tramp(desc, g_ppp_buf, g_ppp_size);
        return;
    }
    g_pinit_tramp(desc, mem, size);
}

// A second pool for a descriptor whose main pool is set up and has none (desc[5] = 0).
static void ensure_second(uint32_t* desc, int size, void* caller) {
    if (!desc || !desc[1] || desc[5]) return;
    SecPool* sp = nullptr;
    for (int i = 0; i < MAX_DESC && !sp; i++) {
        if (g_sec[i].desc == desc) sp = &g_sec[i];
        else if (!g_sec[i].desc) {
            uint8_t* b = desc == g_main ? g_ppp_buf
                       : (uint8_t*)VirtualAlloc(nullptr, g_ppp_size, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
            if (!b) return;
            g_sec[i].desc = desc;
            g_sec[i].buf = b;
            sp = &g_sec[i];
        }
    }
    if (!sp || (uintptr_t)sp->buf <= (uintptr_t)desc[2]) return;   // must lie above that descriptor's main pool end
    g_pinit_tramp(desc + 4, sp->buf, g_ppp_size);
    LONG n = InterlockedIncrement(&sp->setups);
    if (n == 1 || n % 50 == 0)
        log_line("FX-013: particle second pool set up for pool gva=0x%08X (%d KB at %p, its main pool %d KB, caller gva=0x%08X; setup %ld)",
                 rt_to_gva_exe(desc), g_ppp_size / 1024, sp->buf, size / 1024, rt_to_gva_exe(caller), n);
}

// FUN_007169d0 cdecl(desc, scene, mem, size), plain RET 0x716A23: it ends with desc[5] = 0, so the second pool is
// set up after it returns.
static void __cdecl h_pmain(uint32_t* desc, void* scene, void* mem, int size) {
    g_pmain_tramp(desc, scene, mem, size);
    if (row_FX013.on && !g_disabled && g_ppp_buf && above60()) ensure_second(desc, size, _ReturnAddress());
}

// The allocator limit immediates. The first write goes through patch_write (hot-reload revert to 0x156); the later
// switches are written directly, because repeated patch_write records of one site would revert to a wrong value.
static void set_limit(int v) {
    for (int i = 0; i < POOLS; i++) {
        uint8_t* at = g_alloc + k_cmp_off[i] + 6;
        DWORD old;
        if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        InterlockedExchange((volatile LONG*)at, v);
        VirtualProtect(at, 4, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), g_alloc, 0xA0);
}

static void apply_rate() {
    bool big = above60();
    if (g_packets_on && (int)big != g_big) {
        int cap = STOCK_PACKETS * g_mult;
        for (int i = 0; i < POOLS; i++) g_bases[i] = big ? g_new_bases[i] : g_orig_bases[i];
        set_limit(big ? cap : STOCK_PACKETS);
        g_cap = big ? cap : STOCK_PACKETS;
        g_big = big ? 1 : 0;
        log_line("FX-013: draw packets %d per pool (%d in all)%s", g_cap, g_cap * POOLS, big ? "" : " - stock at 60 fps");
    }
    if (big && g_ppp_buf) {                      // above 60: the current pools get a second pool if they have none
        ensure_second(g_main, 0, nullptr);
        if (g_cur && *g_cur && *g_cur != g_main) ensure_second(*g_cur, 0, nullptr);
    }
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
        g_packets_on = g_bases != nullptr;
        for (int i = 0; i < POOLS && g_packets_on; i++)
            if (!sig_match(g_alloc + k_cmp_off[i], "81 3D ?? ?? ?? ?? 56 01 00 00")) {
                log_line("  FX-013: allocator compare %d not where expected - draw packets left at stock", i);
                g_packets_on = false;
            }
    }

    // Particle pools: second-pool setup CMP [DAT_00cec224],0; ...; PUSH desc; MOV [DAT_00cec224],EAX; CALL FUN_0072c480
    uint8_t* site = sig_find_unique("FX-013 second particle pool setup (FUN_0072a7f0)",
        "83 3D ?? ?? ?? ?? 00 75 2C A1 ?? ?? ?? ?? 8A 40 01 84 C0 74 20 0F B6 F0 C1 E6 0D 56 E8 ?? ?? ?? ?? 56 50 68 ?? ?? ?? ?? A3 ?? ?? ?? ?? E8",
        0x72A831);
    g_pinit = sig_find_unique("FX-013 pool setup FUN_0072c480", "55 8B EC 8B 55 08 8B 4D 0C 8B 45 10 89 4A 04", 0x72C480);
    // FUN_007169d0: PUSH EBP; MOV EBP,ESP; MOV EAX,[PTR_DAT_00c3a4cc]; PUSH ESI; MOV ESI,[EBP+8]; PUSH EDI; MOV EDI,[EBP+0xC]; MOV [EAX],ESI
    g_pmain = sig_find_unique("FX-013 current-pool setup FUN_007169d0", "55 8B EC A1 ?? ?? ?? ?? 56 8B 75 08 57 8B 7D 0C 89 30 85 FF", 0x7169D0);
    if (site && g_pinit && g_pmain) {
        g_cur = (uint32_t**)read_abs(g_pmain + 4);
        g_second = (uint32_t*)read_abs(site + 0x24);
        g_main = g_second ? g_second - 4 : nullptr;   // DAT_00cec22c = DAT_00cec23c - 0x10
        g_ppp_buf = g_main ? (uint8_t*)VirtualAlloc(nullptr, g_ppp_size, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE)
                           : nullptr;
        if (!g_ppp_buf) log_line("  FX-013: %d KB for the particle pool not available - left at stock", kb);
    }
    if (!g_packets_on && !g_ppp_buf) return false;
    log_line("  FX-013: above 60 fps only; draw packets %s; particle second pools %s (main pool gva=0x%08X, second gva=0x%08X)",
             g_packets_on ? "x FX-013-PACKETS" : "stock", g_ppp_buf ? "FX-013-PPP-KB" : "stock",
             g_main ? rt_to_gva_exe(g_main) : 0, g_second ? rt_to_gva_exe(g_second) : 0);
    return true;
}

// Every invocation, game thread, after the runner reset the packet counts (0x822856): no packet is handed out yet,
// so the bases and the limit can change together.
static void pre() {
    if (g_patched) {
        if (row_FX013.on && !g_disabled) apply_rate();
        return;
    }
    g_patched = true;
    if (g_packets_on) {
        int cap = STOCK_PACKETS * g_mult;
        bool ok = true;
        for (int i = 0; i < POOLS && ok; i++) {
            g_orig_bases[i] = g_bases[i];
            g_new_bases[i] = (uint8_t*)VirtualAlloc(nullptr, (SIZE_T)cap * PACKET, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            ok = g_new_bases[i] != nullptr;
        }
        if (ok) {                                // ours + raised limit; the patch_write records the revert to 0x156
            for (int i = 0; i < POOLS; i++) g_bases[i] = g_new_bases[i];
            for (int i = 0; i < POOLS; i++) ok = patch_write(g_alloc + k_cmp_off[i] + 6, &cap, 4) && ok;
            g_big = 1;
            g_cap = cap;
        }
        if (!ok) {
            for (int i = 0; i < POOLS; i++) if (g_orig_bases[i]) g_bases[i] = g_orig_bases[i];
            set_limit(STOCK_PACKETS);
            g_cap = STOCK_PACKETS;
            g_packets_on = false;
            log_line("FX-013: draw packet buffers or patch failed - stock limit");
        } else {
            log_line("FX-013: draw packet buffers ready: %d per pool (%d in all, stock %d) while above 60 fps", cap,
                     cap * POOLS, STOCK_PACKETS * POOLS);
        }
    }
    if (g_ppp_buf && (!hook_entry("FX-013 pool setup", g_pinit, (void*)&h_pinit, (void**)&g_pinit_tramp) ||
                      !hook_entry("FX-013 current-pool setup", g_pmain, (void*)&h_pmain, (void**)&g_pmain_tramp))) {
        log_line("FX-013: pool setup hook failed - particle pools left at stock");
        g_ppp_buf = nullptr;
    }
    // Starts (or hot-reloads) inside a scene, or at 60: the state follows the rate now.
    apply_rate();
}

Row row_FX013 = {
    "FX-013",
    "bigger effect pools above 60 fps: draw packets x FX-013-PACKETS, particle second pools FX-013-PPP-KB",
    1, install, pre, nullptr, 0
};
