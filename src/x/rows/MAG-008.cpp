// SPDX-License-Identifier: Apache-2.0
// On render-only frames the engine's exec==0 fallback runs every summon/spell overlay's update, so summon
// choreography runs at 2x while actors don't. This row skips that one dispatch on render-only frames.
// Details: docs/fps60/rows/MAG-008.md
#include "../fps60.h"
#include <string.h>
#include <stdlib.h>

typedef void(__cdecl* dispatch_t)(int);

static uint8_t*   g_site = nullptr;      // the E8 at 0x8210AC
static dispatch_t g_dispatch = nullptr;  // MsBattleScene_DrawDispatch, 0x787E00
static volatile LONG g_skipped = 0;
static volatile LONG g_passed = 0;
static void (*g_build_hooks[4])();        // MAG-021, MAG-032: run in place of the skipped dispatch
static int g_build_hook_n = 0;
void mag008_add_build_hook(void (*fn)()) { if (g_build_hook_n < 4) g_build_hooks[g_build_hook_n++] = fn; }
void* mag008_dispatch() { return (void*)g_dispatch; }

// MAG-008-PASS (default "0670"): magic DLL numbers whose drawing happens inside this dispatch only. While one is
// loaded the dispatch runs on render-only invocations as without MAG-008 (the DLL's update runs there too). magic_0670
// is the GAME OVER effect: the text was drawn on logic frames only and flashed every frame (U 16/10/2026, two
// consecutive screenshots). Other DLLs keep the skip.
enum { MAX_PASS = 16, MAX_LOADED = 8 };
static int      g_pass_ids[MAX_PASS], g_pass_n = 0;
static uint8_t* g_pass_loaded[MAX_LOADED];
static volatile LONG g_pass_count = 0, g_pass_calls = 0;

static int dll_number(const char* name) {           // "magic_0670.dll" -> 670, else -1
    const char* p = strstr(name, "magic_");
    if (!p) return -1;
    int v = 0, d = 0;
    for (p += 6; *p >= '0' && *p <= '9' && d < 5; p++, d++) v = v * 10 + (*p - '0');
    return d ? v : -1;
}

static void on_load(uint8_t* base, uint32_t, const char* name) {
    int n = dll_number(name);
    for (int i = 0; i < g_pass_n; i++) {
        if (g_pass_ids[i] != n) continue;
        for (int k = 0; k < MAX_LOADED; k++) {
            if (g_pass_loaded[k]) continue;
            g_pass_loaded[k] = base;
            InterlockedIncrement(&g_pass_count);
            log_line("MAG-008-PASS: %s loaded - exec==0 dispatch runs on render-only invocations while it is", name);
            return;
        }
    }
}

static void on_unload(uint8_t* base) {
    for (int k = 0; k < MAX_LOADED; k++)
        if (g_pass_loaded[k] == base) { g_pass_loaded[k] = nullptr; InterlockedDecrement(&g_pass_count); }
}

static void __cdecl h_fallback_dispatch(int pass) {
    if (row_MAG008.on && !g_disabled && pacing_kind() == INV_BUILD && g_pass_count > 0) {
        if (InterlockedIncrement(&g_pass_calls) == 1) log_line("MAG-008-PASS: first render-only dispatch passed");
    } else if (row_MAG008.on && !g_disabled && pacing_kind() == INV_BUILD) {
        InterlockedIncrement(&g_skipped);
        where_sub(0x21);                                 // DIAG-HANGDUMP: MAG-021 replay (0x22 = after)
        for (int i = 0; i < g_build_hook_n; i++) g_build_hooks[i]();
        where_sub(0x22);
        return;
    }
    InterlockedIncrement(&g_passed);
    where_sub(0x23);                                     // DIAG-HANGDUMP: battle scene dispatch (0x24 = after)
    g_dispatch(pass);
    where_sub(0x24);
}

static bool install() {
    // Sg_MainLoop: if exec==0, call MsBattleScene_DrawDispatch(0), then the camera fallback
    uint8_t* at = sig_find_unique("MAG-008 exec==0 fallback (Sg_MainLoop)",
        "E8 ?? ?? ?? ?? 85 C0 75 1C 50 E8 ?? ?? ?? ?? 83 C4 04 80 3D ?? ?? ?? ?? 00 74 0A E8", 0x8210A2);
    if (!at) return false;
    if (call_target(at) != (uint8_t*)g_get_exec) {
        log_line("  MAG-008: first CALL is not Sg_GetCurExecFrames - refused");
        return false;
    }
    g_site = at + 0x0A;
    uint8_t* target = call_target(g_site);
    // MsBattleScene_DrawDispatch: prologue, loads the pass argument and tests it
    if (!sig_match(target, "55 8B EC 53 8B 5D 08 8B C3 56 83 E8 00")) {
        log_line("  MAG-008: CALL target %p is not MsBattleScene_DrawDispatch (prologue mismatch) - refused", target);
        return false;
    }
    g_dispatch = (dispatch_t)target;
    if (!patch_call_rel32(g_site, target, (void*)&h_fallback_dispatch)) {
        log_line("  MAG-008: patch failed");
        return false;
    }
    log_line("  MAG-008: CALL at gva=0x%08X -> stub (DrawDispatch gva=0x%08X skipped on render-only invocations)",
             rt_to_gva_exe(g_site), rt_to_gva_exe(target));
    char lst[96];
    config_str("rows", "MAG-008-PASS", lst, sizeof(lst), "0670");
    for (char* q = lst; *q && g_pass_n < MAX_PASS;) {
        char* e = nullptr;
        long v = strtol(q, &e, 10);
        if (e == q) { q++; continue; }
        g_pass_ids[g_pass_n++] = (int)v;
        q = e;
    }
    if (g_pass_n && !magic_mods_subscribe("MAG-008-PASS", on_load, on_unload)) g_pass_n = 0;
    log_line("  MAG-008-PASS: '%s' (%d DLLs pass the dispatch while loaded)", lst, g_pass_n);
    return true;
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_skipped > 0) {
        reported = 1;
        log_line("MAG-008: first skip of the exec==0 dispatch (skipped=%ld passed=%ld)", g_skipped, g_passed);
    }
}

Row row_MAG008 = {
    "MAG-008",
    "skip MsBattleScene_DrawDispatch(0) on render-only invocations (summon/overlay update, OPU pass 0)",
    1, install, nullptr, post, 0
};
