// SPDX-License-Identifier: Apache-2.0
// X2-MAG-008: battle effects update once per game tick; "passing" the effects that need otherwise.
//
// The problem. On every invocation that skips logic (exec == 0, i.e. our render-only frames), the game's main loop
// still calls the effect dispatch FUN_00616660(0) (CALL at gva 0x60582D). It updates every queued effect and spell
// overlay, so at 60 fps effects ran at double speed. The fix: that CALL goes to hook_render_only_dispatch, which
// skips the dispatch on render-only invocations.
//
// "Passing" (X2-MAG-008-PASS, on). Some spell DLLs draw parts of their effect only right after their update, which
// this dispatch runs; with the dispatch skipped they were drawn every second frame (a blink). While such a DLL is
// loaded, the dispatch runs on render-only invocations too: we say the DLL is "passed". (Its update still steps only
// on logic frames: the DLL itself checks the exec flag.) A DLL is passed when:
//   - it belongs to one of two families, found by a string in the DLL: the 63 "cross fade" DLLs (dressphere change and
//     others, "kohno corss fade begin") and 62 "ky" overlays that load their own character model;
//   - its number is in X2-MAG-008-PASS-LIST (default 885 937 906 212 330 386 283: the Rikku -> Warrior change, the
//     arena "VS." and "LOSER..." banners, Requiem, Big Bang);
//   - it loads within 500 ms after a passed DLL unloads (a "chain": effects that run several DLLs back to back),
//     unless the DLL that unloaded is in X2-MAG-008-PASS-NOCHAIN (default 386 283: a chain once passed magic_0449,
//     which then crashed);
//   - X2-MAG-035 sees it blink (mag008_pass_late).
// While a listed DLL (or one chained from it) is loaded, mag008_list_active() is true and X2-MAG-025 uses its "list
// profile". X2-MAG-008-PASS=2 is a test mode that also passes the 310 template DLLs (brings back double-speed effects).
// History, measurements and evidence: re/ledger/PORT-A.md §1, re/MAINLOOP.md §9.4, XFADE-BLINK.md (it also keeps this
// file's earlier header), ARENA-VS.md.
#include "../x2.h"
#include <string.h>
#include <stdlib.h>

