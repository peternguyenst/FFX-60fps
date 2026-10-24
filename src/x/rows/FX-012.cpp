// SPDX-License-Identifier: Apache-2.0
// FX-012: particle (PPP) pool full. The program init 0x7123C0 allocates the spawn table from the PPP pool
// (FUN_00716a30 -> FUN_0072c3d0, two pools at *PTR_DAT_00c3a4cc). When both are full it marks the record
// rec+0 = 0xFFFFF000 and returns with the table rec+0x3C NULL, and nothing checks the mark: Ppp_ProgramStep 0x712250
// walks the table (FUN_00716c40 0x716C8E) and reads [0x10]. Crash D 25/10/2026 00:28/00:36/00:39, 150-240 fps,
// magic_0385 (record dumped: FFFFF000, count 0xC, table 0). Guard: a step of a marked record returns 1 (ended), so the
// DLL kills (+0xB24 0x711F20 frees only non-null children) and unlinks it; the effect loses that emitter. The step's
// entry is MAG-006's hook, so MAG-006's detour asks fx012_failed() first.
// Diagnostic: allocations and frees in the pool per caller and invocation kind, failures, and the net count, every
// ~10 s, to find what fills the pool at high frame rates.
// Details: docs/fps60/rows/FX-012.md
#include "../fps60.h"
#include <intrin.h>
#include <stdio.h>

typedef void*(__cdecl* alloc_t)(void* pool, int size);
typedef void(__cdecl* free_t)(void* pool, void* p);

static uint8_t* g_alloc_fn = nullptr;   // FUN_0072c3d0
static uint8_t* g_free_fn = nullptr;    // FUN_0072c510
static alloc_t  g_alloc_tramp = nullptr;
static free_t   g_free_tramp = nullptr;
static bool     g_patched = false;
static volatile LONG g_guarded = 0;

enum { SITES = 8 };
struct Site { void* ra; uint32_t n[4]; };
static Site     g_asite[SITES], g_fsite[SITES];
static uint32_t g_fail[4];
static long     g_net = 0, g_net_max = 0;        // live blocks since install (allocs - frees)

static void count(Site* t, void* ra, int k) {
    for (int i = 0; i < SITES; i++) {
        if (t[i].ra == ra) { t[i].n[k]++; return; }
        if (!t[i].ra) { t[i].ra = ra; t[i].n[k]++; return; }
    }
}

// cdecl(pool, size) -> block or 0, plain RET (0x72C459 / 0x72C46E).
static void* __cdecl h_alloc(void* pool, int size) {
    void* p = g_alloc_tramp(pool, size);
    int k = pacing_kind() & 3;
    count(g_asite, _ReturnAddress(), k);
    if (p) { if (++g_net > g_net_max) g_net_max = g_net; }
    else if (g_fail[k]++ == 0)
        log_line("FX-012: PPP pool allocation failed (size %d, caller gva=0x%08X, inv kind %d, %ld blocks live)", size,
                 rt_to_gva_exe(_ReturnAddress()), k, g_net);
    return p;
}

// cdecl(pool, block), plain RET (0x72C556).
static void __cdecl h_free(void* pool, void* p) {
    count(g_fsite, _ReturnAddress(), pacing_kind() & 3);
    g_net--;
    g_free_tramp(pool, p);
}

bool fx012_failed(void* prog) {
    uint32_t mark = 0;
    if (!row_FX012.on || g_disabled || !safe_read_u32(prog, &mark) || mark != 0xFFFFF000u) return false;
    if (InterlockedIncrement(&g_guarded) == 1)
        log_line("FX-012: step of a program whose init failed (pool full) answered 'ended' (rec %p, inv kind %d)", prog,
                 pacing_kind());
    return true;
}

static bool install() {
    // FUN_0072c3d0: PUSH EBP; MOV EBP,ESP; MOV EDX,[EBP+8]; PUSH ESI; MOV ESI,[EBP+0xC]; MOV ECX,[EDX]; size round to 16
    g_alloc_fn = sig_find_unique("FX-012 PPP pool alloc FUN_0072c3d0",
        "55 8B EC 8B 55 08 56 8B 75 0C 8B 0A 83 C6 0F 83 E6 F0 83 C6 10 85 C9", 0x72C3D0);
    // FUN_0072c510: MOV EDX,[EBP+0xC]; MOV ECX,[EBP+8]; ADD EDX,-0x10; MOV EAX,EDX; MOV [EDX+8],0; MOV [ECX],EDX
    g_free_fn = sig_find_unique("FX-012 PPP pool free FUN_0072c510",
        "55 8B EC 8B 55 0C 8B 4D 08 83 C2 F0 8B C2 C7 42 08 00 00 00 00 89 11 2B 02 01 41 0C", 0x72C510);
    if (!g_alloc_fn || !g_free_fn) return false;
    log_line("  FX-012: guard via MAG-006's Ppp_ProgramStep detour%s; pool alloc gva=0x%08X, free gva=0x%08X",
             row_MAG006.on ? "" : " (MAG-006 off: no guard)", rt_to_gva_exe(g_alloc_fn), rt_to_gva_exe(g_free_fn));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("FX-012 alloc", g_alloc_fn, (void*)&h_alloc, (void**)&g_alloc_tramp) ||
        !hook_entry("FX-012 free", g_free_fn, (void*)&h_free, (void**)&g_free_tramp))
        log_line("FX-012: pool hooks failed - counts off (the guard still works)");
}

static void line(const char* what, Site* t) {
    char b[600];
    int m = 0;
    for (int i = 0; i < SITES && t[i].ra; i++) {
        uint32_t* n = t[i].n;
        if (!(n[0] | n[1] | n[2] | n[3])) continue;
        m += _snprintf_s(b + m, sizeof(b) - m, _TRUNCATE, " 0x%06X:%u/%u/%u/%u", rt_to_gva_exe(t[i].ra), n[0], n[1], n[2], n[3]);
        memset(n, 0, sizeof(t[i].n));
    }
    if (m) log_line("FX-012: %s per caller (logic/render-only/backlog/stock):%s", what, b);
}

static void post() {
    static uint32_t tick = 0;
    if (++tick % (300 * (uint32_t)rate_mult()) != 0) return;
    line("PPP pool allocs", g_asite);
    line("PPP pool frees ", g_fsite);
    if (g_fail[0] | g_fail[1] | g_fail[2] | g_fail[3]) {
        log_line("FX-012: PPP pool allocation failures (logic/render-only/backlog/stock): %u/%u/%u/%u", g_fail[0], g_fail[1],
                 g_fail[2], g_fail[3]);
        memset(g_fail, 0, sizeof(g_fail));
    }
    log_line("FX-012: PPP pool blocks live %ld (max %ld); failed-init steps guarded %ld", g_net, g_net_max, (long)g_guarded);
    g_net_max = g_net;
}

Row row_FX012 = {
    "FX-012",
    "PPP pool full: a program whose init failed is ended instead of stepped (crash 0x716C8E); pool use logged",
    1, install, pre, post, 0
};
