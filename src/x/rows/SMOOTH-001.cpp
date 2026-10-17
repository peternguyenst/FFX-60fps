// SPDX-License-Identifier: Apache-2.0
// SMOOTH-001: under the mod's pacing the camera is still built once per logic tick, so it moves at 30 Hz.
// This row draws every pass of a logic period with the view a fraction of the way between the last two ticks:
// pass j of N draws (j + 1) / N (pacing_t), and the last pass of the period draws tick k untouched. At 60 fps
// (N = 2) that is the old pair - the logic frame at 0.5, the render-only frame at tick k. The logic pass
// interpolates from the tick before this one (h.prev) to the view the game just built; later passes
// interpolate the pair the period was entered with (h.last -> h.prev). Needs master SMOOTH=1.
// Details: docs/fps60/rows/SMOOTH-001.md
#include "smooth.h"
#include <string.h>
#include <intrin.h>
#include <stdio.h>
#include <math.h>

typedef int (__cdecl* finalize_t)(int);
typedef void(__cdecl* commit_t)(float*);

static finalize_t g_finalize = nullptr;
static commit_t   g_commit = nullptr;
static float*     g_view = nullptr;           // the view matrix the draw reads (16 floats)
static volatile uint32_t* g_src2 = nullptr;   // != 0: camera source s2 (look-at from a fixed camera block)
static volatile uint32_t* g_src3 = nullptr;   // != 0: camera source s3 (field map camera)
static int        g_diag = 0;
static int        g_fov = 1;                 // SMOOTH-001-FOV: interpolate the battle camera FOV with the view
typedef void(__cdecl* fovcommit_t)(int);
static fovcommit_t g_fovcommit = nullptr;    // MsCommitCameraFovAspect (FOV/aspect setters, then the view commit)
static float*     g_fovblk = nullptr;        // camera blocks 0x01137000 (stride 0x1C0)
static const int  k_fovoff[3] = { 0x10, 0x14, 0x1C };
static LONG       g_fov_interp = 0, g_fov_cut = 0;
static int        g_jump = 1;                // SMOOTH-001-JUMPCUT: a step far above the previous tick's step is a cut
static const float k_jump_ratio = 4.0f, k_jump_dist = 20.0f, k_jump_deg = 10.0f;
static LONG       g_jump_cut = 0;
static int        g_camlog = 0;              // SMOOTH-001-CAMLOG: log the view per finalize during LAB-001's cast window
// SMOOTH-001-TRACE=<lines> (Release too): F6 in the game window arms a burst of that many finalize lines. Each
// line says which pass of the logic period it is and what t it draws at, the view the game's own finalize just
// built, and the view left for the draw - so a camera that does not move smoothly can be read off the log
// instead of guessed at. Every commit during the burst is logged too, with its caller.
static int        g_trace = 0;
static volatile LONG g_trace_left = 0;
static bool       g_tr_ours = false, g_tr_wrote = false;
bool lab001_mot_window();
static commit_t   g_commit_tramp = nullptr;   // CAMLOG: commit hook (every committed view, any caller)
static const float* g_fxcam = nullptr;        // CAMLOG: effect-walker camera block 0x0113FCD0 (OPU scratch +0x280 source)
static const uint32_t* g_blocks = nullptr;    // CAMLOG: viewport camera blocks 0x01137000, 3 x 0x1C0 (FOV/aspect, tweens)
static uint32_t   g_blk_prev[3 * 0x70];
static bool       g_blk_valid = false, g_blk_first = false;

// CAMLOG: the camera-block dwords that changed since the previous frame's draw (first finalize of each invocation)
static void log_block_diff() {
    char line[1024]; int k = 0, n = 0; line[0] = 0;
    for (int i = 0; i < 3 * 0x70; i++) {
        uint32_t v = g_blocks[i];
        if (g_blk_valid && v != g_blk_prev[i] && n < 24) {
            float a, b; memcpy(&a, &g_blk_prev[i], 4); memcpy(&b, &v, 4);
            k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %d+%03X:%g>%g", i / 0x70, (i % 0x70) * 4, a, b);
            n++;
        }
        g_blk_prev[i] = v;
    }
    g_blk_valid = true;
    if (line[0]) log_line("SMOOTH-001 blocks: %c%s", "LBKS"[pacing_kind() & 3], line);
}

