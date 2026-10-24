// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-RATE (diagnostic, off): how often per second each part of the game runs.
//
// Every ~5 s it logs the main loop's invocations per second by kind, and the calls of a few functions counted by entry
// hooks: the actor update (should run at logic rate), the battle process (its own counter is read too), and two
// system-layer functions that run once per host frame outside the main loop. Each target's first bytes are checked
// before it is hooked; a mismatch skips that target. Written to find what ran at double speed on the title screen and
// in battle.
#include "../x2.h"
#include <stdio.h>

namespace x2 {

struct Probe {
    const char*   name;
    uint32_t      gva;
    const char*   prologue;
    void*         tramp;
    volatile LONG calls;
    LONG          last;
};

static Probe g_p[4] = {
    { "actor update FUN_006e10a0",  0x6E10A0, "55 8B EC 83 EC 10 E8" },
    { "battle process FUN_00790f00", 0x790F00, "FF 05 ?? ?? ?? ?? 53 56 57" },
    { "system layer FUN_00466ef0",  0x466EF0, "55 8B EC 6A FF 68" },
    { "host frame FUN_0042f8d0",    0x42F8D0, "56 8B F1 80 BE AC 03 00 00 00 74" },
};

// One naked detour per probe: count, then continue in the trampoline (registers and stack untouched).
#define PROBE_STUB(i) \
    __declspec(naked) static void stub##i() { __asm { lock inc dword ptr [g_p + i * TYPE Probe + 16] } \
                                              __asm { jmp dword ptr [g_p + i * TYPE Probe + 12] } }
PROBE_STUB(0)
PROBE_STUB(1)
PROBE_STUB(2)
PROBE_STUB(3)
static void (*const g_stubs[4])() = { stub0, stub1, stub2, stub3 };

// Timed probes (U 13/10/2026: a 60 fps screen recording shows the camera "move, move, pause, move, pause" while every
// game-thread frame is evenly spaced and evenly moved, D F9 traces): call intervals of the app's render (vtable [8]
// 0x42F7F0) and vtable [10] 0x42F490 (FFX: "present"), i.e. how evenly frames leave the game thread.
struct TProbe {
    const char* name;
    uint32_t    gva;
    const char* prologue;
    void*       tramp;
    LARGE_INTEGER last;
    double      min, max, sum;
    int         n, longs, calls;
};
static TProbe g_tp[2] = {
    { "render [8] FUN_0042f7f0", 0x42F7F0, "56 8B F1 80 BE AD 03 00 00 00 74 09 E8" },
    { "vtable [10] FUN_0042f490", 0x42F490, "56 8B F1 80 BE AD 03 00 00 00 75 15 E8" },
};
static LARGE_INTEGER g_tf;
// Sg_MainLoop work per invocation (pre() end to post() start: the game's own frame, with our rows), by kind.
static LARGE_INTEGER g_ml_t0;
static double g_ml_sum[4], g_ml_max[4];
static int    g_ml_n[4];

extern "C" void __cdecl x2_tprobe_hit(int i) {
    TProbe& p = g_tp[i];
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    p.calls++;
    if (p.last.QuadPart && g_tf.QuadPart) {
        double dt = (double)(now.QuadPart - p.last.QuadPart) * 1000.0 / (double)g_tf.QuadPart;
        if (dt < p.min) p.min = dt;
        if (dt > p.max) p.max = dt;
        p.sum += dt;
        p.n++;
        if (dt > 20.0) p.longs++;
    }
    p.last = now;
}

#define TPROBE_STUB(i)     __declspec(naked) static void tstub##i() { __asm { pushfd } __asm { pushad } __asm { push i }         __asm { call x2_tprobe_hit } __asm { add esp, 4 } __asm { popad } __asm { popfd }         __asm { jmp dword ptr [g_tp + i * TYPE TProbe + 12] } }
TPROBE_STUB(0)
TPROBE_STUB(1)
static void (*const g_tstubs[2])() = { tstub0, tstub1 };

static volatile uint32_t* g_btl_counter = nullptr;
static uint32_t g_btl_last = 0;
static bool     g_hooked = false;
static LARGE_INTEGER g_t0, g_f;
static uint64_t g_kind[4], g_kind_last[4];
// Frame time between invocations (stutter check, U 13/10/2026): min / sum / max and frames over 20 ms per window.
static LARGE_INTEGER g_tprev;
static double g_dt_min = 1e9, g_dt_max = 0, g_dt_sum = 0;
static int    g_dt_n = 0, g_dt_long = 0;
extern Row row_DIAGRATE;

