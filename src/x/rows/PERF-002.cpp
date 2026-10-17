// SPDX-License-Identifier: Apache-2.0
// PERF-002 (default on): step down when the PC cannot hold the frame rate the player asked for. Game logic
// advances once per logic tick, and at divider 1 the backlog re-run only starts at >= 33 ms per frame
// (re/MAINLOOP.md section 5), so a PC that draws 45 fps while the mod paces for 60 runs the game at 0.75x
// speed. Dropping one 30 fps notch - 240 -> 210 -> 180 ... -> 60 -> stock 30 - keeps game speed right and
// settles on the highest rate the PC actually holds.
//
// Frame times are measured at Sg_MainLoop entry on paced invocations, in windows of PERF-002-WINDOW frames;
// frames over 50 ms (loads, hitches) are counted apart, and a window with more than a quarter of them is
// ignored. The limit is PERF-002-FPS out of 60 of whatever rate is in force (the default 55 means 55 fps at
// 60, 110 at 120, 220 at 240). Two slow windows in a row step down; the rate the player asked for is retried
// after 20 s, then 40, 80 ... up to 640 s while retries keep failing (one slow window ends a retry), one step
// at a time. Every decision is logged.
#include "../fps60.h"

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

// One 30 fps notch at a time, so a PC that draws 200 fps settles at 180 instead of falling to 120.
static int step_down(int fps) { return fps > 60 ? fps - 30 : 0; }                  // 0 = stock 30
static int step_up(int fps)   { return fps + 30; }

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
    int cur = rate_fps(), down = step_down(cur);
    arm_retry();
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
    if (g_held || rate_fps() < rate_user_fps()) {
        if (g_held) g_prev.QuadPart = 0;
        if (now.QuadPart >= g_retry_at && !g_retry) {
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
