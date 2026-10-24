// SPDX-License-Identifier: Apache-2.0
// Timebase multiplier. The game's logic runs at 29.97 Hz whatever the frame rate; the mod draws N passes per
// logic tick, N = fps / 30. N only has to be a whole number of passes per tick, so every multiple of 30 from
// 60 to 240 works, not just the doubling steps - which matters because a PC that draws 200 fps can hold 180
// but not 240, and would otherwise fall all the way to 120. Pass 0 of each period is the logic tick, passes
// 1..N-1 are render-only (pacing.cpp), and each pass draws the world (pass + 1) / N of the way from the
// previous tick to this one (pacing_t, used by the SMOOTH rows). N = 2 is the original 60 fps behaviour, and
// every value and decision below reduces to the old code at N = 2.
//
// N > 2 also needs the game's own frame limiter retargeted from 59.94 Hz to N * 29.97 Hz (rows/RATE-001.cpp).
// Without it the host loop still calls Sg_MainLoop 59.94 times a second, and N passes per tick would run the
// game at 2/N speed - so rate_mult() stays at 2 until RATE-001 reports the limiter is held at the mod's rate.
// Above 60 the game must also present without V-Sync (its own Config page), or the swap chain caps the loop.
// Details: docs/fps60/rate.md
#include "fps60.h"

static int      g_fps = 60;        // in force: 60 / 120 / 240
static int      g_user_fps = 60;   // what the player asked for; PERF-002's steps do not change it
static int      g_mult_cfg = 2;    // configured N
static bool     g_limiter = false; // RATE-001 holds the limiter at N * 29.97
static int      g_mult = 2;        // N in force
static uint32_t g_gen = 1;

int rate_fps()      { return g_fps; }
int rate_user_fps() { return g_user_fps; }
int rate_mult_cfg() { return g_mult_cfg; }
int rate_mult()     { return g_mult; }
uint32_t rate_gen() { return g_gen; }

enum { FPS_MIN = 60, FPS_MAX = 240, FPS_STEP = 30 };

bool rate_fps_ok(int fps) { return fps >= FPS_MIN && fps <= FPS_MAX && (fps % FPS_STEP) == 0; }

static int mult_of(int fps) { return fps / FPS_STEP; }

// The effective N follows the limiter: while it is stock, two passes per tick is the only rate that keeps
// game speed right. Rows watch rate_gen() and re-seed whatever they derive from N.
static void recompute(const char* why) {
    int want = g_limiter ? g_mult_cfg : 2;
    if (want == g_mult) return;
    g_mult = want;
    g_gen++;
    log_line("rate: %d passes per logic tick (%d fps) - %s", g_mult, g_mult * 30, why);
}

void rate_limiter_engaged(bool on) {
    if (g_limiter == on) return;
    g_limiter = on;
    recompute(on ? "limiter at the mod's rate" : "limiter back at stock 59.94 Hz");
}

void rate_init() {
    int fps = g_cfg.fps;
    if (!rate_fps_ok(fps)) {
        if (fps != 0) log_line("rate: [fps60] fps=%d is not a multiple of %d between %d and %d - using 60", fps,
                               FPS_STEP, FPS_MIN, FPS_MAX);
        fps = 60;
    }
    g_fps = g_user_fps = g_cfg.fps = fps;
    g_mult_cfg = mult_of(fps);
    g_mult = 2;
    log_line("rate: %d fps requested (%d passes per 29.97 Hz logic tick)%s", g_fps, g_mult_cfg,
             g_mult_cfg > 2 ? "; needs RATE-001 and V-Sync off in the launcher (GameSetting.ini VSync=Off)" : "");
}

// Runtime change, game thread. The limiter is re-applied at the new rate before the multiplier follows, so the
// loop never runs fast with the old pass count.
static bool set_fps(int fps, const char* why) {
    if (!rate_fps_ok(fps)) return false;
    if (fps == g_fps) return true;
    int prev = g_fps, prev_mult = g_mult_cfg;
    g_fps = fps;
    g_mult_cfg = mult_of(fps);
    if (!rate001_limiter_set(g_mult_cfg) && g_mult_cfg > 2) {   // RATE-001 off or its signature failed
        g_fps = prev;
        g_mult_cfg = prev_mult;
        rate001_limiter_set(prev_mult);
        log_line("rate: %d fps refused - the host limiter is not retargetable (row RATE-001)", fps);
        return false;
    }
    log_line("rate: %d fps (%d passes per logic tick) - %s", g_fps, g_mult_cfg, why);
    recompute(why);
    return true;
}

// CFG-MENU: the player picked this rate, so it is also what PERF-002 steps back up to.
bool rate_set_fps(int fps) {
    if (!set_fps(fps, "asked for in the Config page")) return false;
    g_user_fps = fps;
    return true;
}

// PERF-002: the PC could not hold the rate, or is being given it back. What the player asked for stands.
bool rate_step_fps(int fps) { return set_fps(fps, "stepped by PERF-002"); }
