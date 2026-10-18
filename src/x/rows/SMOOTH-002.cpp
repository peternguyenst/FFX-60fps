// SPDX-License-Identifier: Apache-2.0
// SMOOTH-002: actor roots (position, heading) are rebuilt once per logic tick, so characters move at
// 30 Hz. This row draws every pass of a logic period with each root a fraction of the way between the last
// two ticks: pass j of N draws (j + 1) / N (pacing_t), and the last pass draws tick k itself - which is
// exactly what SMOOTH-002-RENDERHOLD already did. At 60 fps (N = 2) that is the old pair, the logic frame at
// 0.5 and the render-only frame held at tick k. Needs SMOOTH=1.
// Details: docs/fps60/rows/SMOOTH-002.md
#include "smooth.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

typedef void(__cdecl* drawmain_t)(void);

static const int      MAXA = 512;
static const uint32_t STRIDE = 0x880, ROOT = 0x1D0, FLAGS = 0x194, PARENT = 0x19C;

static drawmain_t g_draw = nullptr;
static uint8_t* volatile* g_table = nullptr;  // pointer to the actor array
static volatile int32_t*  g_count = nullptr;  // actor count
static int   g_nocloth = 0, g_diag = 0;

struct ActHist {
    float    m[2][16];   // [0] = tick k-1, [1] = tick k
    int      n;          // valid samples (0..2)
    uint16_t id;
    float    step[2];    // translation step [0] = k-2 -> k-1, [1] = k-1 -> k (valid when n == 2; [0] when ns >= 2)
    int      ns;         // valid steps (0..2)
};
static ActHist* g_h = nullptr;             // MAXA entries
static uint8_t* g_hist_base = nullptr;     // actor array the history belongs to
static int   g_kind = INV_STOCK;
static bool  g_applied_inv = false;
static int   g_bis = 0;                     // dev: Ctrl+Num 2 stub bisect
// SMOOTH-002-SCRUB (default 1, 17/10/2026): Ch_DrawMain reads at least one stack word it never set (S: the specular
// camera vector's w at [EBP-0x3C8], 0x82FBE1, used at 0x82FC0F/0x82FC59 before its write at 0x82FC9C; there may be
// more). This stub runs Ch_DrawMain one frame deeper than stock, and on logic frames the interpolation leaves its own
// values there, so render-only and logic frames read different leftovers: the save sphere ring changed brightness every
// frame (U 17/10/2026: only with SMOOTH-002 and SMOOTH-003 live, only with the logic-frame apply (Ctrl+Num 2 bisect),
// although no root was written; Ctrl+Num 3 zeroing 16 KB of stack before the call fixed it). The stub zeroes 16 KB
// below its frame before every Ch_DrawMain call, so every frame reads the same leftovers. Ctrl+Num 3 toggles it (dev).
static int   g_scrub = 1;
__declspec(noinline) static void scrub_stack() {
    volatile uint8_t buf[0x4000];
    for (int i = 0; i < (int)sizeof(buf); i += 4) *(volatile uint32_t*)&buf[i] = 0;
}
static bool  g_recorded_inv = false;       // history rotated in this invocation (at the draw)
static int   g_childrel = 0;
static int   g_warp = 1, g_warp_lines = 0;
static float g_warp_min = 30.0f, g_warp_ratio = 3.0f;
static LONG  g_warps = 0;
// SMOOTH-002-RENDERHOLD (default 1): on a render-only invocation an actor whose root is no longer its tick-k root was
// moved outside a logic tick; it is drawn at tick k. U 12/10/2026: Valefor stuttered in Energy Ray (magic_0150, MAG-021
// family E). D SMOOTH-002-TRACE: render-only draws showed tick k+1 (the MAG-021 window replays the DLL update, which
// writes the actor root in the EXE actor table; only the DLL .data is restored), the next logic draw k+0.5: back and
// forth every frame.
static int   g_renderhold = 1, g_rh_lines = 0;
static LONG  g_rh_held = 0;
#ifdef FPS60_DEV
// SMOOTH-002-TRACE-ID=<actor id, decimal> (dev): every draw, the drawn root position of that actor, the invocation kind
// and whether this row moved it (U 12/10/2026: Valefor's Energy Ray stutters; check the drawn steps are even).
static int   g_trace_id = 0;
static LONG  g_trace_n = 0, g_trace_max = 4000;
#endif

