// SPDX-License-Identifier: Apache-2.0
// DIAG-FRAMETIME (diagnostic, default off, observe only): frame-to-frame time at the start of each Sg_MainLoop
// invocation under 60 Hz pacing (logic and render-only invocations; backlog and stock invocations reset the
// chain). The host limiter SKS_WinMainLoop 0x62EE50 waits with Sleep() at a 5 ms timer resolution (re/MAINLOOP.md
// section 2), which VRR displays show as uneven frames; whole-millisecond sleeps matter more the shorter the
// period is (4.17 ms at 240 fps), so PERF-001's timeBeginPeriod(1) belongs with this. Every DIAG-FRAMETIME-N
// frames: mean, standard deviation, percentiles, min/max, and counts outside the rate's period by the 60 fps
// +-1 / +-2 ms bands scaled to that period.
// Each summary also splits the mean frame into game work (Sg_MainLoop entry to exit), Sleep on the game thread
// (the host limiter) and the rest (render + Present outside Sg_MainLoop): U 25/10/2026, 240 fps held at exactly
// 141.0 fps (7.09 ms) with V-Sync and driver limits off.
#include "../fps60.h"
#include <math.h>
#include <string.h>

enum { NB = 400 };                               // 0.125 ms buckets, 0..50 ms; last bucket = overflow
static uint32_t g_hist[NB + 1];
static uint32_t g_n = 0, g_win = 600;
static double   g_sum = 0, g_sum2 = 0, g_min = 1e9, g_max = 0;
static LARGE_INTEGER g_freq = {}, g_prev = {}, g_entry = {};
static double   g_work = 0, g_sleep = 0, g_cur_sleep = 0, g_cur_work = 0;
static uint32_t g_sleeps = 0, g_cur_sleeps = 0;
static DWORD    g_tid = 0;
static bool     g_hooked = false;
typedef void(WINAPI* sleep_t)(DWORD);
static sleep_t  g_sleep_tramp = nullptr;

static void WINAPI h_sleep(DWORD ms) {
    if (GetCurrentThreadId() != g_tid) { g_sleep_tramp(ms); return; }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    g_sleep_tramp(ms);
    QueryPerformanceCounter(&b);
    g_cur_sleep += (b.QuadPart - a.QuadPart) * 1000.0 / g_freq.QuadPart;
    g_cur_sleeps++;
}

static double pct(double p) {
    uint32_t want = (uint32_t)(p * g_n), acc = 0;
    for (int i = 0; i <= NB; i++) { acc += g_hist[i]; if (acc > want) return i * 0.125 + 0.0625; }
    return NB * 0.125;
}

static void dump() {
    double mean = g_sum / g_n, sd = sqrt(g_sum2 / g_n - mean * mean);
    double want = 1000.0 / (rate_mult() * 30.0);     // the period the mod's pacing asks the limiter for
    double w1 = want / 16.683, w2 = 2.0 * w1;        // the 1 / 2 ms bands of 60 fps, scaled to this rate
    uint32_t off1 = 0, off2 = 0;
    for (int i = 0; i <= NB; i++) {
        double c = i * 0.125 + 0.0625, d = fabs(c - want);
        if (d > w1) off1 += g_hist[i];
        if (d > w2) off2 += g_hist[i];
    }
    log_line("DIAG-FRAMETIME: %u frames: mean %.3f ms (%.2f fps), sd %.3f, min %.2f p1 %.2f p5 %.2f p50 %.2f p95 %.2f p99 %.2f "
             "max %.2f; off %.2f by >%.2f ms %u, >%.2f ms %u", g_n, mean, 1000.0 / mean, sd, g_min, pct(0.01), pct(0.05),
             pct(0.5), pct(0.95), pct(0.99), g_max, want, w1, off1, w2, off2);
    log_line("DIAG-FRAMETIME:   split per frame: game work %.3f ms, limiter Sleep %.3f ms (%.2f calls), rest (render + "
             "Present) %.3f ms", g_work / g_n, g_sleep / g_n, (double)g_sleeps / g_n, mean - (g_work + g_sleep) / g_n);
    memset(g_hist, 0, sizeof(g_hist));
    g_n = 0; g_sum = g_sum2 = 0; g_min = 1e9; g_max = 0;
    g_work = g_sleep = 0; g_sleeps = 0;
}

static void pre() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_hooked) {
        g_hooked = true;
        g_tid = GetCurrentThreadId();
        void* sl = (void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "Sleep");
        if (!sl || !hook_entry("DIAG-FRAMETIME Sleep", sl, (void*)&h_sleep, (void**)&g_sleep_tramp))
            log_line("DIAG-FRAMETIME: Sleep hook failed - no split");
    }
    double slept = g_cur_sleep, work = g_cur_work;
    uint32_t sleeps = g_cur_sleeps;
    g_cur_sleep = 0; g_cur_sleeps = 0; g_cur_work = 0;
    g_entry = now;
    int k = pacing_kind();
    if (k != INV_LOGIC && k != INV_BUILD) { g_prev.QuadPart = 0; return; }
    if (g_prev.QuadPart) {
        double ms = (now.QuadPart - g_prev.QuadPart) * 1000.0 / g_freq.QuadPart;
        g_work += work; g_sleep += slept; g_sleeps += sleeps;
        int b = (int)(ms / 0.125);
        g_hist[b < 0 ? 0 : b > NB ? NB : b]++;
        g_n++; g_sum += ms; g_sum2 += ms * ms;
        if (ms < g_min) g_min = ms;
        if (ms > g_max) g_max = ms;
        if (g_n >= g_win) dump();
    }
    g_prev = now;
}

static void post() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_entry.QuadPart) g_cur_work = (now.QuadPart - g_entry.QuadPart) * 1000.0 / g_freq.QuadPart;
    g_cur_sleep = 0; g_cur_sleeps = 0;           // only the gap after this invocation counts
}

static bool install() {
    QueryPerformanceFrequency(&g_freq);
    g_win = (uint32_t)config_row("DIAG-FRAMETIME-N", 600);
    if (g_win < 60) g_win = 60;
    log_line("  DIAG-FRAMETIME: frame times at Sg_MainLoop entry, summary every %u frames", g_win);
    return true;
}

Row row_DIAGFRAMETIME = {
    "DIAG-FRAMETIME",
    "diagnostic: frame-to-frame time histogram under the mod's pacing (VRR pacing check); observe only",
    0, install, pre, post, 0
};