static void __cdecl h_commit(float* v) {
    if (g_trace_left > 0) {
        __try {
            log_line("SMOOTH-001-TRACE commit: %c pass %d from gva 0x%08X view %s | t(%.3f %.3f %.3f) r2(%.4f %.4f %.4f)",
                     "LBKS"[pacing_kind() & 3], pacing_pass(), rt_to_gva_exe(_ReturnAddress()),
                     v == g_view ? "main" : "other", v[12], v[13], v[14], v[8], v[9], v[10]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (lab001_mot_window()) {
        __try {
            log_line("SMOOTH-001 commit: %c from gva 0x%08X view %s | t %.2f %.2f %.2f | r2 %.4f %.4f %.4f",
                     "LBKS"[pacing_kind() & 3], rt_to_gva_exe(_ReturnAddress()), v == g_view ? "main" : "other",
                     v[12], v[13], v[14], v[8], v[9], v[10]);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_commit_tramp(v);
}

struct CamHist {
    float prevfov[3];    // FOV fields (camera block +0x10/+0x14/+0x1C) of the previous logic tick
    float pendfov[3];
    float lastfov[3];    // the tick before prev (render-only passes interpolate last -> prev)
    float prev[16];      // canonical matrix of the previous logic tick
    bool  valid;
    int   tag;           // source of prev (1..3)
    float pend[16];      // sample of the current logic invocation
    int   pend_tag;
    bool  has_pend;
    float last[16];      // canonical matrix of the tick before prev
    bool  last_valid;
    bool  pend_cut;      // this tick was drawn as a cut (its step is not a motion reference)
    bool  prev_cut;      // the tick now in prev was a cut: the render-only passes hold it too
    float step_d, step_a; // eye distance / turn (degrees) from the tick before prev to prev
    bool  step_valid;
};
static CamHist g_h[3];
static int   g_kind = INV_STOCK;
// Deferred restore state (one view matrix; the last write wins).
static bool  g_written = false;
static bool  g_committed = false;
static float g_orig[16], g_out[16];
static SmoothStats g_st;
static LONG  g_logic_calls = 0, g_skip_hist = 0;

static int src_tag() { return *g_src3 ? 3 : (*g_src2 ? 2 : 1); }

static void write_view(const float* out, int tag) {
    memcpy(g_orig, g_view, 64);
    memcpy(g_out, out, 64);
    memcpy(g_view, out, 64);
    g_written = true;
    if (tag == 3 || tag == 1) { g_commit(g_view); g_committed = true; }
}

// Draw the view t of the way from `a` to `b`, with the camera block's FOV fields taken from fa -> fb on the
// same frames (the view alone would lead the zoom: Mental Break shift/zoom). A big FOV jump is a camera cut.
// Returns false when the pair could not be swept: the caller then leaves the frame on tick k.
static bool draw_between(int vp, int tag, const float* a, const float* b, const float* fa, const float* fb, float t) {
    float* blk = g_fovblk ? (float*)((uint8_t*)g_fovblk + vp * 0x1C0) : nullptr;
    bool fov_mid = false;
    float mid[3], cur[3];
    if (g_fov && blk && tag == 1 && fa && fb) {
        for (int i = 0; i < 3; i++) {
            cur[i] = *(float*)((uint8_t*)blk + k_fovoff[i]);
            float x = fa[i], y = fb[i], m = fabsf(x) > fabsf(y) ? fabsf(x) : fabsf(y);
            if (m > 0 && fabsf(x - y) > 0.25f * m) { InterlockedIncrement(&g_fov_cut); return false; }
            mid[i] = x + t * (y - x);
            if (x != y) fov_mid = true;
        }
    }
    float out[16];
    if (!smooth_interp_view(a, b, t, SMOOTH_CUT_DIST, out)) { InterlockedIncrement(&g_st.interp_cut); return false; }
    if (fov_mid) {
        for (int i = 0; i < 3; i++) *(float*)((uint8_t*)blk + k_fovoff[i]) = mid[i];
        g_fovcommit(vp);                                      // commits the game view; write_view commits ours after
        for (int i = 0; i < 3; i++) *(float*)((uint8_t*)blk + k_fovoff[i]) = cur[i];
        InterlockedIncrement(&g_fov_interp);
    }
    if (memcmp(out, g_view, 64) == 0) return true;            // swept, but it is the frame already there
    write_view(out, tag);
    g_tr_wrote = true;
    InterlockedIncrement(&g_st.interp);
    return true;
}

static void on_call(int vp) {
    CamHist& h = g_h[vp];
    int tag = src_tag();
    float t = pacing_t();
    g_tr_ours = g_written && memcmp(g_view, g_out, 64) == 0;
    g_tr_wrote = false;

    if (g_kind == INV_BUILD) {                                // a render-only pass inside the logic period
        if (!smooth_live(1) || t >= 1.0f) return;             // the last pass of the period draws tick k
        // Still the matrix we wrote for an earlier viewport this pass: writing again would save it as g_orig,
        // and pre()'s restore would hand it back as if the game had built it (same guard as the logic path).
        if (g_written && memcmp(g_view, g_out, 64) == 0) return;
        if (tag == 1 && vp == 1) return;                      // special commit paths (MsCommitCameraFovAspect)
        // prev_cut: the logic pass found this tick was a cut and left the frame on tick k, so the passes
        // after it hold tick k as well rather than sweeping across the cut.
        if (!h.valid || !h.last_valid || h.prev_cut || h.tag != tag) { g_skip_hist++; return; }
        draw_between(vp, tag, h.last, h.prev, h.lastfov, h.prevfov, t);
        return;
    }
    if (g_kind != INV_LOGIC && g_kind != INV_BACKLOG) return;

    bool ours = g_written && memcmp(g_view, g_out, 64) == 0;  // already ours (s3: nothing rebuilt it)
    // History always holds the canonical (game) matrix, never our interpolated one.
    memcpy(h.pend, ours ? g_orig : g_view, 64);
    h.pend_tag = tag;
    h.has_pend = true;
    float* blk = g_fovblk ? (float*)((uint8_t*)g_fovblk + vp * 0x1C0) : nullptr;
    if (blk) for (int i = 0; i < 3; i++) h.pendfov[i] = *(float*)((uint8_t*)blk + k_fovoff[i]);
    if (!smooth_live(1) || ours) return;
    g_logic_calls++;
    if (tag == 1 && vp == 1) return;
    if (!h.valid || h.tag != tag) { g_skip_hist++; return; }
    h.pend_cut = true;                                        // cleared below once the frame is interpolated
    // Jump: the camera moved or turned far more than in the previous tick (a cut below the 500-unit / 60-degree
    // limits, U 17/10: field cutscene cut drew one in-between frame of two shots).
    float sd, sa;
    if (g_jump && h.step_valid && smooth_view_step(h.prev, g_view, &sd, &sa) &&
        ((sd > k_jump_dist && sd > k_jump_ratio * h.step_d) || (sa > k_jump_deg && sa > k_jump_ratio * h.step_a))) {
        LONG n = InterlockedIncrement(&g_jump_cut);
        if (n <= 200)
            log_line("SMOOTH-001: jump cut vp %d src %d: step %.1f units %.1f deg (previous tick %.1f units %.1f deg)",
                     vp, tag, sd, sa, h.step_d, h.step_a);
        return;
    }
    if (t >= 1.0f) { h.pend_cut = false; return; }            // a one-pass period (booster): draw tick k
    if (draw_between(vp, tag, h.prev, g_view, h.prevfov, h.pendfov, t)) h.pend_cut = false;
}

static int __cdecl h_finalize(int vp) {
    int r = g_finalize(vp);
    if (r != 0 || vp < 0 || vp > 2 || g_disabled || !row_SMOOTH001.on) return r;
    bool tracing = g_trace_left > 0;
    float built[16];
    if (tracing) __try { memcpy(built, g_view, 64); } __except (EXCEPTION_EXECUTE_HANDLER) { tracing = false; }
    g_tr_ours = g_tr_wrote = false;
    __try { on_call(vp); }
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-001 finalize stub", GetExceptionCode()); }
    if (tracing && InterlockedDecrement(&g_trace_left) >= 0) {
        __try {
            const float* d = g_view;
            log_line("SMOOTH-001-TRACE: %c pass %d t=%.3f vp=%d src=%d valid=%d/%d ours=%d wrote=%d | built t(%.3f %.3f %.3f)"
                     " r2(%.4f %.4f %.4f) | drawn t(%.3f %.3f %.3f) r2(%.4f %.4f %.4f)",
                     "LBKS"[g_kind & 3], pacing_pass(), pacing_t(), vp, src_tag(), g_h[vp].valid, g_h[vp].last_valid,
                     g_tr_ours, g_tr_wrote, built[12], built[13], built[14], built[8], built[9], built[10],
                     d[12], d[13], d[14], d[8], d[9], d[10]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_camlog && lab001_mot_window()) {
        if (g_blk_first && g_blocks) { g_blk_first = false; log_block_diff(); }
        const float* m = g_view;
        log_line("SMOOTH-001 cam: %c vp %d src %d ours %d | r0 %.4f %.4f %.4f | r2 %.4f %.4f %.4f | t %.2f %.2f %.2f",
                 "LBKS"[g_kind & 3], vp, src_tag(), g_written && memcmp(g_view, g_out, 64) == 0,
                 m[0], m[1], m[2], m[8], m[9], m[10], m[12], m[13], m[14]);
        if (g_fxcam)
            log_line("SMOOTH-001 fxcam: %c | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.2f %.2f %.2f %.2f",
                     "LBKS"[g_kind & 3], g_fxcam[0], g_fxcam[1], g_fxcam[2], g_fxcam[3], g_fxcam[4], g_fxcam[5], g_fxcam[6],
                     g_fxcam[7], g_fxcam[8], g_fxcam[9], g_fxcam[10], g_fxcam[11], g_fxcam[12], g_fxcam[13], g_fxcam[14],
                     g_fxcam[15]);
    }
    return r;
}

// F6 (game window focused, not Ctrl+F6 which is DIAG-HANGDUMP's): arm a trace burst.
static void trace_poll() {
    if (!g_trace) return;
    static bool prev = false;
    bool down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0 && !(GetAsyncKeyState(VK_CONTROL) & 0x8000);
    bool edge = down && !prev;
    prev = down;
    if (!edge) return;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;
    InterlockedExchange(&g_trace_left, g_trace);
    log_line("SMOOTH-001-TRACE: armed, next %d finalize calls (F6)", g_trace);
}

static void restore() {
    if (!g_written) return;
    if (memcmp(g_view, g_out, 64) == 0) {
        memcpy(g_view, g_orig, 64);
        if (g_committed) g_commit(g_view);
    }
    g_written = false;
    g_committed = false;
}

static void pre() {
    smooth_poll_hotkey();
    trace_poll();
    __try { restore(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("SMOOTH-001 restore", GetExceptionCode()); }
    g_kind = g_disabled ? INV_STOCK : pacing_kind();
    g_blk_first = true;
    for (int i = 0; i < 3; i++) {
        g_h[i].has_pend = false;
        g_h[i].pend_cut = false;
        if (g_kind == INV_STOCK) g_h[i].valid = g_h[i].last_valid = false;
    }
}

static void post() {
    if (g_kind == INV_LOGIC || g_kind == INV_BACKLOG) {
        for (int i = 0; i < 3; i++) {
            CamHist& h = g_h[i];
            if (!h.has_pend) {                            // viewport not drawn this tick: history broken
                h.valid = h.last_valid = h.step_valid = false;
                continue;
            }
            // Both of these read h.prev while it is still the previous tick, so they come before it rotates.
            h.step_valid = h.valid && h.tag == h.pend_tag && !h.pend_cut && smooth_view_step(h.prev, h.pend, &h.step_d, &h.step_a);
            memcpy(h.last, h.prev, 64);                       // the pair the render-only passes interpolate
            memcpy(h.lastfov, h.prevfov, sizeof(h.lastfov));
            h.last_valid = h.valid;
            h.prev_cut = h.pend_cut;
            memcpy(h.prev, h.pend, 64);
            memcpy(h.prevfov, h.pendfov, sizeof(h.prevfov));
            h.tag = h.pend_tag;
            h.valid = true;
        }
    }
    static LONG last = 0;
    if (g_diag && g_logic_calls - last >= 600) {
        last = g_logic_calls;
        log_line("SMOOTH-001: logic calls=%ld interp=%ld interp_cut=%ld skip_hist=%ld fov_interp=%ld fov_cut=%ld jump_cut=%ld | src=%d live=%d",
                 g_logic_calls, g_st.interp, g_st.interp_cut, g_skip_hist, g_fov_interp, g_fov_cut, g_jump_cut, src_tag(), smooth_live(1));
    }
    static LONG reported = 0;
    if (!reported && g_st.interp > 0) {
        reported = 1;
        log_line("SMOOTH-001: first interpolated camera on a logic invocation (source %d)", src_tag());
    }
}

static bool install() {
    if (!smooth_master()) { log_line("  SMOOTH-001: master switch SMOOTH=0 - off, nothing scanned or patched"); return false; }
    // Sg_MainLoop draw: first per-viewport CALL to the viewport finalize (FUN_007bc110)
    uint8_t* a = sig_find_unique("SMOOTH-001 viewport loop (Sg_MainLoop draw)",
        "33 FF 33 DB 53 E8 ?? ?? ?? ?? 83 C4 04 85 C0 0F 8F ?? ?? ?? ?? 0F 85", 0x821184);
    // Sg_MainLoop draw: the second-pass and main-viewport finalize CALLs
    uint8_t* b = sig_find_unique("SMOOTH-001 viewport loop tail (Sg_MainLoop draw)",
        "43 53 E8 ?? ?? ?? ?? 83 C4 04 85 C0 0F 8E ?? ?? ?? ?? 47 83 FF 02 0F 8C ?? ?? ?? ?? 0F BE 05 ?? ?? ?? ?? 50 E8", 0x82135C);
    // Field map camera: commits its view matrix (gives the view address and the commit function)
    uint8_t* c = sig_find_unique("SMOOTH-001 map camera commit (FUN_00920c00)",
        "68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 0C 5F 5E 8B 4D FC", 0x920E1A);
    if (!a || !b || !c) return false;
    uint8_t* sites[3] = { a + 5, b + 2, b + 0x24 };
    uint8_t* fin = call_target(sites[0]);
    if (!fin || call_target(sites[1]) != fin || call_target(sites[2]) != fin) {
        log_line("  SMOOTH-001: the three viewport CALLs do not share one target - refused");
        return false;
    }
    // Viewport finalize: picks the camera source (src2 look-at, else src3 = map camera)
    uint8_t* d = sig_find_unique("SMOOTH-001 camera sources (FUN_007bc110)",
        "39 1D ?? ?? ?? ?? 74 3A FF 75 FC 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? D9 05 ?? ?? ?? ?? 83 C4 04 D9 1C 24 "
        "68 ?? ?? ?? ?? 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 14 E9 ?? ?? ?? ?? "
        "83 3D ?? ?? ?? ?? 00 0F 85", 0x7BC3D1);
    if (!d) return false;
    if (d < fin || d > fin + 0x600) { log_line("  SMOOTH-001: source block not inside the finalize function - refused"); return false; }
    uint32_t src2 = *(uint32_t*)(d + 2), view = *(uint32_t*)(d + 0x31), src3 = *(uint32_t*)(d + 0x44);
    uint32_t view2 = *(uint32_t*)(c + 1);
    uint8_t* commit = call_target(c + 5);
    // Commit function: prologue, then the skip-render check
    uint8_t* skip = sym_rt("skip_render");
    if (view != view2 || !sig_match(commit, "55 8B EC 81 EC C0 00 00 00 83 3D") ||
        (skip && read_abs(commit + 0xB) != skip)) {
        log_line("  SMOOTH-001: view/commit cross-check failed (view 0x%08X vs 0x%08X, commit %p) - refused", view, view2, commit);
        return false;
    }
    g_finalize = (finalize_t)fin;
    g_commit = (commit_t)commit;
    g_view = (float*)(uintptr_t)view;
    g_src2 = (volatile uint32_t*)(uintptr_t)src2;
    g_src3 = (volatile uint32_t*)(uintptr_t)src3;
    g_diag = config_row("SMOOTH-001-DIAG", 0);
    g_camlog = config_row("SMOOTH-001-CAMLOG", 0);
    g_trace = config_row("SMOOTH-001-TRACE", 0);
    if (g_trace < 0) g_trace = 0;
    if (g_trace > 20000) g_trace = 20000;
    g_fov = config_row("SMOOTH-001-FOV", 1);
    g_jump = config_row("SMOOTH-001-JUMPCUT", 1);
    if (g_fov) {
        // MsCommitCameraFovAspect(idx): FOV/aspect from the camera block idx*0x1C0 (+0x10/+0x14), then the view commit
        uint8_t* f = sig_find_unique("SMOOTH-001 MsCommitCameraFovAspect",
            "55 8B EC 83 EC 10 56 8B 75 08 57 8B FE 69 FF C0 01 00 00 D9 87", 0x7BC980);
        uint8_t* tween = sym_rt("cam_tween_slots");
        if (f && tween && read_abs(f + 0x15) == tween - 0xDC + 0x10) {
            g_fovcommit = (fovcommit_t)f;
            g_fovblk = (float*)(tween - 0xDC);
        } else {
            log_line("  SMOOTH-001: FOV commit or camera blocks not confirmed - SMOOTH-001-FOV off");
            g_fov = 0;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (!patch_call_rel32(sites[i], fin, (void*)&h_finalize)) {
            for (int j = 0; j < i; j++) patch_call_rel32(sites[j], (void*)&h_finalize, fin);
            log_line("  SMOOTH-001: patch %d failed - refused", i);
            return false;
        }
    }
    if (g_trace && !g_camlog) {
        if (!hook_entry("SMOOTH-001 commit", commit, (void*)&h_commit, (void**)&g_commit_tramp))
            log_line("  SMOOTH-001: TRACE commit hook failed - finalize lines only");
        log_line("  SMOOTH-001-TRACE: %d finalize lines per F6 press (commits logged with them)", g_trace);
    }
    if (g_camlog) {
        // OpuMain_UnitBytecodeWalk copies 0x0113FCD0.. into its scratch +0x280 (MOV EAX,[0x0113FCD0] at 0x80CC00+...)
        uint8_t* w = sig_find_unique("SMOOTH-001 CAMLOG OpuMain_UnitBytecodeWalk",
            "55 8B EC 83 EC 28 A1 ?? ?? ?? ?? 33 C5 89 45 FC 53 56 57 0F B7 7D 08 83 FF 01 75 06 89 3D ?? ?? ?? ?? 68 00 04 00 00 E8",
            0x80CC00);
        if (w && sig_match(w + 0x9A, "8B 15")) g_fxcam = (const float*)read_abs(w + 0x9C);   // mov edx,[0x0113FCD0]
        uint8_t* tween = sym_rt("cam_tween_slots");
        if (tween) g_blocks = (const uint32_t*)(tween - 0xDC);                                 // as LOOP-004
        if (!hook_entry("SMOOTH-001 commit", commit, (void*)&h_commit, (void**)&g_commit_tramp))
            log_line("  SMOOTH-001: CAMLOG commit hook failed");
        log_line("  SMOOTH-001: CAMLOG on (finalize view, every commit, fx camera %p)", (void*)g_fxcam);
    }
    if (!row_on("LOOP-005") || !row_on("LOOP-004"))
        log_line("  SMOOTH-001: note: LOOP-005/LOOP-004 off; frames where the game moves the camera itself are left alone");
    log_line("  SMOOTH-001: finalize gva=0x%08X (expected 0x007BC110), CALLs gva=0x%08X/0x%08X/0x%08X -> stub; view gva=0x%08X "
             "(0x02311440), src2 0x%08X (0x0231142C), src3 0x%08X (0x023114C4), commit gva=0x%08X (0x00639860); interpolation; diag=%d; F7 toggles",
             rt_to_gva_exe(fin), rt_to_gva_exe(sites[0]), rt_to_gva_exe(sites[1]), rt_to_gva_exe(sites[2]),
             rt_to_gva_exe(g_view), rt_to_gva_exe((const void*)g_src2), rt_to_gva_exe((const void*)g_src3),
             rt_to_gva_exe(commit), g_diag);
    log_line("  SMOOTH-001: jump cut %s (SMOOTH-001-JUMPCUT; step > %.0fx the previous tick's and > %.0f units or %.0f deg)",
             g_jump ? "ON" : "OFF", k_jump_ratio, k_jump_dist, k_jump_deg);
    log_line("  SMOOTH-001: FOV interpolation %s (SMOOTH-001-FOV; MsCommitCameraFovAspect gva=0x%08X, blocks gva=0x%08X)",
             g_fov ? "ON" : "OFF", g_fovcommit ? rt_to_gva_exe((void*)g_fovcommit) : 0,
             g_fovblk ? rt_to_gva_exe(g_fovblk) : 0);
    return true;
}

Row row_SMOOTH001 = {
    "SMOOTH-001",
    "smooth camera: each pass draws the view a fraction of the way between the last two ticks (needs master SMOOTH=1)",
    1, install, pre, post, 0
};