static float tdist(const float* a, const float* b) {
    float dx = a[12] - b[12], dy = a[13] - b[13], dz = a[14] - b[14];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
// Deferred restore
static uint8_t* g_w_base = nullptr;
static int   g_w_n = 0;
static int*  g_w_idx = nullptr;            // MAXA
static int*  g_slot_w = nullptr;           // MAXA: slot -> write index this frame, -1 = not written
static float (*g_w_orig)[16] = nullptr;    // MAXA
static float (*g_w_out)[16] = nullptr;     // MAXA
static SmoothStats g_st;
static LONG  g_lwindows = 0, g_child = 0, g_skip_hist = 0, g_skip_moved = 0;

static inline bool is_child(const uint8_t* a) { return *(const uint32_t*)(a + PARENT) != 0; }
static inline bool is_cloth(const uint8_t* a) { return (*(const uint32_t*)(a + FLAGS) & 0x600000) == 0x600000; }

static void write_slot(int i, float* root, const float* out) {
    int w = g_w_n++;
    g_w_idx[w] = i;
    g_slot_w[i] = w;
    memcpy(g_w_orig[w], root, 64);
    memcpy(g_w_out[w], out, 64);
    memcpy(root, out, 64);
}

static bool begin(uint8_t** pbase, int* pn) {
    uint8_t* base = *g_table;
    int n = *g_count;
    if (!base || n <= 0 || base != g_hist_base) return false;
    if (n > MAXA) n = MAXA;
    g_w_base = base;
    g_w_n = 0;
    for (int i = 0; i < n; i++) g_slot_w[i] = -1;
    *pbase = base;
    *pn = n;
    return true;
}

// SMOOTH-002-CHILDREL: an attached actor (weapon, shield) follows its parent's drawn root instead of its own
// history: child' = child . inv(P_tick) . P_drawn, so it stays in the drawn hand.
static void apply_children(uint8_t* base, int n) {
    if (!g_childrel) return;
    for (int pass = 0; pass < 4; pass++) {
        int progress = 0;
        for (int i = 0; i < n; i++) {
            uint8_t* a = base + i * STRIDE;
            if (a[2] == 0 || g_slot_w[i] >= 0 || !is_child(a)) continue;
            if (g_nocloth && is_cloth(a)) continue;
            uintptr_t p = *(uint32_t*)(a + PARENT);
            if (p < (uintptr_t)base || ((p - (uintptr_t)base) % STRIDE) != 0) continue;
            int pi = (int)((p - (uintptr_t)base) / STRIDE);
            if (pi >= n || pi == i || g_slot_w[pi] < 0) continue;
            int pw = g_slot_w[pi];
            float inv[16], tmp[16], out[16];
            float* root = (float*)(a + ROOT);
            if (!smooth_affine_inverse(g_w_orig[pw], inv)) continue;
            smooth_mul(root, inv, tmp);
            smooth_mul(tmp, g_w_out[pw], out);
            bool fin = true;
            for (int k = 0; k < 16; k++) if (!isfinite(out[k])) fin = false;
            if (!fin || memcmp(out, root, 64) == 0) continue;
            write_slot(i, root, out);
            InterlockedIncrement(&g_child);
            progress++;
        }
        if (!progress) break;
    }
}

static void record();

// Logic/backlog invocation, lerp(tick k-1, tick k, t) with t = 1/N. The history is
// rotated first from the canonical roots (fresh tick k).
static void apply_interp(float t) {
    record();
    g_recorded_inv = true;
    uint8_t* base;
    int n;
    if (!begin(&base, &n)) return;
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * STRIDE;
        if (a[2] == 0) continue;
        if (g_childrel && is_child(a)) continue;
        ActHist& h = g_h[i];
        if (h.n < 2 || h.id != *(uint16_t*)a) { g_skip_hist++; continue; }
        if (g_nocloth && is_cloth(a)) continue;
        float* root = (float*)(a + ROOT);
        if (memcmp(root, h.m[1], 64) != 0) { g_skip_moved++; continue; }
        if (g_warp) {
            float cur = h.step[1], prev = h.ns >= 2 ? h.step[0] : 0.0f;
            if (cur > g_warp_min && cur > g_warp_ratio * prev) {
                InterlockedIncrement(&g_warps);
                if (g_warp_lines < 50) {
                    g_warp_lines++;
                    log_line("SMOOTH-002: warp, no blend: slot %d id 0x%04X step %.1f (previous %.1f%s) pos (%.1f, %.1f, %.1f)",
                             i, h.id, cur, prev, h.ns >= 2 ? "" : ", none", root[12], root[13], root[14]);
                }
                continue;
            }
        }
        float out[16];
        if (!smooth_interp(h.m[0], h.m[1], t, SMOOTH_CUT_DIST, out)) { InterlockedIncrement(&g_st.interp_cut); continue; }
        if (memcmp(out, root, 64) == 0) continue;
        write_slot(i, root, out);
        InterlockedIncrement(&g_st.interp);
    }
    apply_children(base, n);
}

