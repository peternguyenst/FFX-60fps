// SPDX-License-Identifier: Apache-2.0
// MAG-018: family-E magic DLLs cache "paused" (exec==0) in their draw and their update reads it. With MAG-008
// the update runs right after a render-only draw and never advances (Ixion soft-lock). Each logic invocation
// resets a stale cache and pause flag to what a stock draw would leave.
// Details: docs/fps60/rows/MAG-018.md
#include "../fps60.h"
#include <string.h>

enum { MAX_MODS = 8 };
struct FamE {
    uint8_t*  mod;
    uint32_t  size;
    bool      scanned;
    volatile int32_t*  cache;       // DLL global (0x100FB684 in 0597)
    int32_t* volatile* ptr_var;     // DLL global holding the pause-flag pointer (0x100FF218 in 0597)
    char      name[24];
};
static FamE g_mods[MAX_MODS];
static volatile LONG g_resets = 0, g_pause_resets = 0;

// Exec-pause setter: call slot +0x3F4 (exec count); 0 -> *ptr = 1, cache = 1; else both 0
static const char PAT[] =
    "8B 80 F4 03 00 00 FF D0 85 C0 A1 ?? ?? ?? ?? 75 11 "
    "C7 00 01 00 00 00 C7 05 ?? ?? ?? ?? 01 00 00 00 C3 "
    "C7 00 00 00 00 00 C7 05 ?? ?? ?? ?? 00 00 00 00 C3";

// MAG-018-DIRECT: the uncached variant (6 DLLs: 0198, 0341, 0481, 0483, 0499, 0519): the draw writes (exec==0)
// straight into the pause flag and the update returns while it is 1 (magic_0499: draw 0x100018D0, update test
// 0x10001508 CMP [ECX],0; JNE; Luca Yuna-rescue battle soft-lock, U 16/10/2026). MOV ECX,[ptr var] at +10.
static const char PAT_DIRECT_A[] =
    "8B 80 F4 03 00 00 FF D0 8B 0D ?? ?? ?? ?? FF 35 ?? ?? ?? ?? F7 D8 1B C0 40 89 01";
static const char PAT_DIRECT_B[] =
    "8B 80 F4 03 00 00 FF D0 8B 0D ?? ?? ?? ?? F7 D8 1B C0 40 89 01";
static bool g_direct = true;

// MAG-018-UNLOAD: a family-E DLL whose last draw was render-only unloads with the pause flag at 1, and no draw is
// left to clear it: every particle program stays frozen for the rest of the battle (Thundaga/Death, then Life on
// Yuna: magic_0114's records never finish, no end signal 0x0206, soft-lock; D 20/10/2026 DAT_0230fd34 == 1 live).
// The next logic invocation writes 0, what the DLL's stock last draw would have left.
static bool g_unload_fix = true;
static int32_t* volatile g_stale_pf = nullptr;   // pause flag left at 1 by an unloaded DLL
static char g_stale_name[24];
static volatile LONG g_unload_resets = 0;

// Shared with MAG-021: number of exec-pause setters in a loaded module's code (1 = family E with this
// shape); *hit = the first one.
int famE_find_setter(uint8_t* mod, uint32_t size, uint8_t** hit_out) {
    int hits = 0;
    uint8_t* hit = sig_scan_module(mod, size, PAT, &hits);
    if (hit_out) *hit_out = hit;
    return hits;
}

static void scan(FamE& m) {
    m.scanned = true;
    uint8_t* hit = nullptr;
    int hits = famE_find_setter(m.mod, m.size, &hit);
    if (hits == 0 && g_direct) {
        int ha = 0, hb = 0;
        uint8_t* da = sig_scan_module(m.mod, m.size, PAT_DIRECT_A, &ha);
        uint8_t* db = sig_scan_module(m.mod, m.size, PAT_DIRECT_B, &hb);
        uint8_t* d = ha == 1 && hb == 0 ? da : ha == 0 && hb == 1 ? db : nullptr;
        if (d) {
            uint32_t pv = *(uint32_t*)(d + 10), lo = (uint32_t)(uintptr_t)m.mod, hi = lo + m.size;
            if (pv >= lo && pv < hi) {
                m.cache = nullptr;
                m.ptr_var = (int32_t* volatile*)(uintptr_t)pv;
                log_line("MAG-018-DIRECT: %s uncached exec-pause setter at gva 0x%08X: pause-pointer var gva 0x%08X",
                         m.name, 0x10000000u + (uint32_t)(d - m.mod), 0x10000000u + (pv - lo));
                return;
            }
        }
        if (ha + hb) log_line("MAG-018-DIRECT: %s has %d/%d uncached pause setters - not handled", m.name, ha, hb);
    }
    if (hits != 1) {
        if (hits > 1) log_line("MAG-018: %s has %d pause setters - not handled", m.name, hits);
        m.mod = nullptr;                          // not family E (or ambiguous): forget it
        return;
    }
    uint32_t c1 = *(uint32_t*)(hit + 25), c2 = *(uint32_t*)(hit + 42), pv = *(uint32_t*)(hit + 11);
    uint32_t lo = (uint32_t)(uintptr_t)m.mod, hi = lo + m.size;
    if (c1 != c2 || c1 < lo || c1 >= hi || pv < lo || pv >= hi) {
        log_line("MAG-018: %s pause setter operands out of range - not handled", m.name);
        m.mod = nullptr;
        return;
    }
    m.cache = (volatile int32_t*)(uintptr_t)c1;
    m.ptr_var = (int32_t* volatile*)(uintptr_t)pv;
    log_line("MAG-018: %s exec-pause setter at gva 0x%08X: cache gva 0x%08X, pause-pointer var gva 0x%08X",
             m.name, 0x10000000u + (uint32_t)(hit - m.mod), 0x10000000u + (c1 - lo), 0x10000000u + (pv - lo));
}

