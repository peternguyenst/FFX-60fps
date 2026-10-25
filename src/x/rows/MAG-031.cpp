// SPDX-License-Identifier: Apache-2.0
// MAG-031: Penance's Obliteration (magic_0705.dll) times itself with a counter that its draw FUN_100020c0 increments
// (INC [0x1038afb4] at +0x254, unless *[0x1008a850] != 0), while its update FUN_100017e0 lights one of the 12 body
// light nodes only when the counter is exactly 15 + 7k (< 103). Draws run on every invocation, updates only on logic
// ticks, so at N passes the counter jumped by N per tick and most exact values were never seen (U 25/10/2026: only
// two lights at the bottom of the body lit). The fade (counter / 60) ran N x fast too. On render-only invocations the
// counter is put back after the draw: it steps once per logic tick, as in stock. Found by pattern in any magic DLL.
// Details: docs/fps60/rows/MAG-031.md
#include "../fps60.h"
#include <string.h>

typedef void(__cdecl* draw_t)();

// FUN_100020c0: prologue, SUB ESP,0x54, cookie; CMP [active],0; JZ end; CMP [counter],0; JL end
static const char DRAW[] =
    "55 8B EC 83 EC 54 A1 ?? ?? ?? ?? 33 C5 89 45 FC 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 8C";

enum { MAX_PENDING = 8 };
static uint8_t* volatile g_pending[MAX_PENDING];   // loaded, not yet scanned (set under the loader lock)
static uint32_t g_pending_size[MAX_PENDING];
static char     g_pending_name[MAX_PENDING][24];
static uint8_t* g_mod = nullptr;                   // the hooked module (one at a time)
static char     g_name[24];
static uint8_t* g_entry = nullptr;
static int32_t* g_counter = nullptr;
static draw_t   g_tramp = nullptr;
static volatile LONG g_held = 0;

// void(void), plain RET (+0x269).
static void __cdecl h_draw() {
    if (!row_MAG031.on || g_disabled || pacing_kind() != INV_BUILD) { g_tramp(); return; }
    int32_t c = *g_counter;
    g_tramp();
    *g_counter = c;
    InterlockedIncrement(&g_held);
}

static uint32_t gva(uint8_t* mod, uint8_t* p) { return 0x10000000u + (uint32_t)(p - mod); }

static void try_patch(uint8_t* mod, uint32_t size, const char* name) {
    int hits = 0;
    uint8_t* d = sig_scan_module(mod, size, DRAW, &hits);
    if (!d || hits != 1) return;                   // not this effect
    uint8_t* counter = read_abs(d + 0x1F);
    // tail: CMP dword [EAX],0; POP EDI; POP ESI; JNZ +6; INC dword [counter] (+0x254)
    if (d[0x254] != 0xFF || d[0x255] != 0x05 || read_abs(d + 0x256) != counter || counter < mod || counter >= mod + size) {
        log_line("MAG-031: %s draw at gva 0x%08X: counter INC not at +0x254 - not hooked", name, gva(mod, d));
        return;
    }
    if (g_mod) { log_line("MAG-031: %s matches but %s is still hooked - left alone", name, g_name); return; }
    if (!hook_entry("MAG-031 draw", d, (void*)&h_draw, (void**)&g_tramp)) return;
    g_mod = mod;
    g_entry = d;
    g_counter = (int32_t*)counter;
    g_held = 0;
    memcpy(g_name, name, sizeof(g_name));
    log_line("MAG-031: %s draw hooked (gva 0x%08X, counter gva 0x%08X): counter held on render-only invocations",
             name, gva(mod, d), gva(mod, counter));
}

// ---------------------------------------------------------------- module tracking (magic_mods.cpp, loader lock)
static void on_load(uint8_t* base, uint32_t size, const char* name) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g_pending[i]) continue;
        g_pending_size[i] = size;
        memset(g_pending_name[i], 0, sizeof(g_pending_name[i]));
        strncpy(g_pending_name[i], name, sizeof(g_pending_name[i]) - 1);
        g_pending[i] = base;                     // publish last
        return;
    }
}

static void on_unload(uint8_t* base) {
    for (int i = 0; i < MAX_PENDING; i++) if (g_pending[i] == base) g_pending[i] = nullptr;
    if (base != g_mod) return;
    log_line("MAG-031: %s unloaded (render-only draws with the counter held: %ld)", g_name, g_held);
    hook_remove("MAG-031", g_entry);
    g_mod = nullptr;
    g_entry = nullptr;
    g_counter = nullptr;
}

static bool install() {
    if (!magic_mods_subscribe("MAG-031", on_load, on_unload)) return false;
    log_line("  MAG-031: watching magic DLL loads for the Obliteration draw counter (magic_0705 on disk)");
    return true;
}

static void pre() {
    for (int i = 0; i < MAX_PENDING; i++) {
        uint8_t* m = g_pending[i];
        if (!m) continue;
        char name[24];
        memcpy(name, g_pending_name[i], sizeof(name));
        g_pending[i] = nullptr;
        __try { try_patch(m, g_pending_size[i], name); }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_line("MAG-031: fault 0x%08lX while hooking %s - left unhooked", GetExceptionCode(), name); }
    }
}

Row row_MAG031 = {
    "MAG-031",
    "Obliteration (magic_0705) draw counter held on render-only invocations (body light nodes and fade at stock timing)",
    1, install, pre, nullptr, 0
};