#ifdef FPS60_DEV
static void trace_draw(bool applied) {
    uint8_t* base = *g_table;
    int n = *g_count;
    if (!base || n <= 0) return;
    if (n > MAXA) n = MAXA;
    for (int i = 0; i < n && g_trace_n < g_trace_max; i++) {
        uint8_t* a = base + i * STRIDE;
        if (a[2] == 0 || *(uint16_t*)a != (uint16_t)g_trace_id) continue;
        const float* r = (const float*)(a + ROOT);
        bool moved = false;
        if (applied) for (int w = 0; w < g_w_n; w++) if (g_w_idx[w] == i) { moved = true; break; }
        const ActHist& h = g_h[i];
        InterlockedIncrement(&g_trace_n);
        log_line("SMOOTH-002-TRACE: %s slot %d id 0x%04X drawn (%.2f, %.2f, %.2f) %s | tick k (%.2f, %.2f, %.2f) step %.2f | "
                 "r0 %08X %08X %08X r1 %08X %08X %08X r2 %08X %08X %08X flags %08X",
                 g_kind == INV_LOGIC ? "LOGIC " : g_kind == INV_BUILD ? "RENDER" : g_kind == INV_BACKLOG ? "BACKLG" : "STOCK ",
                 i, g_trace_id, r[12], r[13], r[14], moved ? "interp" : "as-is ",
                 h.n ? h.m[1][12] : 0.0f, h.n ? h.m[1][13] : 0.0f, h.n ? h.m[1][14] : 0.0f, h.step[1],
                 ((const uint32_t*)r)[0], ((const uint32_t*)r)[1], ((const uint32_t*)r)[2], ((const uint32_t*)r)[4],
                 ((const uint32_t*)r)[5], ((const uint32_t*)r)[6], ((const uint32_t*)r)[8], ((const uint32_t*)r)[9],
                 ((const uint32_t*)r)[10], *(const uint32_t*)(a + FLAGS));
    }
}
#endif