// magic_mods.cpp, loader lock
static void on_load(uint8_t* base, uint32_t size, const char* name) {
    for (int i = 0; i < MAX_MODS; i++) {
        if (g_mods[i].mod) continue;
        g_mods[i].size = size;
        g_mods[i].scanned = false;
        g_mods[i].cache = nullptr;
        g_mods[i].ptr_var = nullptr;
        memset(g_mods[i].name, 0, sizeof(g_mods[i].name));
        strncpy(g_mods[i].name, name, sizeof(g_mods[i].name) - 1);
        g_mods[i].mod = base;                 // publish last
        return;
    }
}

// magic_mods.cpp, loader lock; the image is still mapped.
static void on_unload(uint8_t* base) {
    for (int i = 0; i < MAX_MODS; i++) {
        FamE& m = g_mods[i];
        if (m.mod != base) continue;
        if (g_unload_fix && m.scanned && m.ptr_var) {
            int32_t* pf = *m.ptr_var;
            if (pf && *pf == 1) { memcpy(g_stale_name, m.name, sizeof(g_stale_name)); g_stale_pf = pf; }
        }
        m.mod = nullptr;
    }
}

static bool install() {
    g_direct = config_row("MAG-018-DIRECT", 1) != 0;
    g_unload_fix = config_row("MAG-018-UNLOAD", 1) != 0;
    if (!magic_mods_subscribe("MAG-018", on_load, on_unload)) return false;
    log_line("  MAG-018: watching magic DLL loads for the family-E exec-pause setter (MAG-018-UNLOAD %d)", g_unload_fix);
    return true;
}

static void pre() {
    bool logic = pacing_kind() != INV_BUILD;
    if (g_stale_pf && logic && !g_disabled) {
        int32_t* pf = g_stale_pf;
        g_stale_pf = nullptr;
        __try {
            if (*pf == 1) {
                *pf = 0;
                LONG n = InterlockedIncrement(&g_unload_resets);
                if (n <= 5 || n % 50 == 0)
                    log_line("MAG-018-UNLOAD: %s unloaded with the pause flag at 1 - reset at a logic invocation (%ld so far)",
                             g_stale_name, n);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            log_line("MAG-018-UNLOAD: fault 0x%08lX on the pause flag", GetExceptionCode());
        }
    }
    for (int i = 0; i < MAX_MODS; i++) {
        FamE& m = g_mods[i];
        if (!m.mod) continue;
        __try {
            if (!m.scanned) scan(m);
            if (!m.mod || !m.ptr_var || !logic || g_disabled) continue;
            if (!m.cache) {                       // MAG-018-DIRECT: no cache, the update reads the flag itself
                int32_t* pf = *m.ptr_var;
                if (pf && *pf == 1) { *pf = 0; InterlockedIncrement(&g_resets); InterlockedIncrement(&g_pause_resets); }
                continue;
            }
            if (*m.cache == 1) {
                *m.cache = 0;
                InterlockedIncrement(&g_resets);
                int32_t* pf = *m.ptr_var;
                if (pf && *pf == 1) { *pf = 0; InterlockedIncrement(&g_pause_resets); }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            log_line("MAG-018: fault 0x%08lX on %s - module dropped", GetExceptionCode(), m.name);
            m.mod = nullptr;
        }
    }
    static LONG reported = 0;
    if (!reported && g_resets > 0) {
        reported = 1;
        log_line("MAG-018: first reset of a stale exec-pause cache at a logic invocation (pause flag reset too: %ld)", g_pause_resets);
    }
}

Row row_MAG018 = {
    "MAG-018",
    "family-E magic DLLs: stale (exec==0) pause cache and pause flag reset at logic invocations",
    1, install, pre, nullptr, 0
};
