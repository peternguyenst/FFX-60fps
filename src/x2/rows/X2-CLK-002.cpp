// SPDX-License-Identifier: Apache-2.0
// X2-CLK-002 (port of FFX CLK-002): map/BG keyframe animations (FUN_007c0580 and 3 siblings) read a clock in 1/60 s
// units from FUN_007b97e0 (frame counter * 2) or FUN_007b9f60 (vblank), both +2 per invocation: 2x at 60 Hz.
// Both getters return a mod clock that adds half the source's delta on 60 Hz invocations (+1 per 1/60 s: correct
// speed, and the consumer's lerp makes the motion 60 Hz smooth), the full delta otherwise.
// Evidence: outer repo re/ledger/PORT-B.md §1 (S).
#include "../x2.h"

namespace x2 {

typedef uint32_t(__cdecl* clock_t_)(void);       // no args, plain RET

struct ModClock {
    const char*        name;
    volatile uint32_t* src;
    uint32_t           mul;          // source units -> 1/60 s at stock
    uint8_t*           entry;
    clock_t_           tramp;
    uint32_t           last;         // source value at the last commit
    uint32_t           base;         // mod clock at seed
    uint64_t           half;         // committed advance since seed, in 1/120 s
    volatile LONG      calls;
    uint32_t           rep_src, rep_mod;
};

static ModClock g_fc = { "frame counter getter FUN_007b97e0", nullptr, 2 };
static ModClock g_vb = { "vblank getter FUN_007b9f60",        nullptr, 1 };
static bool g_hooked = false;
extern Row row_CLK002;

static bool paced_kind() {
    int k = pacing_kind();
    return k == INV_LOGIC || k == INV_RENDER_ONLY;     // backlog/stock invocations are 1/30 s each
}

static uint32_t live(const ModClock& c) { return *c.src * c.mul; }

// Source delta since the last commit. FFX-2 resyncs the vblank counter (FUN_00604d30, e.g. around loads: D 12/10/2026
// 23:09:50, -1955), so a backwards or implausibly large step counts as 0 and the mod clock stays monotonic.
static uint32_t delta(const ModClock& c, uint32_t now) {
    int32_t d = (int32_t)(now - c.last);
    return (d < 0 || d > 0x1000) ? 0 : (uint32_t)d;
}

static uint32_t value(const ModClock& c) {
    uint64_t d = delta(c, live(c));
    uint64_t h = c.half + (paced_kind() ? d : d * 2);
    return c.base + (uint32_t)(h >> 1);
}

static void commit(ModClock& c) {
    uint32_t now = live(c);
    uint64_t d = delta(c, now);
    if (d == 0 && now != c.last)
        log_line("X2-CLK-002: %s source jumped %d (resync) - mod clock held, re-based", c.name, (int32_t)(now - c.last));
    c.half += paced_kind() ? d : d * 2;
    c.last = now;
}

static uint32_t __cdecl h_fc() { InterlockedIncrement(&g_fc.calls); return value(g_fc); }
static uint32_t __cdecl h_vb() { InterlockedIncrement(&g_vb.calls); return value(g_vb); }

static bool install() {
    // FUN_007c0580: copy +0x46d78 -> +0x46d7c; JNZ; CALL fc getter; JMP; CALL vblank getter
    uint8_t* at = sig_find("X2-CLK-002 map anim clock select",
        "8B 87 ?? ?? ?? ?? 89 87 ?? ?? ?? ?? 75 07 E8 ?? ?? ?? ?? EB 05 E8", 0x7C05AE);
    if (!at) return false;
    g_fc.entry = call_target(at + 0x0E);
    g_vb.entry = call_target(at + 0x15);
    // MOV EAX,[frame_counter]; ADD EAX,EAX; RET  /  MOV EAX,[vblank]; RET
    if (!g_fc.entry || !g_vb.entry || !sig_match(g_fc.entry, "A1 ?? ?? ?? ?? 03 C0 C3") ||
        !sig_match(g_vb.entry, "A1 ?? ?? ?? ?? C3")) {
        log_line("  X2-CLK-002: clock getters %p / %p do not match - refused", g_fc.entry, g_vb.entry);
        return false;
    }
    g_fc.src = (volatile uint32_t*)abs_operand("fc getter source", g_fc.entry + 1);
    g_vb.src = (volatile uint32_t*)abs_operand("vblank getter source", g_vb.entry + 1);
    if (!g_fc.src || !g_vb.src || g_fc.src != g_frame_counter) {
        log_line("  X2-CLK-002: getter sources unexpected (fc %p vs frame counter %p) - refused", g_fc.src, g_frame_counter);
        return false;
    }
    log_line("  X2-CLK-002: getters at gva=0x%08X / 0x%08X (expected 0x007B97E0 / 0x007B9F60), hooked on the game thread",
             rt_to_gva(g_fc.entry), rt_to_gva(g_vb.entry));
    return true;
}

static void seed(ModClock& c) {
    c.last = live(c);
    c.base = c.last;
    c.half = 0;
    c.rep_src = c.last;
    c.rep_mod = c.base;
}

static void report(ModClock& c) {
    LONG n = InterlockedExchange(&c.calls, 0);
    uint32_t src = live(c), mod = c.base + (uint32_t)(c.half >> 1);
    if (n > 0) {
        int32_t ds = (int32_t)(src - c.rep_src), dm = (int32_t)(mod - c.rep_mod);
        log_line("X2-CLK-002: %s: %ld calls in the last 600 invocations; source %+d, mod clock %+d (ratio %.2f)",
                 c.name, n, ds, dm, ds > 0 ? (double)dm / (double)ds : 0.0);
    }
    c.rep_src = src;
    c.rep_mod = mod;
}

static void pre() {
    if (g_hooked) return;
    g_hooked = true;
    seed(g_fc);
    seed(g_vb);
    if (!hook_entry("X2-CLK-002 fc", g_fc.entry, (void*)&h_fc, (void**)&g_fc.tramp) ||
        !hook_entry("X2-CLK-002 vb", g_vb.entry, (void*)&h_vb, (void**)&g_vb.tramp)) {
        log_line("X2-CLK-002: getter hook failed - row off");
        row_CLK002.on = 0;
        return;
    }
    log_line("X2-CLK-002: map clock getters hooked (fc=%u vb=%u)", g_fc.base, g_vb.base);
}

static void post() {
    commit(g_fc);
    commit(g_vb);
    static uint32_t tick = 0;
    if (++tick % 600 == 0) { report(g_fc); report(g_vb); }
}

Row row_CLK002 = {
    "X2-CLK-002",
    "map/BG object animation clock advances in 60 Hz units of wall time under 60 Hz pacing",
    1, install, pre, post, 0
};

} // namespace x2