// Render-only invocation: roots that left tick k since the last logic draw go back to tick k (deferred restore at pre).
static void apply_render_hold() {
    uint8_t* base;
    int n;
    if (!begin(&base, &n)) return;
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * STRIDE;
        if (a[2] == 0) continue;
        if (g_childrel && is_child(a)) continue;
        ActHist& h = g_h[i];
        if (h.n < 1 || h.id != *(uint16_t*)a) continue;
        float* root = (float*)(a + ROOT);
        if (memcmp(root, h.m[1], 64) == 0) continue;
        if (g_rh_lines < 20) {
            g_rh_lines++;
            log_line("SMOOTH-002-RENDERHOLD: slot %d id 0x%04X moved on a render-only invocation (%.2f, %.2f, %.2f) -> drawn at tick k (%.2f, %.2f, %.2f)",
                     i, h.id, root[12], root[13], root[14], h.m[1][12], h.m[1][13], h.m[1][14]);
        }
        write_slot(i, root, h.m[1]);
        InterlockedIncrement(&g_rh_held);
    }
    apply_children(base, n);
}

// Render-only pass j of the period: the roots are still at tick k, so draw them t = (j+1)/N of the way from
// tick k-1. A root that has left tick k since the logic draw was moved outside a logic tick (MAG-021 replays a
// magic DLL's update, which writes the EXE actor table): it is drawn on the same curve, which is what
// SMOOTH-002-RENDERHOLD does at t = 1. A warp is never blended; it is only pinned back to tick k.
static void apply_render(float t) {
    if (t >= 1.0f) { apply_render_hold(); return; }
    uint8_t* base;
    int n;
    if (!begin(&base, &n)) return;
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * STRIDE;
        if (a[2] == 0) continue;
        if (g_childrel && is_child(a)) continue;
        ActHist& h = g_h[i];
        if (h.n < 2 || h.id != *(uint16_t*)a) { g_skip_hist++; continue; }
        if (g_nocloth && is_cloth(a)) continue;
        float* root = (float*)(a + ROOT);
        bool moved = memcmp(root, h.m[1], 64) != 0;
        if (moved && !g_renderhold) { g_skip_moved++; continue; }
        float out[16];
        bool warp = false;
        if (g_warp) {
            float cur = h.step[1], prev = h.ns >= 2 ? h.step[0] : 0.0f;
            warp = cur > g_warp_min && cur > g_warp_ratio * prev;
        }
        if (warp) {
            if (!moved) continue;                            // nothing to blend and nothing to pin back
            memcpy(out, h.m[1], 64);
        } else if (!smooth_interp(h.m[0], h.m[1], t, SMOOTH_CUT_DIST, out)) {
            InterlockedIncrement(&g_st.interp_cut);
            continue;
        }
        if (memcmp(out, root, 64) == 0) continue;
        if (moved) {
            if (g_rh_lines < 20) {
                g_rh_lines++;
                log_line("SMOOTH-002-RENDERHOLD: slot %d id 0x%04X moved on a render-only invocation (%.2f, %.2f, %.2f)"
                         " -> drawn on the tick curve at t=%.2f (%.2f, %.2f, %.2f)", i, h.id, root[12], root[13],
                         root[14], t, out[12], out[13], out[14]);
            }
            InterlockedIncrement(&g_rh_held);
        }
        write_slot(i, root, out);
        InterlockedIncrement(&g_st.interp);
    }
    apply_children(base, n);
}

