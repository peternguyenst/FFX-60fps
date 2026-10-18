// SPDX-License-Identifier: Apache-2.0
// RATE-001 (default on): retarget the game's own frame limiter so the host loop can run faster than 59.94 Hz.
//
// SKS_WinMainLoop's wait (0x62F2EC, re/MAINLOOP.md section 2) keeps a schedule accumulator DAT_00cc9d00 and
// decides per pass whether to step a frame or Sleep:
//     elapsed = now() - DAT_00c90278
//     period  = (double)frame_divider * DAT_00b412d8          ; 0x00B412D8 = 1 / 59.94
//     carry   = min(elapsed - accum - period, period)
//     carry > 0 ? run one Sg_MainLoop pass : Sleep(-carry * 1000)
// So the period is `divider * K`, and K - a double with exactly one cross-reference in .text - is the only
// thing that fixes the loop at 59.94 Hz. The mod holds the divider at 1 (pacing.cpp), so writing K = 2 / (N *
// 59.94) makes the loop run at N * 29.97 Hz: 60, 120 or 240 fps for N = 2, 4, 8. K is restored whenever the
// mod's pacing is not applied (menus, FMV hold, PERF-002 fallback), where the game needs its own 59.94 Hz.
//
// K is written from the game thread between Sg_MainLoop invocations, so the limiter - which runs on the same
// thread, between those invocations - can never read a half-written double.
//
// Above 60 fps the game must also present without V-Sync (its own Config page, [Graphics] VSync), or the swap
// chain holds the loop at the monitor's refresh rate however short the period is. The limiter Sleeps in whole
// milliseconds, so PERF-001's timeBeginPeriod(1) matters more the shorter the period gets (4.17 ms at 240).
// Details: docs/fps60/rows/RATE-001.md
#include "../fps60.h"
#include <math.h>

static double* g_k = nullptr;        // DAT_00b412d8: seconds per divider unit
static double  g_stock = 0;          // the game's own value (1 / 59.94)
static int     g_mult = 2;           // N the limiter should be held at
static bool    g_applied = false;    // our value is in place

// The period the game's own constant stands for is 2 / 59.94 per logic tick; ours is that divided by N / 2.
static double period_for(int mult) { return g_stock * 2.0 / (double)mult; }

static bool write_k(double v) {
    if (!g_k) return false;
    if (!patch_write(g_k, &v, sizeof(v))) {
        log_line("RATE-001: writing the limiter period failed - row off");
        row_RATE001.on = 0;
        return false;
    }
    return true;
}

// pacing.cpp, game thread, once per invocation before the pass kind is chosen.
void rate001_limiter(bool want) {
    bool on = want && row_RATE001.on && !g_disabled && g_k && g_mult > 2;
    if (on == g_applied) return;
    if (!write_k(on ? period_for(g_mult) : g_stock)) {
        rate_limiter_engaged(false);
        g_applied = false;
        return;
    }
    g_applied = on;
    log_line("RATE-001: host limiter at %.2f Hz (period %.4f ms)", 1.0 / (period_for(on ? g_mult : 2)),
             period_for(on ? g_mult : 2) * 1000.0);
    rate_limiter_engaged(on);
}

// rate.cpp: the configured rate changed. Returns false when the limiter is not ours to move, which is only
// fatal above 60 fps (rate.cpp refuses the change then).
bool rate001_limiter_set(int mult) {
    g_mult = mult;
    if (!row_RATE001.on || !g_k) return mult <= 2;
    if (g_applied) {                                  // re-apply at the new rate, or step back to stock
        if (mult > 2) { write_k(period_for(mult)); }
        else { write_k(g_stock); g_applied = false; rate_limiter_engaged(false); }
    }
    return true;
}

static bool install() {
    // SKS_WinMainLoop frame wait: FSTP/FLD the divider, FMUL the period constant, FSUB the accumulator
    uint8_t* at = sig_find_unique("RATE-001 frame limiter period (SKS_WinMainLoop)",
        "DD 9D ?? ?? ?? ?? DD 85 ?? ?? ?? ?? DC 0D ?? ?? ?? ?? D9 C1 DC 25 ?? ?? ?? ?? D8 E1 D8 D1 DF E0 F6 C4 41",
        0x62F30C);
    if (!at) return false;
    double* k = (double*)read_abs(at + 0x0E);
    double v = 0;
    if (!k || !safe_copy(&v, k, sizeof(v)) || !(v > 1.0 / 400.0) || !(v < 1.0 / 20.0)) {
        log_line("  RATE-001: limiter constant at %p is %g, not a frame period - refused", (void*)k, v);
        return false;
    }
    // The game's own rate must be the 59.94 Hz the rest of the mod's timebase assumes.
    double hz = 1.0 / v;
    if (fabs(hz - 59.94005994) > 0.01) {
        log_line("  RATE-001: limiter constant is %.4f Hz, not 59.94 - refused", hz);
        return false;
    }
    g_k = k;
    g_stock = v;
    g_mult = rate_mult_cfg();
    log_line("  RATE-001: limiter period gva=0x%08X = %.9f s (%.4f Hz); %d fps asks for %.9f s (%.4f Hz)",
             rt_to_gva_exe(k), v, hz, rate_fps(), period_for(g_mult), 1.0 / period_for(g_mult));
    if (g_mult > 2)
        log_line("  RATE-001: above 60 fps the game must present without V-Sync (its Config page), or the swap "
                 "chain holds the loop at the monitor's refresh rate");
    return true;
}

// Row off at runtime / dev hot reload: the game gets its own period back.
static void stop() {
    if (!g_applied) return;
    write_k(g_stock);
    g_applied = false;
    rate_limiter_engaged(false);
    log_line("RATE-001: limiter back at stock 59.94 Hz");
}

Row row_RATE001 = {
    "RATE-001",
    "host frame limiter retargeted to fps x 29.97 Hz, so 120 and 240 fps can run (needs V-Sync off above 60)",
    1, install, nullptr, nullptr, 0, stop
};
