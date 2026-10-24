// SPDX-License-Identifier: Apache-2.0
// The SMOOTH rows' shared helpers (x2 copy of the FFX mod's smooth_common.cpp; interface and timebase in x2_smooth.h):
// the X2-SMOOTH master switch, the F6 / F7 hotkeys, and the matrix maths used to draw cameras and actors half way
// between two ticks (inverse, multiply, interpolation with cut tests). Tested by tests/x2/test_smooth_math.cpp.
#include "x2_smooth.h"
#include <math.h>
#include <string.h>

namespace x2 {

static volatile LONG g_live = 1;
static volatile LONG g_off_row = 0;              // 0 = none, else X2-SMOOTH-00<n> alone off

bool smooth_master() { return config_row("X2-SMOOTH", 1) != 0; }

bool smooth_live(int row) { return g_live != 0 && (row == 0 || row != g_off_row); }

bool smooth_get_live() { return g_live != 0; }
void smooth_set_live(bool on) {
    if ((g_live != 0) == on) return;
    g_live = on ? 1 : 0;
    log_line("X2-CFG-MENU: SMOOTH rows %s", on ? "ON" : "OFF (installed, pass-through)");
}
void smooth_set_off_row(int row) { g_off_row = (row >= 0 && row <= 3) ? row : 0; }

static bool focused() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// F7 (game window focused): SMOOTH rows on/off at runtime, for an A/B look in the same scene.
// Several SMOOTH rows call this; the static edge state makes the extra calls no-ops.
void smooth_poll_hotkey() {
    // F6 (game window focused): one SMOOTH row off at a time, live: X2-SMOOTH-001 (camera) -> 002 (roots) ->
    // 003 (limbs) -> all on. U 13/10/2026: "ghost character when walking left/right": which row draws it.
    static bool prev6 = false;
    bool down6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (down6 && !prev6 && focused()) {
        g_off_row = (g_off_row + 1) % 4;
        static const char* const names[4] = { "none (all SMOOTH rows on)", "X2-SMOOTH-001 camera", "X2-SMOOTH-002 roots",
                                               "X2-SMOOTH-003 limbs" };
        log_line("F6: SMOOTH row off: %s%s", names[g_off_row], g_live ? "" : " (F7 has all SMOOTH rows off)");
    }
    prev6 = down6;
    static bool prev = false;
    bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    bool edge = down && !prev;
    prev = down;
    if (!edge || !focused()) return;
    g_live = g_live ? 0 : 1;
    log_line("F7: SMOOTH rows %s", g_live ? "ON" : "OFF (installed, pass-through)");
}

static inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// ---------------------------------------------------------------- interpolation
void smooth_mul(const float* a, const float* b, float* out) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

static bool is_affine(const float* m) {
    for (int i = 0; i < 16; i++) if (!isfinite(m[i])) return false;
    return fabsf(m[3]) < 1e-4f && fabsf(m[7]) < 1e-4f && fabsf(m[11]) < 1e-4f && fabsf(m[15] - 1.0f) < 1e-4f;
}

static float det3(const float* m) {     // rows 0..2, cols 0..2 of a 4x4
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}

bool smooth_affine_inverse(const float* m, float* out) {
    if (!is_affine(m)) return false;
    float d = det3(m);
    if (!(fabsf(d) > 1e-12f)) return false;
    float id = 1.0f / d, o[16];
    o[0] = (m[5] * m[10] - m[6] * m[9]) * id;
    o[1] = (m[2] * m[9] - m[1] * m[10]) * id;
    o[2] = (m[1] * m[6] - m[2] * m[5]) * id;
    o[4] = (m[6] * m[8] - m[4] * m[10]) * id;
    o[5] = (m[0] * m[10] - m[2] * m[8]) * id;
    o[6] = (m[2] * m[4] - m[0] * m[6]) * id;
    o[8] = (m[4] * m[9] - m[5] * m[8]) * id;
    o[9] = (m[1] * m[8] - m[0] * m[9]) * id;
    o[10] = (m[0] * m[5] - m[1] * m[4]) * id;
    o[3] = o[7] = o[11] = 0.0f;
    o[15] = 1.0f;
    for (int c = 0; c < 3; c++)                // translation row: -t . A^-1
        o[12 + c] = -(m[12] * o[0 * 4 + c] + m[13] * o[1 * 4 + c] + m[14] * o[2 * 4 + c]);
    for (int i = 0; i < 16; i++) { if (!isfinite(o[i])) return false; out[i] = o[i]; }
    return true;
}

// q = (x, y, z, w) of a row-normalised 3x3 R (R[i*3+j] = row i, col j). quat_to_mat is its exact
// inverse; slerp is convention-independent as long as the pair matches.
static void mat_to_quat(const float* R, float* q) {
    float tr = R[0] + R[4] + R[8];
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s; q[0] = (R[7] - R[5]) / s; q[1] = (R[2] - R[6]) / s; q[2] = (R[3] - R[1]) / s;
    } else if (R[0] > R[4] && R[0] > R[8]) {
        float s = sqrtf(1.0f + R[0] - R[4] - R[8]) * 2.0f;
        q[3] = (R[7] - R[5]) / s; q[0] = 0.25f * s; q[1] = (R[1] + R[3]) / s; q[2] = (R[2] + R[6]) / s;
    } else if (R[4] > R[8]) {
        float s = sqrtf(1.0f + R[4] - R[0] - R[8]) * 2.0f;
        q[3] = (R[2] - R[6]) / s; q[0] = (R[1] + R[3]) / s; q[1] = 0.25f * s; q[2] = (R[5] + R[7]) / s;
    } else {
        float s = sqrtf(1.0f + R[8] - R[0] - R[4]) * 2.0f;
        q[3] = (R[3] - R[1]) / s; q[0] = (R[2] + R[6]) / s; q[1] = (R[5] + R[7]) / s; q[2] = 0.25f * s;
    }
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= n;
}

