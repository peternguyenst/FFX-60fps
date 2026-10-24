// SPDX-License-Identifier: Apache-2.0
// X2-SMOOTH-002: characters and monsters move at 60 fps.
//
// Each actor's "root" (its position and heading, a 4x4 matrix at actor +0x1DC) is set once per logic tick, so at
// 60 fps bodies would still move in 30 Hz steps. Just before the game draws the actors (our hook on its two calls of
// Ch_DrawMain), a logic invocation writes each root half way between its last two ticks; a render-only invocation
// draws tick k. The game's roots are put back at the next pre(), so game logic never sees ours. Same timebase as the
// camera (X2-SMOOTH-001): the drawn sequence is k-0.5, k, k+0.5, ...
// Not interpolated: an actor without two ticks of history (just appeared, slot reused), one the game moved since its
// tick (another fix may have), a cut (SMOOTH_CUT_DIST), and a warp (below).
// Switches ([rows] X2-SMOOTH-002-*, on unless noted):
//   WARP        a step much bigger than the previous one (> WARP-MIN 30 units and > WARP-RATIO 3x) is a teleport: snap
//   RENDERHOLD  on a render-only frame, an actor the game moved since tick k is drawn at tick k (a spell DLL's update
//               can move actors on render-only frames, which made them jump back and forth)
//   CHILDREL    attached actors (weapons, shields) follow their parent's drawn root, so they stay in the drawn hand
//   NOCLOTH     (off) leave cloth actors alone
//   DIAG        counters line every 600 logic draws
// F9 (with X2-SMOOTH-001): 240 frames of root traces. Needs X2-SMOOTH=1; F7 turns all SMOOTH rows off and on.
// History, measurements and evidence: re/ledger/PORT-SMOOTH.md §4-§6 (it also keeps this file's earlier comments),
// SMOOTH2-GHOST.md.
#include "x2_smooth.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

