// SPDX-License-Identifier: Apache-2.0
// MAG-032: family-B magic DLLs (130 on disk, e.g. Nova magic_0422, Oblivion magic_0154) have an update that only walks
// their effect-unit scripts, pass 0 (service slot +0xB30 with (1, 0)). Camera and screen-warp units are set up there,
// and MAG-008 skips the update on render-only invocations, so those frames drew the battle camera between Nova's
// (U 27/10/2026, three consecutive screenshots: top-down warped view, battle view, top-down). In place of the skipped
// dispatch this row calls only those updates, with MAG-025 holding the walk (WAIT counts kept, per-frame loops run).
// Details: docs/fps60/rows/MAG-032.md
#include "../fps60.h"

typedef int      (__cdecl* f_count_t)(void);
typedef uint8_t* (__cdecl* f_chr_t)(int);
typedef uint8_t* (__cdecl* f_cur_t)(int);
typedef void     (__cdecl* ov_fn_t)(int);

// Family-B update (identical in all 130 DLLs): save the DLL's current pointer, set the "inside overlay call" flag
// (slot +0xCEC) to 1, call slot +0xB30 (1, 0), put the flag and pointer back.
static const char UPDATE[] =
    "57 8B 3D ?? ?? ?? ?? 8B 47 58 85 C0 75 05 A1 ?? ?? ?? ?? 89 47 54 A1 ?? ?? ?? ?? 56 8B 30 C7 00 01 00 00 00 "
    "A1 ?? ?? ?? ?? 6A 00 8B 80 30 0B 00 00 6A 01 FF D0 A1 ?? ?? ?? ?? 83 C4 08 89 30 8B 47 58 5E 85 C0 75 0A "
    "8B 47 54 A3 ?? ?? ?? ?? 5F C3";

// MsBattleScene_DrawDispatch internals (0x787E00), as MAG-021 reads them
static f_count_t g_f_count = nullptr;
static f_chr_t   g_f_chr = nullptr;
static f_cur_t   g_f_cur = nullptr;
static volatile int32_t* g_cur_flag = nullptr;    // DAT_0113335c
static volatile uint8_t* g_st_3364 = nullptr;     // DAT_01133364
static uint8_t* volatile* g_ov_3360 = nullptr;    // DAT_01133360
static bool g_broken = false;
static volatile LONG g_replays = 0;

static void run_update(uint8_t* ov) {
    if (!ov) return;
    ov_fn_t fn = *(ov_fn_t*)(ov + 0xC);
    if (!sig_match((void*)fn, UPDATE)) return;           // not family B (or not a DLL table)
    if (g_f_cur(0) == ov) *g_cur_flag = 1;
    mag025_hold_pass0(true);
    fn(0);
    mag025_hold_pass0(false);
    *g_cur_flag = 0;
    if (InterlockedIncrement(&g_replays) == 1)
        log_line("MAG-032: first family-B update replayed on a render-only invocation (update at %p)", (void*)fn);
}

static void build_hook() {
    if (!row_MAG032.on || g_broken || g_disabled || !mag025_dll_hold_ready()) return;
    __try {
        if (g_f_count()) {
            for (int i = 0; i < 0x1F; i++) {
                uint8_t* chr = g_f_chr(i);
                uint8_t st = chr[0xDFB];
                if (st == 4 || st == 5) run_update(*(uint8_t**)(chr + 0xF8C));
            }
        }
        uint8_t st = *g_st_3364;
        if (st == 4 || st == 5) run_update(*g_ov_3360);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        mag025_hold_pass0(false);
        *g_cur_flag = 0;
        log_line("MAG-032: fault 0x%08lX in a render-only family-B update - row off", GetExceptionCode());
        g_broken = true;
    }
}

static bool install() {
    uint8_t* d = (uint8_t*)mag008_dispatch();
    if (!d || !row_MAG008.on) { log_line("  MAG-032: needs MAG-008 (its skipped dispatch is the hook point) - refused"); return false; }
    if (!row_MAG025.on) { log_line("  MAG-032: needs MAG-025 (it holds the replayed walk) - refused"); return false; }
    // Same landmarks as MAG-021: count/chr calls, chr state +0xDFB and overlay +0xF8C, current-overlay call,
    // "current" flag set then cleared, global scene state and overlay.
    bool ok = sig_match(d + 0x40, "E8 ?? ?? ?? ?? 85 C0") && sig_match(d + 0x51, "E8") &&
              sig_match(d + 0x56, "8A 88 FB 0D 00 00") && sig_match(d + 0x69, "8B B0 8C 0F 00 00") &&
              sig_match(d + 0x7F, "E8") && sig_match(d + 0x8B, "C7 05") && sig_match(d + 0xAE, "C7 05") &&
              read_abs(d + 0xB0) == read_abs(d + 0x8D) &&
              sig_match(d + 0xBF, "A0") && sig_match(d + 0xCC, "8B 35") &&
              call_target(d + 0xE2) && call_target(d + 0xE2) == call_target(d + 0x7F);
    if (!ok) { log_line("  MAG-032: MsBattleScene_DrawDispatch %p has an unexpected shape - refused", d); return false; }
    g_f_count  = (f_count_t)call_target(d + 0x40);
    g_f_chr    = (f_chr_t)call_target(d + 0x51);
    g_f_cur    = (f_cur_t)call_target(d + 0x7F);
    g_cur_flag = (volatile int32_t*)read_abs(d + 0x8D);
    g_st_3364  = (volatile uint8_t*)read_abs(d + 0xC0);
    g_ov_3360  = (uint8_t* volatile*)read_abs(d + 0xCE);
    mag008_add_build_hook(build_hook);
    log_line("  MAG-032: family-B updates (pass-0 walk only) replayed on render-only invocations, walk held by MAG-025");
    return true;
}

static void post() {
    static uint32_t tick = 0;
    static LONG last = 0;
    if (++tick % 1800 == 0 && g_replays != last) {
        last = g_replays;
        log_line("MAG-032: family-B updates replayed so far: %ld", last);
    }
}

Row row_MAG032 = {
    "MAG-032",
    "family-B magic DLL update (effect-unit walk pass 0: camera, screen warp) replayed held on render-only invocations",
    1, install, nullptr, post, 0
};