static void quat_to_mat(const float* q, float* R) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    R[0] = 1 - 2 * (y * y + z * z); R[1] = 2 * (x * y - z * w);     R[2] = 2 * (x * z + y * w);
    R[3] = 2 * (x * y + z * w);     R[4] = 1 - 2 * (x * x + z * z); R[5] = 2 * (y * z - x * w);
    R[6] = 2 * (x * z - y * w);     R[7] = 2 * (y * z + x * w);     R[8] = 1 - 2 * (x * x + y * y);
}

bool smooth_interp(const float* a, const float* b, float t, float cut_dist, float* out) {
    if (!is_affine(a) || !is_affine(b)) return false;
    float d[3] = { b[12] - a[12], b[13] - a[13], b[14] - a[14] };
    if (dot3(d, d) > cut_dist * cut_dist) return false;           // warp / camera cut: no sweep
    float la[3], lb[3], A[9], B[9];
    for (int r = 0; r < 3; r++) {
        la[r] = sqrtf(dot3(a + r * 4, a + r * 4));
        lb[r] = sqrtf(dot3(b + r * 4, b + r * 4));
        if (!(la[r] > 1e-6f) || !(lb[r] > 1e-6f)) return false;
        for (int c = 0; c < 3; c++) { A[r * 3 + c] = a[r * 4 + c] / la[r]; B[r * 3 + c] = b[r * 4 + c] / lb[r]; }
    }
    bool ma = det3(a) < 0.0f, mb = det3(b) < 0.0f;
    if (ma != mb) return false;
    if (ma) for (int c = 0; c < 3; c++) { A[6 + c] = -A[6 + c]; B[6 + c] = -B[6 + c]; }  // mirrored -> proper rotation
    float qa[4], qb[4];
    mat_to_quat(A, qa);
    mat_to_quat(B, qb);
    float c = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (c < 0.0f) { c = -c; for (int i = 0; i < 4; i++) qb[i] = -qb[i]; }
    if (c < 0.8660254f) return false;                             // > 60 degrees in one tick: a cut
    float wa, wb;
    if (c > 0.9995f) { wa = 1.0f - t; wb = t; }
    else {
        float th = acosf(c), s = sinf(th);
        wa = sinf((1.0f - t) * th) / s;
        wb = sinf(t * th) / s;
    }
    float q[4], n = 0.0f;
    for (int i = 0; i < 4; i++) { q[i] = wa * qa[i] + wb * qb[i]; n += q[i] * q[i]; }
    n = sqrtf(n);
    if (!(n > 1e-6f)) return false;
    for (int i = 0; i < 4; i++) q[i] /= n;
    float R[9];
    quat_to_mat(q, R);
    if (ma) for (int k = 0; k < 3; k++) R[6 + k] = -R[6 + k];
    float o[16];
    for (int r = 0; r < 3; r++) {
        float l = la[r] + t * (lb[r] - la[r]);
        for (int k = 0; k < 3; k++) o[r * 4 + k] = R[r * 3 + k] * l;
        o[r * 4 + 3] = b[r * 4 + 3];
    }
    for (int k = 0; k < 3; k++) o[12 + k] = a[12 + k] + t * d[k];
    o[15] = b[15];
    for (int i = 0; i < 16; i++) { if (!isfinite(o[i])) return false; out[i] = o[i]; }
    return true;
}

bool smooth_interp_view(const float* a, const float* b, float t, float cut_dist, float* out) {
    float ia[16], ib[16], io[16], o[16];
    if (!smooth_affine_inverse(a, ia) || !smooth_affine_inverse(b, ib)) return false;
    if (!smooth_interp(ia, ib, t, cut_dist, io)) return false;
    if (!smooth_affine_inverse(io, o)) return false;
    memcpy(out, o, 64);
    return true;
}

} // namespace x2