namespace x2 {

typedef void(__cdecl* effect_dispatch_fn)(int);         // cdecl int, plain RET 0x61675D (PORT-A §1)

static effect_dispatch_fn g_effect_dispatch = nullptr;
static volatile LONG g_dispatch_skipped = 0, g_dispatch_ran = 0, g_dispatch_ran_render_only = 0;
static int  g_opt_pass = 1;                       // [rows] X2-MAG-008-PASS (0 off, 1 on, 2 test: + templates)
static volatile LONG g_passed_dlls_loaded = 0;    // passed DLLs loaded now (> 0: the dispatch runs on render-only frames)
extern Row row_MAG008;

// --- Which DLLs are passed ------------------------------------------------------------------------------------------------
// True if the loaded DLL image contains the string k (the family markers in is_passed_family).
static bool image_contains(const uint8_t* base, uint32_t size, const char* k) {
    const uint32_t n = (uint32_t)strlen(k);
    __try {
        for (uint32_t i = 0; i + n <= size; i++)
            if (base[i] == (uint8_t)k[0] && memcmp(base + i, k, n) == 0) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
// The first bytes of a function every template DLL has (X2-MAG-008-PASS=2 test mode only: most battle effects are
// template DLLs, so passing them all brings back double-speed effects).
static const uint8_t TEMPLATE_PROLOGUE[] = {0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08, 0x56, 0x8B, 0x8B, 0xD0, 0x00, 0x00, 0x00, 0x57,
    0x8B, 0x41, 0x14, 0xA3};
// The two families: cross-fade DLLs ("kohno corss fade begin", the original's typo) and "ky" model overlays.
static bool is_passed_family(const uint8_t* base, uint32_t size) {
    if (image_contains(base, size, "kohno corss fade begin") || image_contains(base, size, "ky: *** Chr Packet Size")) return true;
    if (g_opt_pass != 2) return false;
    __try {
        for (uint32_t i = 0; i + sizeof(TEMPLATE_PROLOGUE) <= size; i++)
            if (base[i] == 0x55 && memcmp(base + i, TEMPLATE_PROLOGUE, sizeof(TEMPLATE_PROLOGUE)) == 0) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
// Passed DLLs loaded now (up to 8): base address, whether its unload opens a chain (family or listed DLLs not in
// NOCHAIN; a DLL passed through a chain does not start another one), and whether it counts as "listed".
static uint8_t* g_passed_base[8];
static bool     g_passed_starts_chain[8];
static bool     g_passed_listed[8];                     // listed, or chained from a listed DLL
static volatile LONG g_listed_dlls_loaded = 0;
static bool     g_chain_from_listed = false;            // the open chain was started by a listed DLL
bool mag008_list_active() { return g_listed_dlls_loaded > 0; }
// The chain: a DLL that loads before this time (500 ms after a chain-starting DLL unloaded) is passed too.
static volatile DWORD g_chain_open_until = 0;
static int g_pass_list[32], g_pass_list_count = 0, g_nochain_list[32], g_nochain_list_count = 0;
static int dll_number(const char* name) {
    const char* p = strstr(name, "magic_");
    return p ? atoi(p + 6) : -1;
}
static bool in_list(const int* l, int cnt, int n) {
    for (int i = 0; i < cnt; i++) if (l[i] == n) return true;
    return false;
}
static bool in_pass_list(const char* name) { return in_list(g_pass_list, g_pass_list_count, dll_number(name)); }
static void parse_list(const char* key, const char* def, int* l, int* cnt) {
    char buf[160];
    config_str("rows", key, buf, sizeof(buf), def);
    for (char* q = buf; *q && *cnt < 32;) {
        char* e = nullptr; long v = strtol(q, &e, 10);
        if (e == q) { q++; continue; }
        l[(*cnt)++] = (int)v; q = e;
    }
}
// A spell DLL loaded: pass it if it is listed, of a passed family, or arrives while a chain is open.
static void on_load(uint8_t* base, uint32_t size, const char* name) {
    bool chained = g_chain_open_until && (LONG)(GetTickCount() - g_chain_open_until) < 0;
    bool listed = in_pass_list(name);
    bool own = listed || is_passed_family(base, size);
    if (listed) log_line("X2-MAG-008-PASS: %s is in X2-MAG-008-PASS-LIST", name);
    if (!own) {
        if (!chained) return;
        log_line("X2-MAG-008-PASS: %s loaded right after a passed DLL - passed too (chain)", name);
    }
    bool prof = listed || (!own && chained && g_chain_from_listed);
    for (int i = 0; i < 8; i++)
        if (!g_passed_base[i]) {
            g_passed_base[i] = base; g_passed_listed[i] = prof;
            g_passed_starts_chain[i] = own && !(listed && in_list(g_nochain_list, g_nochain_list_count, dll_number(name)));
            InterlockedIncrement(&g_passed_dlls_loaded);
            if (prof) InterlockedIncrement(&g_listed_dlls_loaded);
            break;
        }
    log_line("X2-MAG-008-PASS: %s is a cross-fade / ky model DLL - exec==0 dispatch runs on render-only invocations while loaded", name);
}
// For X2-MAG-035: pass a DLL that is already loaded, as a listed DLL that starts no chain (game thread). Returns false
// if passing is off, the DLL is already passed, or all 8 slots are taken.
bool mag008_pass_late(uint8_t* base, const char* name) {
    if (!row_MAG008.on || !g_opt_pass || !base) return false;
    for (int i = 0; i < 8; i++) if (g_passed_base[i] == base) return false;
    for (int i = 0; i < 8; i++)
        if (InterlockedCompareExchangePointer((void* volatile*)&g_passed_base[i], base, nullptr) == nullptr) {
            g_passed_starts_chain[i] = false; g_passed_listed[i] = true;
            InterlockedIncrement(&g_passed_dlls_loaded);
            InterlockedIncrement(&g_listed_dlls_loaded);
            log_line("X2-MAG-008-PASS: %s passed late (X2-MAG-035), no chain", name);
            return true;
        }
    return false;
}
// A spell DLL unloaded: if it was passed, forget it and (if it starts chains) open the 500 ms chain window.
static void on_unload(uint8_t* base) {
    for (int i = 0; i < 8; i++)
        if (g_passed_base[i] == base) {
            g_passed_base[i] = nullptr;
            InterlockedDecrement(&g_passed_dlls_loaded);
            if (g_passed_listed[i]) InterlockedDecrement(&g_listed_dlls_loaded);
            g_chain_open_until = g_passed_starts_chain[i] ? GetTickCount() + 500 : 0;
            g_chain_from_listed = g_passed_starts_chain[i] && g_passed_listed[i];
        }
}

// --- The hook --------------------------------------------------------------------------------------------------------------
// Replaces the main loop's exec == 0 call of the effect dispatch: on render-only invocations it runs only while a
// passed DLL is loaded.
static void __cdecl hook_render_only_dispatch(int pass) {
    if (row_MAG008.on && !g_disabled && render_only()) {
        if (!(g_opt_pass && g_passed_dlls_loaded > 0)) { InterlockedIncrement(&g_dispatch_skipped); return; }
        InterlockedIncrement(&g_dispatch_ran_render_only);
    }
    InterlockedIncrement(&g_dispatch_ran);
    g_effect_dispatch(pass);
}

static bool install() {
    // Sg_MainLoop: if exec==0 { FUN_00616660(0); if (flag) { camera pair } }
    uint8_t* at = sig_find("X2-MAG-008 exec==0 fallback",
        "E8 ?? ?? ?? ?? 85 C0 75 1C 50 E8 ?? ?? ?? ?? 83 C4 04 80 3D ?? ?? ?? ?? 00 74 0A E8 ?? ?? ?? ?? E8", 0x605823);
    if (!at) return false;
    if (!g_get_exec || call_target(at) != g_get_exec) {
        log_line("  X2-MAG-008: first CALL is not Sg_GetCurExecFrames - refused");
        return false;
    }
    uint8_t* site = at + 0x0A;
    uint8_t* target = call_target(site);
    if (!sig_match(target, "55 8B EC 51 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 53 8B 5D 08 8B C3 83 E8 00")) {
        log_line("  X2-MAG-008: CALL target %p is not the effect dispatch (prologue mismatch) - refused", target);
        return false;
    }
    g_effect_dispatch = (effect_dispatch_fn)target;
    g_opt_pass = config_row("X2-MAG-008-PASS", 1);
    parse_list("X2-MAG-008-PASS-LIST", "885 937 906 212 330 386 283", g_pass_list, &g_pass_list_count);
    parse_list("X2-MAG-008-PASS-NOCHAIN", "386 283", g_nochain_list, &g_nochain_list_count);
    if (g_pass_list_count) log_line("  X2-MAG-008-PASS-LIST: %d magic DLL(s) passed by number, %d of them start no chain", g_pass_list_count,
                           g_nochain_list_count);
    if (g_opt_pass && !magic_mods_subscribe("X2-MAG-008", on_load, on_unload)) g_opt_pass = 0;
    if (!patch_call_rel32(site, target, (void*)&hook_render_only_dispatch)) {
        log_line("  X2-MAG-008: patch failed");
        return false;
    }
    log_line("  X2-MAG-008: CALL at gva=0x%08X -> stub (dispatch gva=0x%08X skipped on render-only invocations)",
             rt_to_gva(site), rt_to_gva(target));
    return true;
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_dispatch_skipped > 0) {
        reported = 1;
        log_line("X2-MAG-008: first skip of the exec==0 dispatch (skipped=%ld passed=%ld)", g_dispatch_skipped, g_dispatch_ran);
    }
    static LONG ro_seen = 0;
    if (g_dispatch_ran_render_only && !ro_seen) {
        ro_seen = 1;
        log_line("X2-MAG-008-PASS: first render-only dispatch run for a cross-fade DLL");
    }
}

Row row_MAG008 = {
    "X2-MAG-008",
    "skip the exec==0 effect dispatch FUN_00616660(0) on render-only invocations",
    1, install, nullptr, post, 0
};

} // namespace x2