// SMOOTH-002-DIAG: every 600 logic windows, one line per active actor (slot, id, flags, parent, position, translation
// and rotation step of the last tick; used to find the save sphere actor, 17/10/2026).
static void diag_actors() {
    uint8_t* base;
    int n;
    if (!begin(&base, &n)) return;
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * STRIDE;
        if (a[2] == 0) continue;
        ActHist& h = g_h[i];
        float deg = -1.0f, d = -1.0f;
        if (h.n >= 2) {
            d = tdist(h.m[0], h.m[1]);
            float tr = 0.0f;                                         // angle from the trace of R0^T R1 (row-normalised)
            float la[3], lb[3];
            for (int r = 0; r < 3; r++) {
                la[r] = sqrtf(h.m[0][r*4]*h.m[0][r*4] + h.m[0][r*4+1]*h.m[0][r*4+1] + h.m[0][r*4+2]*h.m[0][r*4+2]);
                lb[r] = sqrtf(h.m[1][r*4]*h.m[1][r*4] + h.m[1][r*4+1]*h.m[1][r*4+1] + h.m[1][r*4+2]*h.m[1][r*4+2]);
            }
            for (int r = 0; r < 3; r++)
                if (la[r] > 1e-6f && lb[r] > 1e-6f)
                    tr += (h.m[0][r*4]*h.m[1][r*4] + h.m[0][r*4+1]*h.m[1][r*4+1] + h.m[0][r*4+2]*h.m[1][r*4+2]) / (la[r] * lb[r]);
            float c = (tr - 1.0f) * 0.5f;
            if (c > 1.0f) c = 1.0f;
            if (c < -1.0f) c = -1.0f;
            deg = acosf(c) * 57.29578f;
        }
        const float* r = (const float*)(a + ROOT);
        log_line("SMOOTH-002-DIAG: slot %d id 0x%04X flags %08X parent %08X pos (%.1f, %.1f, %.1f) step %.2f units %.2f deg scale %.3f",
                 i, *(uint16_t*)a, *(uint32_t*)(a + FLAGS), *(uint32_t*)(a + PARENT), r[12], r[13], r[14], d, deg,
                 sqrtf(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]));
    }
}

