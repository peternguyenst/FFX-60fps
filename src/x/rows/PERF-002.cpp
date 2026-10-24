// SPDX-License-Identifier: Apache-2.0
// PERF-002 (default on): step down when the PC cannot hold the frame rate the player asked for. Game logic
// advances once per logic tick, and at divider 1 the backlog re-run only starts at >= 33 ms per frame
// (re/MAINLOOP.md section 5), so a PC that draws 45 fps while the mod paces for 60 runs the game at 0.75x
// speed. Dropping to the highest 30 fps notch at or below the measured rate (240 -> 120 for a PC that draws 141;
// at least one notch, ... -> 60 -> stock 30) keeps game speed right in one step (PERF-002-JUMP; U 25/10/2026: 240
// stepped 240 -> 210 -> 180 -> 150 over ~6 s at 141 fps, slow all the way, and 150 stayed ~6% slow).
//
// Frame times are measured at Sg_MainLoop entry on paced invocations, in windows of PERF-002-WINDOW frames;
// frames over 50 ms (loads, hitches) are counted apart, and a window with more than a quarter of them is
// ignored. The limit is PERF-002-FPS out of 60 of whatever rate is in force (the default 55 means 55 fps at
// 60, 110 at 120, 220 at 240). Two slow windows in a row step down; the rate the player asked for is retried
// after 20 s, then 40, 80 ... up to 640 s while retries keep failing (one slow window ends a retry), one step
// at a time. A retry that fails at the same measured rate as the fall before it (within 2%) is a frame cap
// (driver low-latency cap, RTSS, monitor): no more retries until the player changes the rate (PERF-002-CAP).
// Every decision is logged.
#include "../fps60.h"
#include <math.h>

enum { HITCH_MS = 50, BASE_S = 20, MAX_S = 640 };

static LARGE_INTEGER g_freq = {}, g_prev = {};
static double   g_limit_frac = 55.0 / 60.0;       // of the rate in force
static uint32_t g_win = 120;
static uint32_t g_n = 0, g_hitch = 0;
static double   g_sum = 0;
static int      g_slow = 0;                       // consecutive slow windows
static bool     g_held = false, g_retry = false;
static uint32_t g_backoff_s = BASE_S;
static LONGLONG g_retry_at = 0;
static uint32_t g_falls = 0;
static uint32_t g_gen = 0;
static bool     g_jump = true;                    // PERF-002-JUMP
static bool     g_capstop = true;                 // PERF-002-CAP
static double   g_fall_fps = 0;                   // measured fps at the last fall
static bool     g_capped = false;
static int      g_user = 0;                       // rate_user_fps() the cap was found under

// One 30 fps notch; a PC that draws 200 fps still settles at 180 (fall_to never goes below the measured rate's notch).
static int step_down(int fps) { return fps > 60 ? fps - 30 : 0; }                  // 0 = stock 30
static int step_up(int fps)   { return fps + 30; }

// The highest notch the measured rate holds, at least one notch below cur.
static int fall_to(int cur, double measured) {
    int down = step_down(cur);
    if (!g_jump || !down) return down;
    int fit = (int)(measured / 30.0) * 30;
    if (fit < 60) return 0;
    return fit < down ? fit : down;
}

static double limit_ms() { return 1000.0 / (rate_mult() * 30.0 * g_limit_frac); }

static void hold(bool on) {
    if (g_held == on) return;
    g_held = on;
    pacing_hold_stock("PERF-002", on);
}

static void reset_window() { g_n = g_hitch = 0; g_sum = 0; }

static void arm_retry() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_retry) g_backoff_s = g_backoff_s * 2 > MAX_S ? MAX_S : g_backoff_s * 2;
    g_retry_at = now.QuadPart + (LONGLONG)g_backoff_s * g_freq.QuadPart;
    g_retry = false;
    g_slow = 0;
    g_falls++;
}

