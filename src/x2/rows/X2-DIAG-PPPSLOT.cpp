// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-PPPSLOT (diagnostic, default 0; CRASH-0946, user 24/10/2026 "fix the cause"): balance of a magic DLL's
// per-unit particle programs. A walker-template DLL hands the exe per-unit callbacks in its overlay table (S, magic_0946
// table 0x10004028): +0x44 start (attach op 0x6BA160 -> CALL 0x6BA283; 0946 FUN_10001330 takes a slot of its 64-entry
// table), +0x48 step (CALL 0x6BB0B0; 0946 FUN_10001480 frees the slot when Ppp_ProgramStep reports done, returns
// non-zero), +0x54 end (op FUN_006ba5a0, CALL 0x6BA5B5; 0946 FUN_10001620 frees it). With X2-MAG-035's late pass 0946
// filled all 64 slots and crashed (CRASH-0946.md). This row hooks those three callbacks of the DLL named in
// X2-DIAG-PPPSLOT-DLL (default 946) and logs every 0.25 s: calls per invocation kind (logic / render-only) and walk
// (X2-MAG-025: pass, held mode), step "done" returns, and the units holding a program (started, not yet done/ended).
// Observe only: every hook calls the original and returns its value.
#include "../x2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace x2 {

extern Row row_DIAGPPPSLOT;

typedef int(__cdecl* cb_t)(uint8_t* unit);
typedef void** (__cdecl* table_t)(int);

static int g_dll = 946;
static uint8_t* g_mod = nullptr;
static uint8_t* g_fn[3] = {};                      // start, step, end
static cb_t g_orig[3] = {};
static const int OFF[3] = {0x44, 0x48, 0x54};
static const char* const NAME[3] = {"start", "step", "end"};

// counters [callback][kind 0 logic / 1 render-only][walk: 0 none, 1 pass 0, 2 pass 1, 3 held]
static volatile LONG g_n[3][2][4], g_done[2][4];
enum { MAXU = 256 };
static uint8_t* g_unit[MAXU];
static uint32_t g_unit_inv[MAXU];
static volatile LONG g_inv = 0, g_restart = 0, g_lost = 0;
static ULONGLONG g_next = 0;

static int walk_slot() {
    int w = mag025_walk_info();
    if (w & 0xFF) return 3;
    int p = w >> 8;
    return p == 1 ? 1 : p == 2 ? 2 : 0;
}

static int find(uint8_t* u) {
    for (int i = 0; i < MAXU; i++) if (g_unit[i] == u) return i;
    return -1;
}

static void live_add(uint8_t* u) {
    if (find(u) >= 0) { InterlockedIncrement(&g_restart); return; }
    for (int i = 0; i < MAXU; i++)
        if (!g_unit[i]) { g_unit[i] = u; g_unit_inv[i] = (uint32_t)g_inv; return; }
    InterlockedIncrement(&g_lost);
}

static void live_del(uint8_t* u) {
    int i = find(u);
    if (i >= 0) g_unit[i] = nullptr;
}

static int __cdecl h_cb(int k, uint8_t* unit) {
    int kind = render_only() ? 1 : 0, w = walk_slot();
    InterlockedIncrement(&g_n[k][kind][w]);
    int r = g_orig[k](unit);
    if (k == 0) live_add(unit);
    else if (k == 1 && r) { InterlockedIncrement(&g_done[kind][w]); live_del(unit); }
    else if (k == 2) live_del(unit);
    return r;
}
static int __cdecl h_start(uint8_t* u) { return h_cb(0, u); }
static int __cdecl h_step(uint8_t* u) { return h_cb(1, u); }
static int __cdecl h_end(uint8_t* u) { return h_cb(2, u); }

static void report(const char* why) {
    char line[900];
    int k = _snprintf_s(line, sizeof(line), _TRUNCATE, "X2-DIAG-PPPSLOT: magic_%04d %s inv %ld", g_dll, why, g_inv);
    static const char* const W[4] = {"-", "p0", "p1", "held"};
    for (int c = 0; c < 3; c++)
        for (int kind = 0; kind < 2; kind++)
            for (int w = 0; w < 4; w++)
                if (g_n[c][kind][w])
                    k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " | %s %s/%s %ld", NAME[c], kind ? "R" : "L", W[w],
                                     g_n[c][kind][w]);
    for (int kind = 0; kind < 2; kind++)
        for (int w = 0; w < 4; w++)
            if (g_done[kind][w])
                k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " | done %s/%s %ld", kind ? "R" : "L", W[w], g_done[kind][w]);
    int live = 0;
    uint32_t oldest = 0;
    for (int i = 0; i < MAXU; i++)
        if (g_unit[i]) { live++; if (!oldest || g_unit_inv[i] < oldest) oldest = g_unit_inv[i]; }
    _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " || live %d (oldest from inv %u), restarts %ld, passed %d", live,
                oldest, g_restart, mag008_list_active() ? 1 : 0);
    log_line("%s", line);
}

static void on_load(uint8_t* mod, uint32_t, const char* name) {
    if (!row_DIAGPPPSLOT.on || g_disabled || g_mod) return;
    const char* p = strstr(name, "magic_");
    if (!p || atoi(p + 6) != g_dll) return;
    void** tbl = nullptr;
    __try {
        table_t get = (table_t)GetProcAddress((HMODULE)mod, "GetEffectOverlayTable");
        tbl = get ? get(0) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { tbl = nullptr; }
    if (!tbl) { log_line("X2-DIAG-PPPSLOT: %s has no overlay table", name); return; }
    memset((void*)g_n, 0, sizeof(g_n));
    memset((void*)g_done, 0, sizeof(g_done));
    memset(g_unit, 0, sizeof(g_unit));
    g_inv = g_restart = g_lost = 0;
    g_mod = mod;
    static void* const H[3] = {(void*)&h_start, (void*)&h_step, (void*)&h_end};
    for (int c = 0; c < 3; c++) {
        g_fn[c] = (uint8_t*)tbl[OFF[c] / 4];
        if (!g_fn[c] || !hook_entry(NAME[c], g_fn[c], H[c], (void**)&g_orig[c])) g_fn[c] = nullptr;
    }
    log_line("X2-DIAG-PPPSLOT: %s table %p: start %p %s, step %p %s, end %p %s", name, tbl, g_fn[0], g_fn[0] ? "hooked" : "-",
             g_fn[1], g_fn[1] ? "hooked" : "-", g_fn[2], g_fn[2] ? "hooked" : "-");
}

static void on_unload(uint8_t* mod) {
    if (!g_mod || mod != g_mod) return;
    report("unloaded");
    for (int c = 0; c < 3; c++) if (g_fn[c]) hook_remove(g_fn[c]);
    memset(g_fn, 0, sizeof(g_fn));
    g_mod = nullptr;
}

static void post() {
    if (!g_mod) return;
    InterlockedIncrement(&g_inv);
    ULONGLONG now = GetTickCount64();
    if (now < g_next) return;
    g_next = now + 250;
    report("running");
}

static bool install() {
    g_dll = config_row("X2-DIAG-PPPSLOT-DLL", 946);
    if (!magic_mods_subscribe("X2-DIAG-PPPSLOT", on_load, on_unload)) return false;
    log_line("  X2-DIAG-PPPSLOT: watching magic_%04d start / step / end callbacks", g_dll);
    return true;
}

Row row_DIAGPPPSLOT = {
    "X2-DIAG-PPPSLOT",
    "diagnostic: per-unit particle program start / step / end balance of one magic DLL (CRASH-0946)",
    0, install, nullptr, post, 0
};

} // namespace x2