namespace x2 {

typedef void(__cdecl* draw_main_fn)(void);

// Actor array layout (re/ledger/PORT-SMOOTH.md §5): 0x910 bytes per actor, root matrix +0x1DC, flags +0x1A0,
// parent +0x1A8 (0 = not attached; an attached actor's root = its local matrix x the parent's root).
static const int      MAX_ACTORS = 512;
static const uint32_t ACTOR_SIZE = 0x910, OFF_ROOT = 0x1DC, OFF_FLAGS = 0x1A0, OFF_PARENT = 0x1A8;

static draw_main_fn g_draw_main = nullptr;          // Ch_DrawMain: draws every actor
static uint8_t* volatile* g_actor_table = nullptr;  // pointer to the actor array
static volatile int32_t*  g_actor_count = nullptr;  // actor count
static int   g_opt_nocloth = 0, g_opt_diag = 0;

// The last two tick roots of one actor slot, and how far it moved in each of the last two ticks (for WARP).
struct RootHistory {
    float    m[2][16];   // [0] = tick k-1, [1] = tick k
    int      n;          // valid samples (0..2)
    uint16_t id;         // actor id the samples belong to
    float    step[2];    // distance moved: [0] = tick k-2 -> k-1, [1] = tick k-1 -> k
    int      ns;         // valid steps (0..2)
};
extern Row row_SMOOTH002;
static RootHistory* g_history = nullptr;             // MAX_ACTORS entries
static uint8_t* g_history_table = nullptr;           // actor array the history belongs to
static int   g_inv_kind = INV_STOCK;
static bool  g_applied_now = false;                  // roots already written this invocation (Ch_DrawMain runs twice)
static bool  g_recorded_now = false;                 // history already moved on this invocation (at the draw)
static int   g_opt_childrel = 0;
static int   g_opt_warp = 1, g_warp_lines = 0;
static float g_opt_warp_min = 30.0f, g_opt_warp_ratio = 3.0f;
static LONG  g_warps = 0;
static int   g_opt_renderhold = 1, g_rh_lines = 0;
static LONG  g_rh_held = 0;

static float translation_distance(const float* a, const float* b) {
    float dx = a[12] - b[12], dy = a[13] - b[13], dz = a[14] - b[14];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
// The roots we wrote this invocation, so pre() can put the game's back: slot, the game's root and ours.
static uint8_t* g_written_table = nullptr;
static int   g_written_count = 0;
static int*  g_written_slot = nullptr;            // MAX_ACTORS
static int*  g_slot_write = nullptr;              // MAX_ACTORS: slot -> write index this frame, -1 = not written
static float (*g_written_game_root)[16] = nullptr;    // MAX_ACTORS
static float (*g_written_our_root)[16] = nullptr;     // MAX_ACTORS
static SmoothStats g_stats;
static LONG  g_logic_draws = 0, g_children_moved = 0, g_skip_hist = 0, g_skip_moved = 0;

static inline bool is_child(const uint8_t* a) { return *(const uint32_t*)(a + OFF_PARENT) != 0; }
static inline bool is_cloth_actor(const uint8_t* a) { return (*(const uint32_t*)(a + OFF_FLAGS) & 0x600000) == 0x600000; }

// --- Writing roots -----------------------------------------------------------------------------------------------------
// Draw slot i with `out` this invocation (the game's root is kept for pre()).
static void write_root(int i, float* root, const float* out) {
    int w = g_written_count++;
    g_written_slot[w] = i;
    g_slot_write[i] = w;
    memcpy(g_written_game_root[w], root, 64);
    memcpy(g_written_our_root[w], out, 64);
    memcpy(root, out, 64);
}

// Start this invocation's writes; false if there is no history for the current actor array.
static bool begin_writes(uint8_t** pbase, int* pn) {
    uint8_t* base = *g_actor_table;
    int n = *g_actor_count;
    if (!base || n <= 0 || base != g_history_table) return false;
    if (n > MAX_ACTORS) n = MAX_ACTORS;
    g_written_table = base;
    g_written_count = 0;
    for (int i = 0; i < n; i++) g_slot_write[i] = -1;
    *pbase = base;
    *pn = n;
    return true;
}

// CHILDREL: an attached actor (weapon, shield) follows its parent's drawn root instead of its own history:
// child' = child . inv(parent at tick) . parent as drawn, so it stays in the drawn hand. Up to 4 passes, for chains.
static void apply_children(uint8_t* base, int n) {
    if (!g_opt_childrel) return;
    for (int pass = 0; pass < 4; pass++) {
        int progress = 0;
        for (int i = 0; i < n; i++) {
            uint8_t* a = base + i * ACTOR_SIZE;
            if (a[2] == 0 || g_slot_write[i] >= 0 || !is_child(a)) continue;
            if (g_opt_nocloth && is_cloth_actor(a)) continue;
            uintptr_t p = *(uint32_t*)(a + OFF_PARENT);
            if (p < (uintptr_t)base || ((p - (uintptr_t)base) % ACTOR_SIZE) != 0) continue;
            int pi = (int)((p - (uintptr_t)base) / ACTOR_SIZE);
            if (pi >= n || pi == i || g_slot_write[pi] < 0) continue;
            int pw = g_slot_write[pi];
            float inv[16], tmp[16], out[16];
            float* root = (float*)(a + OFF_ROOT);
            if (!smooth_affine_inverse(g_written_game_root[pw], inv)) continue;
            smooth_mul(root, inv, tmp);
            smooth_mul(tmp, g_written_our_root[pw], out);
            bool fin = true;
            for (int k = 0; k < 16; k++) if (!isfinite(out[k])) fin = false;
            if (!fin || memcmp(out, root, 64) == 0) continue;
            write_root(i, root, out);
            InterlockedIncrement(&g_children_moved);
            progress++;
        }
        if (!progress) break;
    }
}

static void record_tick();

// Logic (or backlog) invocation: record the fresh tick k, then draw every actor half way between k-1 and k.
static void apply_interp() {
    record_tick();
    g_recorded_now = true;
    uint8_t* base;
    int n;
    if (!begin_writes(&base, &n)) return;
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * ACTOR_SIZE;
        if (a[2] == 0) continue;
        if (g_opt_childrel && is_child(a)) continue;
        RootHistory& h = g_history[i];
        if (h.n < 2 || h.id != *(uint16_t*)a) { g_skip_hist++; continue; }
        if (g_opt_nocloth && is_cloth_actor(a)) continue;
        float* root = (float*)(a + OFF_ROOT);
        if (memcmp(root, h.m[1], 64) != 0) { g_skip_moved++; continue; }
        if (g_opt_warp) {
            float cur = h.step[1], prev = h.ns >= 2 ? h.step[0] : 0.0f;
            if (cur > g_opt_warp_min && cur > g_opt_warp_ratio * prev) {
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
        if (!smooth_interp(h.m[0], h.m[1], 0.5f, SMOOTH_CUT_DIST, out)) { InterlockedIncrement(&g_stats.interp_cut); continue; }
        if (memcmp(out, root, 64) == 0) continue;
        write_root(i, root, out);
        InterlockedIncrement(&g_stats.interp);
    }
    apply_children(base, n);
}


// Render-only invocation (RENDERHOLD): roots that left tick k since the last logic draw are drawn at tick k.
static void apply_render_hold() {
    uint8_t* base;
    int n;
    if (!begin_writes(&base, &n)) return;
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * ACTOR_SIZE;
        if (a[2] == 0) continue;
        if (g_opt_childrel && is_child(a)) continue;
        RootHistory& h = g_history[i];
        if (h.n < 1 || h.id != *(uint16_t*)a) continue;
        float* root = (float*)(a + OFF_ROOT);
        if (memcmp(root, h.m[1], 64) == 0) continue;
        if (g_rh_lines < 20) {
            g_rh_lines++;
            log_line("X2-SMOOTH-002-RENDERHOLD: slot %d id 0x%04X moved on a render-only invocation (%.2f, %.2f, %.2f) -> drawn at tick k (%.2f, %.2f, %.2f)",
                     i, h.id, root[12], root[13], root[14], h.m[1][12], h.m[1][13], h.m[1][14]);
        }
        write_root(i, root, h.m[1]);
        InterlockedIncrement(&g_rh_held);
    }
    apply_children(base, n);
}

// --- F9 trace ------------------------------------------------------------------------------------------------------------
// For 240 invocations: the first 3 root actors' drawn position, their tick-k position, and whose root was drawn.
static LONG g_trace_left = 0;
static bool g_f9_was_down = false;
static bool g_traced_inv = false;

static void trace_roots() {
    uint8_t* base = *g_actor_table;
    int n = *g_actor_count;
    if (!base || n <= 0) return;
    if (n > MAX_ACTORS) n = MAX_ACTORS;
    char line[700];
    int k = _snprintf_s(line, sizeof(line), _TRUNCATE, "X2-SMOOTH-002 trace: %c", "LRBS"[g_inv_kind & 3]);
    int shown = 0;
    for (int i = 0; i < n && shown < 3; i++) {
        uint8_t* a = base + i * ACTOR_SIZE;
        if (a[2] == 0 || is_child(a)) continue;
        const float* r = (const float*)(a + OFF_ROOT);
        bool w = g_slot_write[i] >= 0 && g_written_count > 0;
        const RootHistory& h = g_history[i];
        k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " | s%d id%04X %s drawn (%.2f %.2f %.2f) tickk (%.2f %.2f %.2f)",
                         i, *(uint16_t*)a, w ? "ours" : "game", r[12], r[13], r[14],
                         h.n ? h.m[1][12] : 0.f, h.n ? h.m[1][13] : 0.f, h.n ? h.m[1][14] : 0.f);
        shown++;
    }
    log_line("%s", line);
}

// --- Hook and every invocation -------------------------------------------------------------------------------------------
// Replaces the main loop's two calls of Ch_DrawMain: write the roots once per invocation, then let the game draw.
static void __cdecl hook_draw_main() {
    bool applied = false;
    if (row_SMOOTH002.on && !g_disabled && !g_applied_now && smooth_live(2) &&
        (g_inv_kind == INV_LOGIC || g_inv_kind == INV_BACKLOG)) {
        g_applied_now = true;
        g_logic_draws++;
        applied = true;
        __try { apply_interp(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_line("X2-SMOOTH-002: fault 0x%08lX in apply - row off", GetExceptionCode()); row_SMOOTH002.on = 0; }
    } else if (g_opt_renderhold && row_SMOOTH002.on && !g_disabled && !g_applied_now && smooth_live(2) && g_inv_kind == INV_RENDER_ONLY) {
        g_applied_now = true;
        applied = true;
        __try { apply_render_hold(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_line("X2-SMOOTH-002: fault 0x%08lX in render hold - row off", GetExceptionCode()); row_SMOOTH002.on = 0; }
    }
    if (g_trace_left > 0 && !g_traced_inv) {
        g_traced_inv = true;
        __try { trace_roots(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (--g_trace_left == 0) log_line("X2-SMOOTH-002 trace: end");
    }
    g_draw_main();
}

// Put the game's roots back where ours are still in place.
static void restore_game_roots() {
    if (g_written_count == 0) return;
    if (*g_actor_table == g_written_table) {
        int n = *g_actor_count;
        for (int w = 0; w < g_written_count; w++) {
            int i = g_written_slot[w];
            if (i >= n) continue;
            float* root = (float*)(g_written_table + i * ACTOR_SIZE + OFF_ROOT);
            if (memcmp(root, g_written_our_root[w], 64) == 0) memcpy(root, g_written_game_root[w], 64);
        }
    }
    g_written_count = 0;
}

// Dev hot reload: put the game's roots back now (pre() would have done it next).
static void stop() {
    __try { restore_game_roots(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Before: F9 trace key, put the game's roots back; while pacing is stock the history is dropped.
static void pre() {
    smooth_poll_hotkey();
    g_traced_inv = false;
    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !g_f9_was_down && g_trace_left <= 0) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) { g_trace_left = 240; log_line("F9: X2-SMOOTH-002 trace: next 240 frames"); }
    }
    g_f9_was_down = f9;
    __try { restore_game_roots(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { log_line("X2-SMOOTH-002: fault 0x%08lX in restore", GetExceptionCode()); }
    g_inv_kind = g_disabled ? INV_STOCK : pacing_kind();
    g_applied_now = false;
    g_recorded_now = false;
    if (g_inv_kind == INV_STOCK) g_history_table = nullptr;   // history restarts after menus / FMV hold
}

// Shift every slot's history by one tick: tick k becomes k-1, the game's current root becomes tick k.
static void record_tick() {
    uint8_t* base = *g_actor_table;
    int n = *g_actor_count;
    if (!base || n <= 0) { g_history_table = nullptr; return; }
    if (n > MAX_ACTORS) n = MAX_ACTORS;
    if (base != g_history_table) {
        for (int i = 0; i < MAX_ACTORS; i++) g_history[i].n = 0;
        g_history_table = base;
    }
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * ACTOR_SIZE;
        RootHistory& h = g_history[i];
        uint16_t id = *(uint16_t*)a;
        if (a[2] == 0) { h.n = 0; continue; }
        if (h.n > 0 && h.id != id) h.n = 0;
        if (h.n == 0) h.ns = 0;
        memcpy(h.m[0], h.m[1], 64);
        memcpy(h.m[1], a + OFF_ROOT, 64);
        if (h.n >= 1) {                              // a step exists once there is a previous sample
            h.step[0] = h.step[1];
            h.step[1] = translation_distance(h.m[0], h.m[1]);
            if (h.ns < 2) h.ns++;
        }
        h.id = id;
        if (h.n < 2) h.n++;
    }
    for (int i = n; i < MAX_ACTORS; i++) g_history[i].n = 0;
}

// After a logic invocation whose draw did not run: record the tick anyway, so the history stays continuous.
static void post() {
    if ((g_inv_kind == INV_LOGIC || g_inv_kind == INV_BACKLOG) && !g_recorded_now) {
        __try { record_tick(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { log_line("X2-SMOOTH-002: fault 0x%08lX in record", GetExceptionCode()); }
    }
    static LONG last = 0;
    if (g_opt_diag && g_logic_draws - last >= 600) {
        last = g_logic_draws;
        log_line("X2-SMOOTH-002: logic windows=%ld interp=%ld interp_cut=%ld warps=%ld renderhold=%ld skip(hist=%ld moved=%ld) | children=%ld actors=%d live=%d",
                 g_logic_draws, g_stats.interp, g_stats.interp_cut, g_warps, g_rh_held, g_skip_hist, g_skip_moved, g_children_moved, *g_actor_count, smooth_live(2));
    }
    static LONG reported = 0;
    if (!reported && g_stats.interp > 0) {
        reported = 1;
        log_line("X2-SMOOTH-002: first interpolated actor root on a logic invocation");
    }
}

// --- Install: find and check every address, then patch the two CALLs -------------------------------------------------------
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
    g_actor_count = (volatile int32_t*)abs_operand("actor count", t + 2);
    g_actor_table = (uint8_t* volatile*)abs_operand("actor table", t + 8);
    if (!g_actor_count || !g_actor_table) return false;
    g_history = (RootHistory*)calloc(MAX_ACTORS, sizeof(RootHistory));
    g_written_slot = (int*)calloc(MAX_ACTORS, sizeof(int));
    g_slot_write = (int*)calloc(MAX_ACTORS, sizeof(int));
    g_written_game_root = (float(*)[16])calloc(MAX_ACTORS, 64);
    g_written_our_root = (float(*)[16])calloc(MAX_ACTORS, 64);
    if (!g_history || !g_written_slot || !g_slot_write || !g_written_game_root || !g_written_our_root) { log_line("  X2-SMOOTH-002: out of memory - refused"); return false; }
    g_draw_main = (draw_main_fn)fn;
    g_opt_nocloth = config_row("X2-SMOOTH-002-NOCLOTH", 0);
    g_opt_diag = config_row("X2-SMOOTH-002-DIAG", 1);
    g_opt_childrel = config_row("X2-SMOOTH-002-CHILDREL", 1);
    g_opt_warp = config_row("X2-SMOOTH-002-WARP", 1);
    g_opt_warp_min = (float)config_row("X2-SMOOTH-002-WARP-MIN", 30);
    g_opt_warp_ratio = (float)config_row("X2-SMOOTH-002-WARP-RATIO", 3);
    g_opt_renderhold = config_row("X2-SMOOTH-002-RENDERHOLD", 1);
    for (int i = 0; i < 2; i++) {
        if (!patch_call_rel32(sites[i], fn, (void*)&hook_draw_main)) {
            if (i == 1) patch_call_rel32(sites[0], (void*)&hook_draw_main, fn);
            log_line("  X2-SMOOTH-002: patch %d failed - refused", i);
            return false;
        }
    }
    log_line("  X2-SMOOTH-002: Ch_DrawMain gva=0x%08X (expected 0x006DBA00), CALLs gva=0x%08X/0x%08X -> stub; actors gva=0x%08X "
             "(0x01CC9604) count gva=0x%08X (0x01CC9600) stride 0x910 root +0x1DC; nocloth=%d childrel=%d diag=%d; "
             "warp guard %d (min %.0f, ratio %.0f); renderhold %d; F7 toggles",
             rt_to_gva(fn), rt_to_gva(sites[0]), rt_to_gva(sites[1]), rt_to_gva((const void*)g_actor_table),
             rt_to_gva((const void*)g_actor_count), g_opt_nocloth, g_opt_childrel, g_opt_diag, g_opt_warp, g_opt_warp_min, g_opt_warp_ratio, g_opt_renderhold);
    return true;
}

Row row_SMOOTH002 = {
    "X2-SMOOTH-002",
    "60 Hz actor roots: logic frames draw each root half-way between the last two ticks (needs X2-SMOOTH=1)",
    1, install, pre, post, 0, 0, stop
};

} // namespace x2
