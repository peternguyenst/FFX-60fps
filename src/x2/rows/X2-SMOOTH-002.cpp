// SPDX-License-Identifier: Apache-2.0
// X2-SMOOTH-002 (port of FFX SMOOTH-002): actor roots (position, heading) are rebuilt once per logic tick, so
// characters move at 30 Hz. Logic invocations draw each root half-way between the last two ticks (needs X2-SMOOTH=1;
// F7 toggles). Sub-switches as FFX: WARP, RENDERHOLD, CHILDREL (default 1), NOCLOTH (default 0).
// FFX-2 layout (PORT-SMOOTH.md §4-§6, S): Ch_DrawMain 0x6DBA00 (cdecl void, 2 CALLs), actor table 0x01CC9604 / count
// 0x01CC9600, stride 0x910, root +0x1DC, flags +0x1A0, parent +0x1A8 (child root = local x parent root, 0x6E0C50).
#include "x2_smooth.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

namespace x2 {

typedef void(__cdecl* drawmain_t)(void);

static const int      MAXA = 512;
static const uint32_t STRIDE = 0x910, ROOT = 0x1DC, FLAGS = 0x1A0, PARENT = 0x1A8;

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
extern Row row_SMOOTH002;
static ActHist* g_h = nullptr;             // MAXA entries
static uint8_t* g_hist_base = nullptr;     // actor array the history belongs to
static int   g_kind = INV_STOCK;
static bool  g_applied_inv = false;
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

// Logic/backlog invocation, lerp(tick k-1, tick k, 0.5). The history is
// rotated first from the canonical roots (fresh tick k).
static void apply_interp() {
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
                    log_line("X2-SMOOTH-002: warp, no blend: slot %d id 0x%04X step %.1f (previous %.1f%s) pos (%.1f, %.1f, %.1f)",
                             i, h.id, cur, prev, h.ns >= 2 ? "" : ", none", root[12], root[13], root[14]);
                }
                continue;
            }
        }
        float out[16];
        if (!smooth_interp(h.m[0], h.m[1], 0.5f, SMOOTH_CUT_DIST, out)) { InterlockedIncrement(&g_st.interp_cut); continue; }
        if (memcmp(out, root, 64) == 0) continue;
        write_slot(i, root, out);
        InterlockedIncrement(&g_st.interp);
    }
    apply_children(base, n);
}


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
            log_line("X2-SMOOTH-002-RENDERHOLD: slot %d id 0x%04X moved on a render-only invocation (%.2f, %.2f, %.2f) -> drawn at tick k (%.2f, %.2f, %.2f)",
                     i, h.id, root[12], root[13], root[14], h.m[1][12], h.m[1][13], h.m[1][14]);
        }
        write_slot(i, root, h.m[1]);
        InterlockedIncrement(&g_rh_held);
    }
    apply_children(base, n);
}

// F9 trace (with X2-SMOOTH-001's camera trace): for 240 invocations, the first 3 active non-child actors' drawn root
// position, tick-k root position and whether this row wrote the root. U 13/10/2026: walking left/right shows a ghost
// character, gone with X2-SMOOTH-002 off (F6); static RE found the body draw reads the root as expected
// (re/ledger/SMOOTH2-GHOST.md), so the drawn sequence decides.
static LONG g_trace_left = 0;
static bool g_f9_prev = false;
static bool g_traced_inv = false;

static void trace_roots() {
    uint8_t* base = *g_table;
    int n = *g_count;
    if (!base || n <= 0) return;
    if (n > MAXA) n = MAXA;
    char line[700];
    int k = _snprintf_s(line, sizeof(line), _TRUNCATE, "X2-SMOOTH-002 trace: %c", "LRBS"[g_kind & 3]);
    int shown = 0;
    for (int i = 0; i < n && shown < 3; i++) {
        uint8_t* a = base + i * STRIDE;
        if (a[2] == 0 || is_child(a)) continue;
        const float* r = (const float*)(a + ROOT);
        bool w = g_slot_w[i] >= 0 && g_w_n > 0;
        const ActHist& h = g_h[i];
        k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " | s%d id%04X %s drawn (%.2f %.2f %.2f) tickk (%.2f %.2f %.2f)",
                         i, *(uint16_t*)a, w ? "ours" : "game", r[12], r[13], r[14],
                         h.n ? h.m[1][12] : 0.f, h.n ? h.m[1][13] : 0.f, h.n ? h.m[1][14] : 0.f);
        shown++;
    }
    log_line("%s", line);
}

