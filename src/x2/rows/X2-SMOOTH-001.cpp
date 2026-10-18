// SPDX-License-Identifier: Apache-2.0
// X2-SMOOTH-001 (port of FFX SMOOTH-001): at 60 Hz the camera is still built once per logic tick, so it moves at
// 30 Hz. Logic invocations draw the view half-way between the last two ticks (interpolation; render-only invocations
// draw tick k). The game view is put back at the next invocation. Needs master X2-SMOOTH=1; F7 toggles.
// FFX-2 changes (PORT-SMOOTH.md): finalize Cam_ViewportFinalize(vp, pass) and commit Cam_CommitView(view, slot) take
// 2 args (re-commit uses slot 0, as every tag-1 commit for vp 0/2 and the map camera do); no interpolation while the
// debug camera flag [0x01ACCE10] is set; FOV (X2-SMOOTH-001-FOV): FFX-2 has no FOV commit helper, so the two inline
// setter CALLs (Gfx_SetCameraFov / Gfx_SetCameraAspect) record the game's values, and an interpolated frame calls the
// setters again with the midpoints (the cut test stays on camera block +0x10/+0x14/+0x1C).
// Evidence: outer repo re/ledger/PORT-SMOOTH.md §1-§3 (S).
// X2-SMOOTH-001-RHCUT (default 0, test): render-hold also when the render-only view jumped beyond SMOOTH_CUT_DIST.
// U 18/10/2026 (dressphere change, Warrior): three consecutive frames dark close-up / lit battle floor / dark close-up;
// H: the change's camera is set on logic invocations only, a render-only frame gets the battle camera, which the
// distance test passes as a cut. Logs the first held jump and counts them.
#include "x2_smooth.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

