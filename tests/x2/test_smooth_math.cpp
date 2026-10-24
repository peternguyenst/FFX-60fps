// SPDX-License-Identifier: Apache-2.0
// Tests for the shared SMOOTH maths (rows/x2_smooth_common.cpp): matrix inverse, multiply and the interpolation the
// camera and actor rows use. Matrices are row-vector, translation in row 3.
#include "test.h"
#include "fakes.h"
#include "../../src/x2/rows/x2_smooth.h"
#include <math.h>

using namespace x2;

static void identity(float* m) {
    for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
}
static void yaw(float* m, float rad, float x, float y, float z) {   // rotation about Y, then translation
    identity(m);
    m[0] = cosf(rad);  m[2] = -sinf(rad);
    m[8] = sinf(rad);  m[10] = cosf(rad);
    m[12] = x; m[13] = y; m[14] = z;
}
static bool close_to(float a, float b, float eps = 1e-4f) { return fabsf(a - b) < eps; }

TEST(smooth_math_inverse_times_matrix_is_identity) {
    float m[16], inv[16], prod[16], id[16];
    yaw(m, 0.7f, 10, -3, 25);
    CHECK(smooth_affine_inverse(m, inv));
    smooth_mul(m, inv, prod);
    identity(id);
    for (int i = 0; i < 16; i++) CHECK(close_to(prod[i], id[i]));
}

TEST(smooth_math_singular_matrix_has_no_inverse) {
    float m[16] = {}, inv[16];
    m[15] = 1.0f;
    CHECK(!smooth_affine_inverse(m, inv));
}

TEST(smooth_math_interp_halfway_translation_and_turn) {
    float a[16], b[16], out[16];
    yaw(a, 0.0f, 0, 0, 0);
    yaw(b, 0.4f, 10, 0, 20);
    CHECK(smooth_interp(a, b, 0.5f, SMOOTH_CUT_DIST, out));
    float expect[16];
    yaw(expect, 0.2f, 5, 0, 10);
    for (int i = 0; i < 16; i++) CHECK(close_to(out[i], expect[i], 1e-3f));
}

TEST(smooth_math_interp_cuts_a_teleport) {
    float a[16], b[16], out[16];
    yaw(a, 0.0f, 0, 0, 0);
    yaw(b, 0.0f, SMOOTH_CUT_DIST + 1.0f, 0, 0);
    out[0] = 123.0f;
    CHECK(!smooth_interp(a, b, 0.5f, SMOOTH_CUT_DIST, out));
    CHECK(out[0] == 123.0f);                          // untouched on a cut
}

TEST(smooth_math_interp_cuts_a_big_turn) {
    float a[16], b[16], out[16];
    yaw(a, 0.0f, 0, 0, 0);
    yaw(b, 1.2f, 0, 0, 0);                            // about 69 degrees: more than the 60 degree limit
    CHECK(!smooth_interp(a, b, 0.5f, SMOOTH_CUT_DIST, out));
}

TEST(smooth_math_interp_view_moves_the_eye_halfway) {
    // a view matrix is the inverse of the camera's transform: interpolate two cameras 10 units apart
    float cam_a[16], cam_b[16], view_a[16], view_b[16], out[16], cam_out[16];
    yaw(cam_a, 0.0f, 0, 2, 0);
    yaw(cam_b, 0.0f, 10, 2, 0);
    CHECK(smooth_affine_inverse(cam_a, view_a));
    CHECK(smooth_affine_inverse(cam_b, view_b));
    CHECK(smooth_interp_view(view_a, view_b, 0.5f, SMOOTH_CUT_DIST, out));
    CHECK(smooth_affine_inverse(out, cam_out));
    CHECK(close_to(cam_out[12], 5.0f) && close_to(cam_out[13], 2.0f) && close_to(cam_out[14], 0.0f));
}