static void window_done() {
    if (g_hitch * 4 > g_win) {
        log_line("PERF-002: window ignored (%u of %u frames over %d ms: loading or hitches)", g_hitch, g_win, HITCH_MS);
        reset_window();
        return;
    }
    double mean = g_n ? g_sum / g_n : 0;
    double lim = limit_ms();
    // A retry must reach the full rate (97%, frame jitter only): with the 55/60 limit, 150 "held" at 142.9 fps
    // under a 141 cap and the game ran ~5% slow (D fps60.log 02:28:33).
    if (g_retry && g_jump) lim = 1000.0 / (rate_mult() * 30.0 * 0.97);
    bool slow = mean > lim;
    reset_window();
    if (!slow) {
        if (g_retry) {
            log_line("PERF-002: %d fps retry holds (mean %.2f ms, %.1f fps); back-off reset", rate_mult() * 30,
                     mean, 1000.0 / mean);
            g_retry = false;
            g_backoff_s = BASE_S;
            if (rate_fps() < rate_user_fps()) {    // one more step up, after a window at this one
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                g_retry_at = now.QuadPart + (LONGLONG)g_backoff_s * g_freq.QuadPart;
            }
        }
        g_slow = 0;
        return;
    }
    g_slow++;
    if (g_slow < (g_retry ? 1 : 2)) return;
    double fps = 1000.0 / mean;
    bool was_retry = g_retry;
    int cur = rate_fps(), down = fall_to(cur, fps);
    arm_retry();
    if (g_capstop && was_retry && g_fall_fps > 0 && fabs(fps - g_fall_fps) < g_fall_fps * 0.02) {
        g_capped = true;
        g_user = rate_user_fps();
        log_line("PERF-002: retry failed at %.1f fps again (last fall %.1f fps): a frame cap outside the game "
                 "(driver low-latency/G-Sync cap, RTSS, monitor); no more retries until the frame rate is changed",
                 fps, g_fall_fps);
    }
    g_fall_fps = fps;
    if (down && rate_step_fps(down)) {
        log_line("PERF-002: %d fps not held (mean %.2f ms, %.1f fps, limit %.1f fps): %d fps, retry in %u s (fall %u)",
                 cur, mean, 1000.0 / mean, 1000.0 / lim, down, g_backoff_s, g_falls);
        return;
    }
    log_line("PERF-002: %d fps not held (mean %.2f ms, %.1f fps, limit %.1f fps): stock 30 fps, retry in %u s (fall %u)",
             cur, mean, 1000.0 / mean, 1000.0 / lim, g_backoff_s, g_falls);
    hold(true);
}

static void pre() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (rate_gen() != g_gen) { g_gen = rate_gen(); g_prev.QuadPart = 0; reset_window(); }
    if (g_capped && rate_user_fps() != g_user) {
        g_capped = false;
        g_fall_fps = 0;
        log_line("PERF-002: frame rate changed to %d fps; retries back on", rate_user_fps());
    }
    if (g_held || rate_fps() < rate_user_fps()) {
        if (g_held) g_prev.QuadPart = 0;
        if (now.QuadPart >= g_retry_at && !g_retry && !g_capped) {
            g_retry = true;
            reset_window();
            if (g_held) { log_line("PERF-002: retrying %d fps", rate_mult() * 30); hold(false); }
            else {
                int up = step_up(rate_fps());
                if (up > rate_user_fps()) up = rate_user_fps();
                log_line("PERF-002: retrying %d fps", up);
                rate_step_fps(up);
            }
            g_prev.QuadPart = 0;
            return;
        }
        if (g_held) return;
    }
    int k = pacing_kind();
    if (k != INV_LOGIC && k != INV_BUILD) { g_prev.QuadPart = 0; return; }   // stock, backlog, other holders
    if (g_prev.QuadPart) {
        double ms = (now.QuadPart - g_prev.QuadPart) * 1000.0 / g_freq.QuadPart;
        if (ms > HITCH_MS) g_hitch++;
        else { g_sum += ms; g_n++; }
        if (g_n + g_hitch >= g_win) window_done();
    }
    g_prev = now;
}

static bool install() {
    QueryPerformanceFrequency(&g_freq);
    int fps = config_row("PERF-002-FPS", 55);
    if (fps < 30) fps = 30;
    if (fps > 59) fps = 59;
    g_limit_frac = fps / 60.0;
    g_win = (uint32_t)config_row("PERF-002-WINDOW", 120);
    if (g_win < 30) g_win = 30;
    g_gen = rate_gen();
    g_jump = config_row("PERF-002-JUMP", 1) != 0;
    g_capstop = config_row("PERF-002-CAP", 1) != 0;
    g_user = rate_user_fps();
    log_line("  PERF-002: fall to the highest notch the measured rate holds %s; frame-cap detection %s",
             g_jump ? "on" : "off (one notch)", g_capstop ? "on" : "off");
    log_line("  PERF-002: step down below %d of every 60 fps over 2 windows of %u frames (%.0f fps at the asked-for "
             "%d); retry after %d s, doubling to %d s", fps, g_win, rate_user_fps() * g_limit_frac, rate_user_fps(),
             BASE_S, MAX_S);
    return true;
}

static void stop() { hold(false); }

Row row_PERF002 = {
    "PERF-002",
    "step down one 30 fps notch (right game speed) when the PC cannot hold the rate; retries with back-off",
    1, install, pre, nullptr, 0, stop
};