namespace x2 {

typedef int (__cdecl* finalize_t)(int vp, int pass);
typedef void(__cdecl* commit_t)(float* view, int slot);
typedef void(__cdecl* fovset_t)(float* v);

static finalize_t g_finalize = nullptr;
static commit_t   g_commit = nullptr;
static float*     g_view = nullptr;           // 0x01ACCD80
static volatile uint32_t* g_src2 = nullptr;   // 0x01ACCD74 != 0: fixed look-at
static volatile uint32_t* g_src3 = nullptr;   // 0x01ACCE04 != 0: map camera
static volatile uint32_t* g_dbg = nullptr;    // 0x01ACCE10 != 0: debug camera (H: 0 in retail)
static int        g_fov = 1;
static fovset_t   g_setfov = nullptr, g_setasp = nullptr;
static float*     g_blk = nullptr;            // viewport camera blocks 0xE12A40, stride 0x1C0
static const int  k_fovoff[3] = { 0x10, 0x14, 0x1C };
static int        g_cur_vp = -1;              // viewport inside the finalize (setter stubs record per vp)
static float      g_rec_fov[3], g_rec_asp[3];
static bool       g_rec_ok[3];
static LONG       g_fov_interp = 0, g_fov_cut = 0;
extern Row row_SMOOTH001;

static void __cdecl h_setfov(float* v) { if (g_cur_vp >= 0 && v) g_rec_fov[g_cur_vp] = *v; g_setfov(v); }
static void __cdecl h_setasp(float* v) {
    if (g_cur_vp >= 0 && v) { g_rec_asp[g_cur_vp] = *v; g_rec_ok[g_cur_vp] = true; }
    g_setasp(v);
}

struct CamHist {
    float prevfov[3], pendfov[3];        // camera block fields (cut test)
    float prevset[2], pendset[2];        // game FOV / aspect values passed to the setters
    bool  prevset_ok, pendset_ok;
    float prev[16];
    bool  valid;
    int   tag;
    float pend[16];
    int   pend_tag;
    bool  has_pend;
};
static CamHist g_h[3];
static int   g_kind = INV_STOCK;
static bool  g_written = false, g_committed = false;
static float g_orig[16], g_out[16];
static SmoothStats g_st;
static LONG  g_logic_calls = 0, g_skip_hist = 0;
// X2-SMOOTH-001-RENDERHOLD (default 1): a render-only invocation whose rebuilt view differs from tick k (the game moved
// the camera outside a logic tick) draws tick k, so the drawn sequence stays k-0.5, k, k+0.5 (U 13/10/2026: "camera is
// the issue now" after limbs; frame pacing clean, D fps60x2.log 21:05: avg 16.68 ms). Same idea as X2-SMOOTH-002-
// RENDERHOLD. Logged: how often and how far the view moved on render-only frames (proves or refutes the cause).
static int   g_renderhold = 1;
// X2-SMOOTH-001-CUTDEG (default 20): a view that turned more than this many degrees in one tick is a camera cut and
// snaps (the shared interpolator only cuts above 60 deg). D 13/10/2026 F9 trace: normal turning 0.2-0.3 deg per frame;
// a 44 deg angle switch was blended into a 22 deg in-between frame (one-frame whip at camera cuts).
static float g_cut_cos = 0.9396926f;            // cos(20 deg)
static LONG  g_rot_cuts = 0;

// Cosine of the turn between two view matrices (camera forward = row 2 of the inverse).
static bool view_turn_cos(const float* a, const float* b, float* c) {
    float ia[16], ib[16];
    if (!smooth_affine_inverse(a, ia) || !smooth_affine_inverse(b, ib)) return false;
    float la = sqrtf(ia[8] * ia[8] + ia[9] * ia[9] + ia[10] * ia[10]);
    float lb = sqrtf(ib[8] * ib[8] + ib[9] * ib[9] + ib[10] * ib[10]);
    if (!(la > 1e-6f) || !(lb > 1e-6f)) return false;
    *c = (ia[8] * ib[8] + ia[9] * ib[9] + ia[10] * ib[10]) / (la * lb);
    return true;
}
static LONG  g_rh_checked = 0, g_rh_held = 0;
static float g_rh_sum = 0, g_rh_max = 0;

// X2-SMOOTH-001-LASTWRITE (default 1): the HD camera setter HdCam_Set 0x482320 keeps only the FIRST write per slot and
// HD frame (mask [hd+0x218], reset to 0xFF at 0x47EE9A at the end of app render). On logic invocations MapCam_Update
// (0x7DB05D, before the viewport loop) commits the raw new tick first, so this row's interpolated commit was dropped and
// the screen showed k+1, k+1, k+2 (D 14/10/2026: user recording, background identical every 2nd frame, 7 px steps).
// Our commits re-arm slot 0's bit so the last write (ours) wins. Evidence: re/ledger/HDNODE.md (S).
static int   g_lastwrite = 1;
static uint8_t** g_hdobj = nullptr;             // 0x00DCDA04 (HD renderer object)
static LONG  g_lw_rearm = 0, g_lw_commits = 0;

static void commit_slot0() {
    if (g_lastwrite && g_hdobj && *g_hdobj) {
        uint32_t* mask = (uint32_t*)(*g_hdobj + 0x218);
        if (!(*mask & 1)) { *mask |= 1; InterlockedIncrement(&g_lw_rearm); }
    }
    InterlockedIncrement(&g_lw_commits);
    g_commit(g_view, 0);
}

static int src_tag() { return *g_src3 ? 3 : (*g_src2 ? 2 : 1); }

static void write_view(const float* out, int tag) {
    memcpy(g_orig, g_view, 64);
    memcpy(g_out, out, 64);
    memcpy(g_view, out, 64);
    g_written = true;
    if (tag == 3 || tag == 1) { commit_slot0(); g_committed = true; }
}

static int  g_rhcut = 0;
static LONG g_rhcut_n = 0, g_rhtag_n = 0;
static void render_hold(int vp) {
    CamHist& h = g_h[vp];
    if (!g_renderhold || !h.valid || !smooth_live(1) || *g_dbg) return;
    int tag = src_tag();
    if (tag != h.tag && g_rhcut && InterlockedIncrement(&g_rhtag_n) <= 4)    // not held: log the source switch
        log_line("X2-SMOOTH-001-RHCUT: render-only camera source %d differs from tick k's %d (vp %d) - not held", tag, h.tag, vp);
    if (tag != h.tag || (tag == 1 && vp == 1)) return;
    if (g_written && memcmp(g_view, g_out, 64) == 0) return;       // already ours this invocation
    InterlockedIncrement(&g_rh_checked);
    if (memcmp(g_view, h.prev, 64) == 0) return;                 // the view is tick k: nothing to do
    float ia[16], ib[16];
    if (!smooth_affine_inverse(g_view, ia) || !smooth_affine_inverse(h.prev, ib)) return;
    float dx = ia[12] - ib[12], dy = ia[13] - ib[13], dz = ia[14] - ib[14];
    float d = sqrtf(dx * dx + dy * dy + dz * dz);                // eye moved this far outside a logic tick
    if (d > SMOOTH_CUT_DIST) {                                   // a cut on a render-only frame: let it through
        if (!g_rhcut) return;
        if (InterlockedIncrement(&g_rhcut_n) == 1)
            log_line("X2-SMOOTH-001-RHCUT: first render-only view jump past the cut distance (eye moved %.2f, source %d) - held", d, tag);
    }
    g_rh_sum += d;
    if (d > g_rh_max) g_rh_max = d;
    write_view(h.prev, tag);
    if (InterlockedIncrement(&g_rh_held) == 1)
        log_line("X2-SMOOTH-001-RENDERHOLD: first render-only view that left tick k (eye moved %.2f, source %d) - drawn at tick k", d, tag);
}

static void on_call(int vp) {
    if (g_kind == INV_BUILD) { render_hold(vp); return; }        // render-only frames draw tick k
    if (g_kind != INV_LOGIC && g_kind != INV_BACKLOG) return;
    CamHist& h = g_h[vp];
    int tag = src_tag();
    bool ours = g_written && memcmp(g_view, g_out, 64) == 0;
    memcpy(h.pend, ours ? g_orig : g_view, 64);                // history holds the game matrix
    h.pend_tag = tag;
    h.has_pend = true;
    float* blk = (float*)((uint8_t*)g_blk + vp * 0x1C0);
    for (int i = 0; i < 3; i++) h.pendfov[i] = *(float*)((uint8_t*)blk + k_fovoff[i]);
    h.pendset_ok = g_rec_ok[vp];
    h.pendset[0] = g_rec_fov[vp];
    h.pendset[1] = g_rec_asp[vp];
    if (!smooth_live(1) || ours || *g_dbg) return;
    g_logic_calls++;
    if (tag == 1 && vp == 1) return;                          // special commit slots on viewport 1
    if (!h.valid || h.tag != tag) { g_skip_hist++; return; }
    bool fov_mid = false;
    if (g_fov && tag == 1 && h.pendset_ok && h.prevset_ok) {
        for (int i = 0; i < 3; i++) {
            float a = h.prevfov[i], b = h.pendfov[i], m = fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b);
            if (m > 0 && fabsf(a - b) > 0.25f * m) { InterlockedIncrement(&g_fov_cut); return; }   // zoom cut
            if (a != b) fov_mid = true;
        }
        if (h.prevset[0] != h.pendset[0] || h.prevset[1] != h.pendset[1]) fov_mid = true;
    }
    float tc;
    if (view_turn_cos(h.prev, g_view, &tc) && tc < g_cut_cos) { InterlockedIncrement(&g_rot_cuts); return; }   // angle cut
    float out[16];
    if (!smooth_interp_view(h.prev, g_view, 0.5f, SMOOTH_CUT_DIST, out)) { InterlockedIncrement(&g_st.interp_cut); return; }
    if (fov_mid) {
        float f = 0.5f * (h.prevset[0] + h.pendset[0]), a = 0.5f * (h.prevset[1] + h.pendset[1]);
        g_setfov(&f);                                         // the next finalize sets the game values again
        g_setasp(&a);
        InterlockedIncrement(&g_fov_interp);
    }
    if (memcmp(out, g_view, 64) == 0) return;
    write_view(out, tag);
    InterlockedIncrement(&g_st.interp);
}