static void __cdecl h_draw() {
    bool applied = false;
    if (row_SMOOTH002.on && !g_disabled && !g_applied_inv && smooth_live(2) &&
        (g_kind == INV_LOGIC || g_kind == INV_BACKLOG)) {
        g_applied_inv = true;
        g_lwindows++;
        applied = true;
        __try { apply_interp(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_line("X2-SMOOTH-002: fault 0x%08lX in apply - row off", GetExceptionCode()); row_SMOOTH002.on = 0; }
    } else if (g_renderhold && row_SMOOTH002.on && !g_disabled && !g_applied_inv && smooth_live(2) && g_kind == INV_RENDER_ONLY) {
        g_applied_inv = true;
        applied = true;
        __try { apply_render_hold(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_line("X2-SMOOTH-002: fault 0x%08lX in render hold - row off", GetExceptionCode()); row_SMOOTH002.on = 0; }
    }
    if (g_trace_left > 0 && !g_traced_inv) {
        g_traced_inv = true;
        __try { trace_roots(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (--g_trace_left == 0) log_line("X2-SMOOTH-002 trace: end");
    }
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

// Dev hot reload: actor roots written for a render-only frame go back (pre would restore them next).
static void stop() {
    __try { restore(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void pre() {
    smooth_poll_hotkey();
    g_traced_inv = false;
    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !g_f9_prev && g_trace_left <= 0) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) { g_trace_left = 240; log_line("F9: X2-SMOOTH-002 trace: next 240 frames"); }
    }
    g_f9_prev = f9;
    __try { restore(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { log_line("X2-SMOOTH-002: fault 0x%08lX in restore", GetExceptionCode()); }
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
        __except (EXCEPTION_EXECUTE_HANDLER) { log_line("X2-SMOOTH-002: fault 0x%08lX in record", GetExceptionCode()); }
    }
    static LONG last = 0;
    if (g_diag && g_lwindows - last >= 600) {
        last = g_lwindows;
        log_line("X2-SMOOTH-002: logic windows=%ld interp=%ld interp_cut=%ld warps=%ld renderhold=%ld skip(hist=%ld moved=%ld) | children=%ld actors=%d live=%d",
                 g_lwindows, g_st.interp, g_st.interp_cut, g_warps, g_rh_held, g_skip_hist, g_skip_moved, g_child, *g_count, smooth_live(2));
    }
    static LONG reported = 0;
    if (!reported && g_st.interp > 0) {
        reported = 1;
        log_line("X2-SMOOTH-002: first interpolated actor root on a logic invocation");
    }
}

static bool install() {
    if (!smooth_master()) { log_line("  X2-SMOOTH-002: master X2-SMOOTH=0 - off"); return false; }
    // Sg_MainLoop draw: CALL FUN_006d36a0; CALL Ch_DrawMain ... (second pair at +0x2F)
    uint8_t* s = sig_find("X2-SMOOTH-002 Ch_DrawMain calls",
        "E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 01 05 ?? ?? ?? ?? 01 05 ?? ?? ?? ?? EB 60 80 3D ?? ?? ?? ?? 00 74 2F "
        "80 3D ?? ?? ?? ?? 00 74 26 E8 ?? ?? ?? ?? E8", 0x605B34);
    // Ch_DrawMain: MOV ECX,[count]; MOV ESI,[table]; IMUL ECX,ECX,0x910
    uint8_t* t = sig_find("X2-SMOOTH-002 actor table", "8B 0D ?? ?? ?? ?? 8B 35 ?? ?? ?? ?? 69 C9 10 09 00 00 03 CE", 0x6DBAA5);
    // Ch_DrawMain first loop: submits actor+0x1DC (root) to the draw
    uint8_t* r = sig_find("X2-SMOOTH-002 root submit", "8D 85 7C FF FF FF 50 8D 86 DC 01 00 00 50 8D 85 FC FE FF FF 50", 0x6DBDF5);
    // Ch_BuildRootAndBones: child root = local x [actor+0x1A8]+0x1DC
    uint8_t* c = sig_find("X2-SMOOTH-002 child root from parent", "8B 86 A8 01 00 00 D9 85 ?? ?? ?? ?? 05 DC 01 00 00", 0x6E0C50);
    if (!s || !t || !r || !c) return false;
    uint8_t* sites[2] = { s + 0x05, s + 0x34 };
    uint8_t* fn = call_target(sites[0]);
    if (!fn || call_target(sites[1]) != fn || t < fn || t > fn + 0x200 || r < fn || r > fn + 0x600) {
        log_line("  X2-SMOOTH-002: Ch_DrawMain cross-check failed (fn %p, table sig %p, root sig %p) - refused", fn, t, r);
        return false;
    }
    g_count = (volatile int32_t*)abs_operand("actor count", t + 2);
    g_table = (uint8_t* volatile*)abs_operand("actor table", t + 8);
    if (!g_count || !g_table) return false;
    g_h = (ActHist*)calloc(MAXA, sizeof(ActHist));
    g_w_idx = (int*)calloc(MAXA, sizeof(int));
    g_slot_w = (int*)calloc(MAXA, sizeof(int));
    g_w_orig = (float(*)[16])calloc(MAXA, 64);
    g_w_out = (float(*)[16])calloc(MAXA, 64);
    if (!g_h || !g_w_idx || !g_slot_w || !g_w_orig || !g_w_out) { log_line("  X2-SMOOTH-002: out of memory - refused"); return false; }
    g_draw = (drawmain_t)fn;
    g_nocloth = config_row("X2-SMOOTH-002-NOCLOTH", 0);
    g_diag = config_row("X2-SMOOTH-002-DIAG", 1);
    g_childrel = config_row("X2-SMOOTH-002-CHILDREL", 1);
    g_warp = config_row("X2-SMOOTH-002-WARP", 1);
    g_warp_min = (float)config_row("X2-SMOOTH-002-WARP-MIN", 30);
    g_warp_ratio = (float)config_row("X2-SMOOTH-002-WARP-RATIO", 3);
    g_renderhold = config_row("X2-SMOOTH-002-RENDERHOLD", 1);
    for (int i = 0; i < 2; i++) {
        if (!patch_call_rel32(sites[i], fn, (void*)&h_draw)) {
            if (i == 1) patch_call_rel32(sites[0], (void*)&h_draw, fn);
            log_line("  X2-SMOOTH-002: patch %d failed - refused", i);
            return false;
        }
    }
    log_line("  X2-SMOOTH-002: Ch_DrawMain gva=0x%08X (expected 0x006DBA00), CALLs gva=0x%08X/0x%08X -> stub; actors gva=0x%08X "
             "(0x01CC9604) count gva=0x%08X (0x01CC9600) stride 0x910 root +0x1DC; nocloth=%d childrel=%d diag=%d; "
             "warp guard %d (min %.0f, ratio %.0f); renderhold %d; F7 toggles",
             rt_to_gva(fn), rt_to_gva(sites[0]), rt_to_gva(sites[1]), rt_to_gva((const void*)g_table),
             rt_to_gva((const void*)g_count), g_nocloth, g_childrel, g_diag, g_warp, g_warp_min, g_warp_ratio, g_renderhold);
    return true;
}

Row row_SMOOTH002 = {
    "X2-SMOOTH-002",
    "60 Hz actor roots: logic frames draw each root half-way between the last two ticks (needs X2-SMOOTH=1)",
    1, install, pre, post, 0, 0, stop
};

} // namespace x2