static void __cdecl h_draw() {
    bool applied = false;
    float t = pacing_t();
    static LONG dumped = -1;
    if (g_diag && (g_kind == INV_LOGIC) && g_lwindows % 600 == 300 && dumped != g_lwindows) {
        dumped = g_lwindows;
        __try { diag_actors(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
#ifdef FPS60_DEV
    // Dev hotkey Ctrl+Num 2 (U 17/10/2026: save sphere ring flickers only with SMOOTH-002 and SMOOTH-003 both live,
    // while SMOOTH-002 writes no root): bisect this stub. 0 normal, 1 no logic-frame apply, 2 no render-only hold,
    // 3 neither (pass-through).
    static bool prevr = false;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && dev_key(VK_NUMPAD2, &prevr)) {
        g_bis = (g_bis + 1) % 4;
        log_line("Ctrl+Num2: SMOOTH-002 stub bisect %d (%s)", g_bis,
                 g_bis == 0 ? "normal" : g_bis == 1 ? "no logic apply" : g_bis == 2 ? "no render hold" : "pass-through");
    }
    static bool prevs = false;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && dev_key(VK_NUMPAD3, &prevs)) {
        g_scrub = !g_scrub;
        log_line("Ctrl+Num3: SMOOTH-002 stack scrub before Ch_DrawMain %s", g_scrub ? "ON" : "OFF");
    }
#endif
    if (row_SMOOTH002.on && !g_disabled && !g_applied_inv && smooth_live(2) && !(g_bis & 1) &&
        (g_kind == INV_LOGIC || g_kind == INV_BACKLOG)) {
        g_applied_inv = true;
        g_lwindows++;
        applied = true;
        __try { apply_interp(t); }
        __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-002 apply", GetExceptionCode()); }
    // Before the last pass of a period there is a frame to draw between the ticks whether or not RENDERHOLD
    // is on; at t = 1 the branch is RENDERHOLD's own, so it keeps that switch.
    } else if (row_SMOOTH002.on && !g_disabled && !g_applied_inv && smooth_live(2) && !(g_bis & 2) &&
               g_kind == INV_BUILD && (g_renderhold || t < 1.0f)) {
        g_applied_inv = true;
        applied = true;
        __try { apply_render(t); }
        __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-002 render pass", GetExceptionCode()); }
    }
#ifdef FPS60_DEV
    if (g_trace_id && g_trace_n < g_trace_max) {
        __try { trace_draw(applied); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
#endif
    if (g_scrub) scrub_stack();
    g_draw();
}

static void restore() {
    if (g_w_n == 0) return;
    if (*g_table == g_w_base) {
        int n = *g_count;
        for (int w = 0; w < g_w_n; w++) {
            int i = g_w_idx[w];
            if (i >= n) continue;
            float* root = (float*)(g_w_base + i * STRIDE + ROOT);
            if (memcmp(root, g_w_out[w], 64) == 0) memcpy(root, g_w_orig[w], 64);
        }
    }
    g_w_n = 0;
}

static void pre() {
    smooth_poll_hotkey();
    __try { restore(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-002 restore", GetExceptionCode()); }
    g_kind = g_disabled ? INV_STOCK : pacing_kind();
    g_applied_inv = false;
    g_recorded_inv = false;
    if (g_kind == INV_STOCK) g_hist_base = nullptr;   // history restarts after menus / FMV hold
}

static void record() {
    uint8_t* base = *g_table;
    int n = *g_count;
    if (!base || n <= 0) { g_hist_base = nullptr; return; }
    if (n > MAXA) n = MAXA;
    if (base != g_hist_base) {
        for (int i = 0; i < MAXA; i++) g_h[i].n = 0;
        g_hist_base = base;
    }
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * STRIDE;
        ActHist& h = g_h[i];
        uint16_t id = *(uint16_t*)a;
        if (a[2] == 0) { h.n = 0; continue; }
        if (h.n > 0 && h.id != id) h.n = 0;
        if (h.n == 0) h.ns = 0;
        memcpy(h.m[0], h.m[1], 64);
        memcpy(h.m[1], a + ROOT, 64);
        if (h.n >= 1) {                              // a step exists once there is a previous sample
            h.step[0] = h.step[1];
            h.step[1] = tdist(h.m[0], h.m[1]);
            if (h.ns < 2) h.ns++;
        }
        h.id = id;
        if (h.n < 2) h.n++;
    }
    for (int i = n; i < MAXA; i++) g_h[i].n = 0;
}

static void post() {
    if ((g_kind == INV_LOGIC || g_kind == INV_BACKLOG) && !g_recorded_inv) {
        __try { record(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-002 record", GetExceptionCode()); }
    }
    static LONG last = 0;
    if (g_diag && g_lwindows - last >= 600) {
        last = g_lwindows;
        log_line("SMOOTH-002: logic windows=%ld interp=%ld interp_cut=%ld warps=%ld renderhold=%ld skip(hist=%ld moved=%ld) | children=%ld actors=%d live=%d",
                 g_lwindows, g_st.interp, g_st.interp_cut, g_warps, g_rh_held, g_skip_hist, g_skip_moved, g_child, *g_count, smooth_live(2));
    }
    static LONG reported = 0;
    if (!reported && g_st.interp > 0) {
        reported = 1;
        log_line("SMOOTH-002: first interpolated actor root on a logic invocation");
    }
}

static bool install() {
    if (!smooth_master()) { log_line("  SMOOTH-002: master switch SMOOTH=0 - off, nothing scanned or patched"); return false; }
    // Sg_MainLoop draw: the two Ch_DrawMain CALLs
    uint8_t* s = sig_find_unique("SMOOTH-002 Ch_DrawMain calls (Sg_MainLoop draw)",
        "E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 74 04 85 F6 74 05 E8 ?? ?? ?? ?? 6A 02 EB 51 80 3D ?? ?? ?? ?? 00 74 2F "
        "80 3D ?? ?? ?? ?? 00 74 26 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 74 04 85 F6 74 05 E8", 0x8212E6);
    // Ch_DrawMain: loads the actor count and table, stride 0x880
    uint8_t* t = sig_find_unique("SMOOTH-002 actor table (Ch_DrawMain)",
        "DD 05 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 35 ?? ?? ?? ?? DC C9 69 C0 80 08 00 00", 0x82EE11);
    // Ch_DrawMain first loop: submits each actor's root (actor+0x1D0) to the draw
    uint8_t* r = sig_find_unique("SMOOTH-002 root submit (Ch_DrawMain)",
        "8D 85 7C FF FF FF 50 8D 86 D0 01 00 00 50 8D 85 FC FE FF FF", 0x82EF7E);
    if (!s || !t || !r) return false;
    uint8_t* sites[2] = { s + 0x12, s + 0x3F };
    uint8_t* fn = call_target(sites[0]);
    if (!fn || call_target(sites[1]) != fn || t < fn || t > fn + 0x200 || r < fn || r > fn + 0x400) {
        log_line("  SMOOTH-002: Ch_DrawMain cross-check failed (fn %p, table sig %p, root sig %p) - refused", fn, t, r);
        return false;
    }
    g_count = (volatile int32_t*)read_abs(t + 7);
    g_table = (uint8_t* volatile*)read_abs(t + 0xD);
    uint8_t* st = sym_rt("actor_table");
    uint8_t* sc = sym_rt("actor_count");
    if ((st && st != (uint8_t*)g_table) || (sc && sc != (uint8_t*)g_count)) {
        log_line("  SMOOTH-002: actor table differs from the scan symbols - refused");
        return false;
    }
    g_h = (ActHist*)calloc(MAXA, sizeof(ActHist));
    g_w_idx = (int*)calloc(MAXA, sizeof(int));
    g_slot_w = (int*)calloc(MAXA, sizeof(int));
    g_w_orig = (float(*)[16])calloc(MAXA, 64);
    g_w_out = (float(*)[16])calloc(MAXA, 64);
    if (!g_h || !g_w_idx || !g_slot_w || !g_w_orig || !g_w_out) { log_line("  SMOOTH-002: out of memory - refused"); return false; }
    g_draw = (drawmain_t)fn;
    g_nocloth = config_row("SMOOTH-002-NOCLOTH", 0);
    g_diag = config_row("SMOOTH-002-DIAG", 0);
    g_childrel = config_row("SMOOTH-002-CHILDREL", 1);
    g_warp = config_row("SMOOTH-002-WARP", 1);
    g_warp_min = (float)config_row("SMOOTH-002-WARP-MIN", 30);
    g_warp_ratio = (float)config_row("SMOOTH-002-WARP-RATIO", 3);
    g_renderhold = config_row("SMOOTH-002-RENDERHOLD", 1);
    g_scrub = config_row("SMOOTH-002-SCRUB", 1);
#ifdef FPS60_DEV
    g_trace_id = config_row("SMOOTH-002-TRACE-ID", 0);
    g_trace_max = config_row("SMOOTH-002-TRACE-N", 4000);
    if (g_trace_id) log_line("  SMOOTH-002-TRACE: actor id 0x%04X, up to %ld draws logged", g_trace_id, g_trace_max);
#endif
    for (int i = 0; i < 2; i++) {
        if (!patch_call_rel32(sites[i], fn, (void*)&h_draw)) {
            if (i == 1) patch_call_rel32(sites[0], (void*)&h_draw, fn);
            log_line("  SMOOTH-002: patch %d failed - refused", i);
            return false;
        }
    }
    log_line("  SMOOTH-002: Ch_DrawMain gva=0x%08X (expected 0x0082ED50), CALLs gva=0x%08X/0x%08X -> stub; actors gva=0x%08X "
             "(0x023C44E4) count gva=0x%08X (0x023C44E0) stride 0x880 root +0x1D0; interpolation; nocloth=%d childrel=%d diag=%d; "
             "warp guard %d (min %.0f, ratio %.0f); renderhold %d; F7 toggles",
             rt_to_gva_exe(fn), rt_to_gva_exe(sites[0]), rt_to_gva_exe(sites[1]), rt_to_gva_exe((const void*)g_table),
             rt_to_gva_exe((const void*)g_count), g_nocloth, g_childrel, g_diag, g_warp, g_warp_min, g_warp_ratio, g_renderhold);
    log_line("  SMOOTH-002: stack scrub before Ch_DrawMain %s (SMOOTH-002-SCRUB)", g_scrub ? "ON" : "OFF");
    return true;
}

Row row_SMOOTH002 = {
    "SMOOTH-002",
    "smooth actor roots: each pass draws a root a fraction of the way between the last two ticks (needs master SMOOTH=1)",
    1, install, pre, post, 0
};
