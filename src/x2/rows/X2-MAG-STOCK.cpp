// SPDX-License-Identifier: Apache-2.0
// X2-MAG-STOCK: fallback for a spell not fixed yet: stock 30 Hz pacing while one of the listed spell DLLs is loaded.
//
// The effect then plays exactly as in the stock game; 60 Hz comes back when the DLL unloads (as X2-W2A-002 mode 1 does
// for movies). Every spell it was used for has since been fixed at 60 Hz (Full Throttle, Ultima), so the list is empty.
// ini [rows]: X2-MAG-STOCK=1, X2-MAG-STOCK-LIST=<spell DLL numbers, any separator> (default empty).
// History: re/ledger/CRASH-0681.md, ARENA-VS.md.
#include "../x2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace x2 {

enum { MAX_LIST = 32, MAX_LOADED = 8 };
static int      g_list[MAX_LIST];
static int      g_nlist = 0;
static uint8_t* g_loaded[MAX_LOADED];
static volatile LONG g_count = 0;       // listed DLLs loaded now (loader thread writes, game thread reads)
static bool     g_held = false;          // game thread
extern Row row_MAGSTOCK;

static int magic_number(const char* name) {        // "magic_0681.dll" / "magic_0681_us.dll" -> 681
    const char* p = strstr(name, "magic_");
    return p ? atoi(p + 6) : -1;
}

static void on_load(uint8_t* base, uint32_t, const char* name) {
    int n = magic_number(name);
    for (int i = 0; i < g_nlist; i++) {
        if (g_list[i] != n) continue;
        for (int k = 0; k < MAX_LOADED; k++)
            if (!g_loaded[k]) { g_loaded[k] = base; InterlockedIncrement(&g_count); break; }
        log_line("X2-MAG-STOCK: %s loaded - stock 30 Hz pacing while it is loaded", name);
        return;
    }
}

static void on_unload(uint8_t* base) {
    for (int k = 0; k < MAX_LOADED; k++)
        if (g_loaded[k] == base) { g_loaded[k] = nullptr; InterlockedDecrement(&g_count); }
}

static void pre() {
    bool want = g_count > 0;
    if (want == g_held) return;
    g_held = want;
    pacing_hold_stock("X2-MAG-STOCK", want);
}

static bool install() {
    char buf[256];
    config_str("rows", "X2-MAG-STOCK-LIST", buf, sizeof(buf), "");
    for (char* t = buf; *t && g_nlist < MAX_LIST;) {
        char* e = nullptr;
        long v = strtol(t, &e, 10);
        if (e == t) { t++; continue; }
        g_list[g_nlist++] = (int)v;
        t = e;
    }
    if (!g_nlist) { log_line("  X2-MAG-STOCK: empty list - nothing to hold"); return true; }
    if (!magic_mods_subscribe("X2-MAG-STOCK", on_load, on_unload)) return false;
    char line[256]; int k = 0; line[0] = 0;
    for (int i = 0; i < g_nlist; i++) k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %04d", g_list[i]);
    log_line("  X2-MAG-STOCK: stock pacing while loaded:%s", line);
    return true;
}

Row row_MAGSTOCK = {
    "X2-MAG-STOCK",
    "fallback: stock 30 Hz pacing while a listed magic DLL is loaded (X2-MAG-STOCK-LIST, default none)",
    1, install, pre, nullptr, 0
};

} // namespace x2