// X2-SMOOTH-001 trace (F9, always available): the next 240 viewport-0 finalizes log the drawn eye position, the
// invocation kind (L = logic, R = render-only, B = backlog, S = stock), whether this row wrote the view (interp / hold)
// and the step from the previous drawn frame. U 13/10/2026: camera still judders after the render hold ("maybe less").
static LONG  g_trace_left = 0;
static bool  g_f9_prev = false;
static float g_trace_last[3];
static bool  g_trace_have = false;
// X2-SMOOTH-001-TRACEDLL (ini, default empty): magic numbers whose load starts the camera trace for 900 frames (the
// dressphere change's camera frame, U 18/10/2026: one lit battle-floor frame between dark close-up frames).
static int  g_tdll[8], g_ntdll = 0;
static void trace_on_load(uint8_t*, uint32_t, const char* name) {
    const char* p = strstr(name, "magic_");
    int n = p ? atoi(p + 6) : -1;
    for (int i = 0; i < g_ntdll; i++)
        if (g_tdll[i] == n) { g_trace_left = 900; g_trace_have = false; log_line("X2-SMOOTH-001 trace: %s loaded - next 900 frames", name); }
}
static void trace_on_unload(uint8_t*) {}

// Once per invocation, at post(): the view the frame was drawn with (viewport 0 finalizes run 3 times per frame).
static void trace_frame() {
    if (g_trace_left <= 0) return;
    float inv[16];
    if (!smooth_affine_inverse(g_view, inv)) return;
    float dx = 0, dy = 0, dz = 0;
    if (g_trace_have) { dx = inv[12] - g_trace_last[0]; dy = inv[13] - g_trace_last[1]; dz = inv[14] - g_trace_last[2]; }
    bool ours = g_written && memcmp(g_view, g_out, 64) == 0;
    log_line("X2-SMOOTH-001 trace: %c %s eye (%.2f, %.2f, %.2f) step %.3f | fwd (%.3f, %.3f, %.3f) src %d smooth %s",
             "LRBS"[g_kind & 3], ours ? "ours" : "game", inv[12], inv[13], inv[14], sqrtf(dx * dx + dy * dy + dz * dz),
             inv[8], inv[9], inv[10], src_tag(), smooth_live(1) ? "on" : "OFF");
    g_trace_last[0] = inv[12]; g_trace_last[1] = inv[13]; g_trace_last[2] = inv[14];
    g_trace_have = true;
    if (--g_trace_left == 0) log_line("X2-SMOOTH-001 trace: end");
}

