// SPDX-License-Identifier: Apache-2.0
// X2-MAG-008 (port of FFX MAG-008): on render-only invocations Sg_MainLoop's exec==0 fallback runs the effect
// dispatch FUN_00616660(0) (FFX MsBattleScene_DrawDispatch): every queued effect / magic overlay updates again,
// so effects run 2x. The CALL at 0x60582D goes to a stub that skips it on render-only invocations.
// Evidence: outer repo re/ledger/PORT-A.md §1 (S), re/MAINLOOP.md §9.4.
// X2-MAG-008-PASS (default 1; port of FFX MAG-008-PASS / the MAG-021 symptom): the 63 "cross fade" magic DLLs
// (dressphere change and others, magic_0313 .. 0990, string "kohno corss fade begin") draw parts of their effect only
// right after their update, which this dispatch runs: skipped on render-only invocations, the character model and
// effects were drawn every second frame (U 16/10/2026: transformation blink, gone with X2-MAG-008 off via F8). While
// such a DLL is loaded the dispatch runs on render-only invocations too; their update steps only when the exec getter
// (+0x400) is non-zero (S, magic_0646 FUN_10002b90 / FUN_10003fb0). Ledger: re/ledger/XFADE-BLINK.md.
// X2-MAG-008-PASS-LIST (ini): magic numbers passed as well, by name (U 18/10/2026: Rikku -> Warrior
// 0885 -> 0937 -> 0906 still alternates dark / lit frames after the body fixes; none of the three is string-matched,
// and PASS=2 crashes magic_0888). Listed DLLs start a chain like string-matched ones. Default "885 937 906 212 330 386 283" (Rikku ->
// Warrior; U 18/10 fixed at 60 fps with X2-MAG-025's list profile; 212 = arena "VS." intro wave overlay, blinked every
// second frame, U 21/10 fixed by the list; 330 = arena result "LOSER..." banner, same, U 21/10; ARENA-VS.md; 386 = Seymour's Requiem, same template, U 22/10 fixed with no chain;
// 283 = Big Bang, same template and same neighbours (0840 before, 0106 after), U 22/10 blink). While a listed DLL, or one chained from it, is
// loaded, mag008_list_active() is true (X2-MAG-025 uses its list profile then).
// X2-MAG-008-PASS-NOCHAIN (ini, default "386 283"): listed numbers that do not start a chain. U/D 22/10/2026: a chain from a
// listed DLL passed the next battle effect magic_0449, which then crashed at FFX-2.exe 0x864C50 (ARENA-VS.md); the
// reverse dressphere change needs the chain from 937 (0888 chained, XFADE-BLINK.md), so it stays per number.
#include "../x2.h"
#include <string.h>
#include <stdlib.h>

namespace x2 {

typedef void(__cdecl* effect_dispatch_fn)(int);         // cdecl int, plain RET 0x61675D (PORT-A §1)

static effect_dispatch_fn g_effect_dispatch = nullptr;
static volatile LONG g_dispatch_skipped = 0, g_dispatch_ran = 0, g_dispatch_ran_render_only = 0;
static int  g_opt_pass = 1;                       // [rows] X2-MAG-008-PASS
static volatile LONG g_passed_dlls_loaded = 0;          // cross-fade DLLs loaded now
extern Row row_MAG008;

// Families passed (strings in the DLL image): "kohno corss fade begin" (63 cross-fade DLLs, dressphere change) and
// "ky: *** Chr Packet Size" (62 "ky" overlays that load their own character model, e.g. magic_0931 after the change:
// model + fire effect still blinked with the first family alone, U 16/10/2026 20:41).
static bool image_contains(const uint8_t* base, uint32_t size, const char* k) {
    const uint32_t n = (uint32_t)strlen(k);
    __try {
        for (uint32_t i = 0; i + n <= size; i++)
            if (base[i] == (uint8_t)k[0] && memcmp(base + i, k, n) == 0) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
// X2-MAG-008-PASS=2 (test mode): also the 310-DLL effect template (magic_0938 / magic_0904 shape, FUN_10001330
// prologue below), e.g. magic_0904 at the start of a second dressphere change (U 16/10/2026 23:10: still blinks).
// The same template carries most battle effects, so this mode is expected to bring back 2x effects: test only.
static const uint8_t TEMPLATE_PROLOGUE[] = {0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08, 0x56, 0x8B, 0x8B, 0xD0, 0x00, 0x00, 0x00, 0x57,
    0x8B, 0x41, 0x14, 0xA3};
static bool is_passed_family(const uint8_t* base, uint32_t size) {
    if (image_contains(base, size, "kohno corss fade begin") || image_contains(base, size, "ky: *** Chr Packet Size")) return true;
    if (g_opt_pass != 2) return false;
    __try {
        for (uint32_t i = 0; i + sizeof(TEMPLATE_PROLOGUE) <= size; i++)
            if (base[i] == 0x55 && memcmp(base + i, TEMPLATE_PROLOGUE, sizeof(TEMPLATE_PROLOGUE)) == 0) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
static uint8_t* g_passed_base[8];
static bool     g_passed_starts_chain[8];                       // matched by string (only these start a chain: one hop)
static bool     g_passed_listed[8];                      // listed, or chained from a listed DLL
static volatile LONG g_listed_dlls_loaded = 0;
static bool     g_chain_from_listed = false;            // the chain was started by a listed DLL
bool mag008_list_active() { return g_listed_dlls_loaded > 0; }
// Chain: a DLL loaded within 500 ms of a passed DLL's unload is passed too (the dressphere change runs magic_0646 ->
// magic_0938 -> magic_0931 back to back, 2-3 ms apart; 0938 has neither string; U 16/10/2026 20:45 still blinked).
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
            g_passed_starts_chain[i] = own && !(listed && in_list(g_nochain_list, g_nochain_list_count, dll_number(name)));   // starts a chain
            InterlockedIncrement(&g_passed_dlls_loaded);
            if (prof) InterlockedIncrement(&g_listed_dlls_loaded);
            break;
        }
    log_line("X2-MAG-008-PASS: %s is a cross-fade / ky model DLL - exec==0 dispatch runs on render-only invocations while loaded", name);
}
// X2-MAG-035: pass a DLL that is already loaded, as a listed DLL that starts no chain (game thread). False if the
// pass is off, the DLL is already passed, or no slot is free.
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
