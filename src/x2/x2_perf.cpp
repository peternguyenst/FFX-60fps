// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-PERF (diagnostic, default 0): U 18/10/2026: "on Steam Deck it is not able to keep up 60 fps - is our mod too
// unoptimized?". Every 10 s, on the game thread: invocations per second, the average and worst Sg_MainLoop invocation
// (from x2_pre to the end of x2_post: game logic + render submission + every row), the time the rows' pre/post take per
// invocation and the six costliest rows, and X2-SMOOTH-003's render-only motion walk (the extra engine motion steps
// that 60 fps limbs cost). Off: one flag test per row call.
#include "x2.h"
#include <string.h>
#include <stdio.h>

namespace x2 {

extern Row** x2_row_list();

static int  g_on = -1;                           // -1 = not read yet
static LARGE_INTEGER g_f, g_win, g_t0;
static double g_inv_sum = 0, g_inv_max = 0, g_rows_sum = 0, g_walk_sum = 0;
static LONG   g_inv_n = 0, g_walk_n = 0;
enum { MAXR = 128 };
static double g_row_ms[MAXR];

static inline double ms(LONGLONG d) { return (double)d * 1000.0 / (double)g_f.QuadPart; }

bool perf_on() {
    if (g_on < 0) {
        g_on = config_row("X2-DIAG-PERF", 0) ? 1 : 0;
        QueryPerformanceFrequency(&g_f);
        QueryPerformanceCounter(&g_win);
        if (g_on) log_line("X2-DIAG-PERF: on (10 s summaries of invocation time, row time, SMOOTH-003 walk)");
    }
    return g_on != 0;
}

LONGLONG perf_now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

void perf_row(int idx, LONGLONG dt) { if (idx >= 0 && idx < MAXR) { double m = ms(dt); g_row_ms[idx] += m; g_rows_sum += m; } }
void perf_walk(LONGLONG dt) { g_walk_sum += ms(dt); g_walk_n++; }
void perf_inv_begin() { QueryPerformanceCounter(&g_t0); }

void perf_inv_end() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    double d = ms(t.QuadPart - g_t0.QuadPart);
    g_inv_sum += d;
    if (d > g_inv_max) g_inv_max = d;
    g_inv_n++;
    double s = (double)(t.QuadPart - g_win.QuadPart) / (double)g_f.QuadPart;
    if (s < 10.0 || g_inv_n == 0) return;
    Row** rows = x2_row_list();
    int top[6]; double topv[6];
    for (int k = 0; k < 6; k++) { top[k] = -1; topv[k] = 0; }
    for (int r = 0; r < MAXR && rows[r]; r++) {
        double v = g_row_ms[r];
        for (int k = 0; k < 6; k++) {
            if (v <= topv[k]) continue;
            for (int m = 5; m > k; m--) { top[m] = top[m - 1]; topv[m] = topv[m - 1]; }
            top[k] = r; topv[k] = v;
            break;
        }
    }
    char line[512];
    int n = 0;
    for (int k = 0; k < 6 && top[k] >= 0; k++)
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %s %.3f;", rows[top[k]]->id, topv[k] / g_inv_n);
    log_line("X2-DIAG-PERF: %.1f inv/s, invocation ms avg %.2f max %.2f | rows %.3f ms/inv | SMOOTH-003 walk %.3f ms per render-only "
             "(%ld walks) | top rows ms/inv:%s",
             g_inv_n / s, g_inv_sum / g_inv_n, g_inv_max, g_rows_sum / g_inv_n, g_walk_n ? g_walk_sum / g_walk_n : 0.0, g_walk_n,
             n ? line : " -");
    g_win = t;
    g_inv_sum = g_inv_max = g_rows_sum = g_walk_sum = 0;
    g_inv_n = g_walk_n = 0;
    memset(g_row_ms, 0, sizeof(g_row_ms));
}

} // namespace x2