static int __cdecl h_finalize(int vp, int pass) {
    bool rec = vp >= 0 && vp <= 2;
    if (rec) { g_cur_vp = vp; g_rec_ok[vp] = false; }
    int r = g_finalize(vp, pass);
    g_cur_vp = -1;
    if (r != 0 || !rec || g_disabled || !row_SMOOTH001.on) return r;
    __try { on_call(vp); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("X2-SMOOTH-001: fault 0x%08lX in the finalize stub - row off", GetExceptionCode());
        row_SMOOTH001.on = 0;
    }
    return r;
}

static void restore() {
    if (!g_written) return;
    if (memcmp(g_view, g_out, 64) == 0) {
        memcpy(g_view, g_orig, 64);
        if (g_committed) commit_slot0();
    }
    g_written = false;
    g_committed = false;
}

static void pre() {
    smooth_poll_hotkey();
    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !g_f9_prev && g_trace_left <= 0) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) { g_trace_left = 240; g_trace_have = false; log_line("F9: X2-SMOOTH-001 trace: next 240 frames"); }
    }
    g_f9_prev = f9;
    __try { restore(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { log_line("X2-SMOOTH-001: fault in restore"); }
    g_kind = g_disabled ? INV_STOCK : pacing_kind();
    for (int i = 0; i < 3; i++) {
        g_h[i].has_pend = false;
        if (g_kind == INV_STOCK) g_h[i].valid = false;
    }
}

static void post() {
    __try { trace_frame(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_kind == INV_LOGIC || g_kind == INV_BACKLOG) {
        for (int i = 0; i < 3; i++) {
            CamHist& h = g_h[i];
            if (!h.has_pend) { h.valid = false; continue; }   // viewport not drawn this tick: history broken
            memcpy(h.prev, h.pend, 64);
            memcpy(h.prevfov, h.pendfov, sizeof(h.prevfov));
            memcpy(h.prevset, h.pendset, sizeof(h.prevset));
            h.prevset_ok = h.pendset_ok;
            h.tag = h.pend_tag;
            h.valid = true;
        }
    }
    static LONG last = 0;
    if (g_logic_calls - last >= 1800) {
        last = g_logic_calls;
        log_line("X2-SMOOTH-001: rhcut=%ld logic calls=%ld interp=%ld cut=%ld skip_hist=%ld fov_interp=%ld fov_cut=%ld rot_cuts=%ld | renderhold "
                 "checked=%ld held=%ld eye avg %.2f max %.2f | lastwrite commits=%ld rearmed=%ld | src=%d live=%d",
                 g_rhcut_n, g_logic_calls, g_st.interp, g_st.interp_cut, g_skip_hist, g_fov_interp, g_fov_cut, g_rot_cuts, g_rh_checked, g_rh_held,
                 g_rh_held ? g_rh_sum / g_rh_held : 0.0f, g_rh_max, g_lw_commits, g_lw_rearm, src_tag(), smooth_live(1));
    }
    static LONG reported = 0;
    if (!reported && g_st.interp > 0) {
        reported = 1;
        log_line("X2-SMOOTH-001: first interpolated camera on a logic invocation (source %d)", src_tag());
    }
}

static bool install() {
    {
        char buf[96];
        config_str("rows", "X2-SMOOTH-001-TRACEDLL", buf, sizeof(buf), "");
        for (char* q = buf; *q && g_ntdll < 8;) {
            char* e = nullptr; long v = strtol(q, &e, 10);
            if (e == q) { q++; continue; }
            g_tdll[g_ntdll++] = (int)v; q = e;
        }
        if (g_ntdll) magic_mods_subscribe("X2-SMOOTH-001-TRACEDLL", trace_on_load, trace_on_unload);
    }
    if (!smooth_master()) { log_line("  X2-SMOOTH-001: master X2-SMOOTH=0 - off"); return false; }
    uint8_t* a = sig_find("X2-SMOOTH-001 viewport loop head",
        "BF 01 00 00 00 33 F6 33 DB 57 53 E8 ?? ?? ?? ?? 83 C4 08 85 C0 0F 8F ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 33 FF", 0x60593F);
    uint8_t* b = sig_find("X2-SMOOTH-001 viewport loop tail",
        "43 57 53 E8 ?? ?? ?? ?? 83 C4 08 85 C0 0F 8E ?? ?? ?? ?? 46 83 FE 02 0F 8C ?? ?? ?? ?? 0F BE 05 ?? ?? ?? ?? 57 50 E8",
        0x605BB7);
    uint8_t* d = sig_find("X2-SMOOTH-001 src2 / debug cam",
        "83 3D ?? ?? ?? ?? 00 74 7C 83 3D ?? ?? ?? ?? 00 8D 43 10 50 53 74 37 68", 0x650113);
    uint8_t* v = sig_find("X2-SMOOTH-001 view / src3",
        "68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 14 E9 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D", 0x650186);
    uint8_t* c = sig_find("X2-SMOOTH-001 map camera commit",
        "50 E8 ?? ?? ?? ?? 6A 00 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 10 8B 4D FC", 0x7DB050);
    if (!a || !b || !d || !v || !c) return false;
    uint8_t* sites[3] = { a + 0x0B, b + 0x03, b + 0x26 };
    uint8_t* fin = call_target(sites[0]);
    if (!fin || call_target(sites[1]) != fin || call_target(sites[2]) != fin) {
        log_line("  X2-SMOOTH-001: the three viewport CALLs do not share one target - refused");
        return false;
    }
    if (d < fin || d > fin + 0xA00 || v < fin || v > fin + 0xA00) {
        log_line("  X2-SMOOTH-001: camera source blocks not inside the finalize - refused");
        return false;
    }
    g_src2 = (volatile uint32_t*)abs_operand("src2", d + 2);
    g_dbg = (volatile uint32_t*)abs_operand("debug cam", d + 0x0B);
    g_view = (float*)abs_operand("view", v + 1);
    g_src3 = (volatile uint32_t*)abs_operand("src3", v + 0x14);
    if (!g_src2 || !g_dbg || !g_view || !g_src3 || *(const uint32_t*)(v + 0x21) != (uint32_t)(uintptr_t)g_dbg) {
        log_line("  X2-SMOOTH-001: camera globals not as proven - refused");
        return false;
    }
    // map camera: PUSH 0; PUSH view; CALL Cam_CommitView (prologue + skip-render check at +0xB)
    uint8_t* commit = call_target(c + 0x0D);
    if (*(const uint32_t*)(c + 9) != (uint32_t)(uintptr_t)g_view ||
        !sig_match(commit, "55 8B EC 81 EC 00 01 00 00 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 8B 45 08 D9 00") ||
        !g_backlog_count) {
        log_line("  X2-SMOOTH-001: view/commit cross-check failed (commit %p) - refused", commit);
        return false;
    }
    // the camera blocks, as X2-LOOP-004 (LEA EAX,[EBX+base] in the finalize)
    uint8_t* bb = sig_find("X2-SMOOTH-001 camera blocks",
        "69 F6 40 10 00 00 DD 05 ?? ?? ?? ?? 8D 83 ?? ?? ?? ?? 83 C4 04 81 C6 ?? ?? ?? ?? 8D B8 C2 00 00 00 "
        "8D 88 DC 00 00 00", 0x64FE9E);
    if (!bb) return false;
    g_blk = (float*)abs_operand("camera blocks", bb + 0x0E);
    if (!g_blk) return false;
    // LASTWRITE: inside Cam_CommitView, MOV ECX,[hdobj]; slot switch; slot-2 CALL HdCam_Set (thiscall, RET 8, mask +0x218)
    g_lastwrite = config_row("X2-SMOOTH-001-LASTWRITE", 1);
    if (g_lastwrite) {
        uint8_t* lw = sig_find("X2-SMOOTH-001-LASTWRITE commit tail",
            "8B 45 0C 8B 0D ?? ?? ?? ?? 83 E8 00 74 2A 48 74 15 48 8D 85 40 FF FF FF 75 24 6A 02 50 E8", 0x449B58);
        uint8_t* set = lw ? call_target(lw + 0x1D) : nullptr;
        if (!lw || lw < commit || lw > commit + 0x300 || !set ||
            !sig_match(set, "55 8B EC 57 8B F9 8B 4D 0C 8B 97 18 02 00 00 B8 01 00 00 00 D3 E0 85 D0 74") ||
            !(g_hdobj = (uint8_t**)abs_operand("hd renderer", lw + 5))) {
            log_line("  X2-SMOOTH-001: HdCam_Set not confirmed - X2-SMOOTH-001-LASTWRITE off");
            g_lastwrite = 0;
            g_hdobj = nullptr;
        } else {
            log_line("  X2-SMOOTH-001-LASTWRITE: HdCam_Set gva=0x%08X (expected 0x00482320), hd object ptr gva=0x%08X (0x00DCDA04)",
                     rt_to_gva(set), rt_to_gva(g_hdobj));
        }
    }
    g_finalize = (finalize_t)fin;
    g_commit = (commit_t)commit;
    g_fov = config_row("X2-SMOOTH-001-FOV", 1);
    g_renderhold = config_row("X2-SMOOTH-001-RENDERHOLD", 1);
    g_cut_cos = cosf((float)config_row("X2-SMOOTH-001-CUTDEG", 20) * 0.017453293f);
    g_rhcut = config_row("X2-SMOOTH-001-RHCUT", 0);
    uint8_t* fs = g_fov ? sig_find("X2-SMOOTH-001-FOV setters",
        "8D 45 CC 50 E8 ?? ?? ?? ?? 8D 45 D0 50 E8 ?? ?? ?? ?? 83 C4 08 83 3D", 0x6504DF) : nullptr;
    if (g_fov) {
        uint8_t* sf = fs ? call_target(fs + 4) : nullptr;
        uint8_t* sa = fs ? call_target(fs + 0x0D) : nullptr;
        if (!fs || fs < fin || fs > fin + 0xA00 || !sf || !sa) {
            log_line("  X2-SMOOTH-001: FOV setters not confirmed - X2-SMOOTH-001-FOV off");
            g_fov = 0;
        } else {
            g_setfov = (fovset_t)sf;
            g_setasp = (fovset_t)sa;
            if (!patch_call_rel32(fs + 4, sf, (void*)&h_setfov) || !patch_call_rel32(fs + 0x0D, sa, (void*)&h_setasp)) {
                patch_call_rel32(fs + 4, (void*)&h_setfov, sf);
                log_line("  X2-SMOOTH-001: FOV setter patch failed - X2-SMOOTH-001-FOV off");
                g_fov = 0;
            }
        }
    }
    for (int i = 0; i < 3; i++) {
        if (!patch_call_rel32(sites[i], fin, (void*)&h_finalize)) {
            for (int j = 0; j < i; j++) patch_call_rel32(sites[j], (void*)&h_finalize, fin);
            log_line("  X2-SMOOTH-001: patch %d failed - refused", i);
            return false;
        }
    }
    log_line("  X2-SMOOTH-001: finalize gva=0x%08X (expected 0x0064FC10), CALLs gva=0x%08X/0x%08X/0x%08X -> stub; view gva=0x%08X "
             "(0x01ACCD80), src2 0x%08X, src3 0x%08X, debug 0x%08X, commit gva=0x%08X (0x00449960); FOV %s; F7 toggles",
             rt_to_gva(fin), rt_to_gva(sites[0]), rt_to_gva(sites[1]), rt_to_gva(sites[2]), rt_to_gva(g_view),
             rt_to_gva((const void*)g_src2), rt_to_gva((const void*)g_src3), rt_to_gva((const void*)g_dbg),
             rt_to_gva(commit), g_fov ? "ON" : "OFF");
    return true;
}

Row row_SMOOTH001 = {
    "X2-SMOOTH-001",
    "60 Hz camera: logic frames draw the view half-way between the last two ticks (needs X2-SMOOTH=1)",
    1, install, pre, post, 0
};

} // namespace x2