static bool install() {
    static_assert(offsetof(Probe, tramp) == 12 && offsetof(Probe, calls) == 16, "stub offsets");
    for (auto& p : g_p) {
        uint8_t* at = exe_base() + (p.gva - 0x400000);
        if (!sig_match(at, p.prologue)) { log_line("  X2-DIAG-RATE: %s prologue mismatch - not counted", p.name); p.gva = 0; }
    }
    if (g_p[1].gva) g_btl_counter = (volatile uint32_t*)abs_operand("battle entry counter", exe_base() + (0x790F00 - 0x400000) + 2);
    static_assert(offsetof(TProbe, tramp) == 12, "tstub offset");
    for (auto& p : g_tp) {
        p.min = 1e9;
        if (!sig_match(exe_base() + (p.gva - 0x400000), p.prologue)) { log_line("  X2-DIAG-RATE: %s prologue mismatch - not timed", p.name); p.gva = 0; }
    }
    QueryPerformanceFrequency(&g_tf);
    QueryPerformanceFrequency(&g_f);
    QueryPerformanceCounter(&g_t0);
    return true;
}

static void pre() {
    g_kind[pacing_kind()]++;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_tprev.QuadPart) {
        double dt = (double)(now.QuadPart - g_tprev.QuadPart) * 1000.0 / (double)g_f.QuadPart;
        if (dt < g_dt_min) g_dt_min = dt;
        if (dt > g_dt_max) g_dt_max = dt;
        g_dt_sum += dt;
        g_dt_n++;
        if (dt > 20.0) g_dt_long++;
    }
    g_tprev = now;
    QueryPerformanceCounter(&g_ml_t0);
    if (g_hooked) return;
    g_hooked = true;
    for (int i = 0; i < 4; i++) {
        if (!g_p[i].gva) continue;
        uint8_t* at = exe_base() + (g_p[i].gva - 0x400000);
        if (!hook_entry(g_p[i].name, at, (void*)g_stubs[i], &g_p[i].tramp)) g_p[i].gva = 0;
    }
    for (int i = 0; i < 2; i++) {
        if (!g_tp[i].gva) continue;
        if (!hook_entry(g_tp[i].name, exe_base() + (g_tp[i].gva - 0x400000), (void*)g_tstubs[i], &g_tp[i].tramp)) g_tp[i].gva = 0;
    }
    if (g_btl_counter) g_btl_last = *g_btl_counter;
    log_line("X2-DIAG-RATE: probes hooked");
}

static void post() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (g_ml_t0.QuadPart) {
        int kd = pacing_kind() & 3;
        double ms = (double)(t.QuadPart - g_ml_t0.QuadPart) * 1000.0 / (double)g_f.QuadPart;
        g_ml_sum[kd] += ms; g_ml_n[kd]++;
        if (ms > g_ml_max[kd]) g_ml_max[kd] = ms;
    }
    double s = (double)(t.QuadPart - g_t0.QuadPart) / (double)g_f.QuadPart;
    if (s < 5.0) return;
    g_t0 = t;
    char line[1200];
    int k = _snprintf_s(line, sizeof(line), _TRUNCATE, "X2-DIAG-RATE /s: inv LOGIC %.1f BUILD %.1f BACKLOG %.1f STOCK %.1f |",
                        (g_kind[0] - g_kind_last[0]) / s, (g_kind[1] - g_kind_last[1]) / s,
                        (g_kind[2] - g_kind_last[2]) / s, (g_kind[3] - g_kind_last[3]) / s);
    for (int i = 0; i < 4; i++) g_kind_last[i] = g_kind[i];
    for (auto& p : g_p) {
        if (!p.gva) continue;
        LONG c = p.calls;
        k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %s %.1f;", p.name, (c - p.last) / s);
        p.last = c;
    }
    if (g_btl_counter) {
        uint32_t c = *g_btl_counter;
        k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " battle counter %.1f", (c - g_btl_last) / s);
        g_btl_last = c;
    }
    if (g_dt_n)
        k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " | frame ms min %.1f avg %.2f max %.1f, >20ms %d",
                         g_dt_min, g_dt_sum / g_dt_n, g_dt_max, g_dt_long);
    g_dt_min = 1e9; g_dt_max = 0; g_dt_sum = 0; g_dt_n = 0; g_dt_long = 0;
    k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " | mainloop ms logic avg %.2f max %.1f, render-only avg %.2f max %.1f",
                     g_ml_n[0] ? g_ml_sum[0] / g_ml_n[0] : 0.0, g_ml_max[0], g_ml_n[1] ? g_ml_sum[1] / g_ml_n[1] : 0.0, g_ml_max[1]);
    for (int i = 0; i < 4; i++) { g_ml_sum[i] = 0; g_ml_max[i] = 0; g_ml_n[i] = 0; }
    for (auto& p : g_tp) {
        if (!p.gva || !p.n) continue;
        k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " | %s %.1f/s ms min %.1f avg %.2f max %.1f >20ms %d",
                         p.name, p.calls / s, p.min, p.sum / p.n, p.max, p.longs);
        p.min = 1e9; p.max = 0; p.sum = 0; p.n = 0; p.longs = 0; p.calls = 0;
    }
    log_line("%s", line);
}

Row row_DIAGRATE = {
    "X2-DIAG-RATE",
    "diagnostic: per-second rates of invocation kinds and key update functions",
    0, install, pre, post, 0
};

} // namespace x2
