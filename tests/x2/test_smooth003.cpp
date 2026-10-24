// SPDX-License-Identifier: Apache-2.0
// Tests for X2-SMOOTH-003's helpers that run on plain memory: the loop-wrap guard, the angle maths, the short-loop and
// slot tests, and the loop-end extrapolation. The row's .cpp is included so its static helpers can be called; it is
// not compiled on its own in x2_tests.
#include "test.h"
#include "fakes.h"
#include "../../src/x2/rows/X2-SMOOTH-003.cpp"
#include <math.h>
#include <vector>

using namespace x2;

// A zeroed actor record with a motion set up.
struct TestActor {
    std::vector<uint8_t> mem = std::vector<uint8_t>(ACTOR_SIZE, 0);
    uint8_t* p() { return mem.data(); }
    template <class T> T& at(uint32_t off) { return *(T*)(p() + off); }
    void motion(int32_t start, int32_t end, int32_t loop_len, int16_t loops) {
        at<uint32_t>(OFF_MOTION_SEQ) = 1;
        at<int32_t>(OFF_MOTION_START) = start;
        at<int32_t>(OFF_MOTION_END) = end;
        at<int32_t>(OFF_MOTION_LOOP_LEN) = loop_len;
        at<int16_t>(OFF_MOTION_LOOPS) = loops;
    }
};

static bool close_to(float a, float b) { return fabsf(a - b) < 1e-4f; }

// --- guard_clock: where would the logic step sample for clock c? (clock in 1/256 frames) ---

TEST(smooth003_guard_clock_inside_the_motion_is_unchanged) {
    TestActor a;
    a.motion(0, 0x500, 0x400, 0);
    int kind = -1;
    CHECK_EQ(guard_clock(a.p(), 0x300, &kind), 0x300);
    CHECK_EQ(kind, 0);
}

TEST(smooth003_guard_clock_wraps_a_loop_back_by_its_length) {
    TestActor a;
    a.motion(0, 0x500, 0x400, 0);
    int kind = -1;
    CHECK_EQ(guard_clock(a.p(), 0x580, &kind), 0x180);
    CHECK_EQ(kind, 1);
}

TEST(smooth003_guard_clock_wraps_several_lengths) {
    TestActor a;
    a.motion(0, 0x500, 0x100, 0);
    int kind = -1;
    CHECK_EQ(guard_clock(a.p(), 0x880, &kind), 0x480);
    CHECK_EQ(kind, 1);
}

TEST(smooth003_guard_clock_clamps_a_play_once_motion_at_its_last_whole_frame) {
    TestActor a;
    a.motion(0, 0x5C0, 0x400, 1);                    // play once: last whole frame = 0x500
    int kind = -1;
    CHECK_EQ(guard_clock(a.p(), 0x580, &kind), 0x500);
    CHECK_EQ(kind, 2);
    CHECK_EQ(guard_clock(a.p(), 0x480, &kind), 0x480);
    CHECK_EQ(kind, 0);
}

TEST(smooth003_guard_clock_negative_clock_wraps_to_zero) {
    TestActor a;
    a.motion(0, 0x500, 0x400, 0);
    int kind = -1;
    CHECK_EQ(guard_clock(a.p(), -5, &kind), 0);
    CHECK_EQ(kind, 1);
}

TEST(smooth003_guard_clock_zero_length_loop_restarts_at_the_start) {
    TestActor a;
    a.motion(0x40, 0x500, 0, 0);
    int kind = -1;
    CHECK_EQ(guard_clock(a.p(), 0x600, &kind), 0x40);
    CHECK_EQ(kind, 1);
}

TEST(smooth003_guard_clock_no_stock_equivalent_holds) {
    TestActor a;
    a.motion(0, 0x500, 0, 3);                         // zero length, counted loops: the stock step has no answer
    int kind = -1;
    guard_clock(a.p(), 0x600, &kind);
    CHECK_EQ(kind, 3);
    a.motion(0, 0, 1, 0);                             // would need more than 64 wraps
    guard_clock(a.p(), 0x10000, &kind);
    CHECK_EQ(kind, 3);
}

// --- turn_diff: size of the shortest turn, in turns ---

TEST(smooth003_turn_diff_takes_the_short_way_round) {
    CHECK(close_to(turn_diff(0.2f, 0.1f), 0.1f));
    CHECK(close_to(turn_diff(0.1f, 0.2f), 0.1f));
    CHECK(close_to(turn_diff(0.95f, 0.05f), 0.1f));      // across a whole turn
    CHECK(close_to(turn_diff(2.3f, 0.2f), 0.1f));        // several turns apart
    CHECK(close_to(turn_diff(0.1f, 0.6f), 0.5f));        // exactly opposite
}

// --- short_loop: animations one frame long run as stock ---

TEST(smooth003_short_loop) {
    TestActor a;
    a.motion(0, 0x100, 0x100, 0);
    CHECK(short_loop(a.p()));
    a.motion(0, 0x200, 0x200, 0);
    CHECK(!short_loop(a.p()));                        // longer than a frame
    a.motion(0, 0x100, 0x100, 1);
    CHECK(!short_loop(a.p()));                        // play once
    a.motion(0, 0x100, 0x100, 0);
    a.at<uint32_t>(OFF_FLAGS) = FLAG_SCRIPTED;
    CHECK(!short_loop(a.p()));                        // script-driven
    a.at<uint32_t>(OFF_FLAGS) = 0;
    a.at<uint32_t>(OFF_MOTION_SEQ) = 0;
    CHECK(!short_loop(a.p()));                        // no motion
}

// --- slot_of: actor pointer -> slot index ---

TEST(smooth003_slot_of) {
    std::vector<uint8_t> table(ACTOR_SIZE * 4);
    uint8_t* base = table.data();
    CHECK_EQ(slot_of(base, base, 4), 0);
    CHECK_EQ(slot_of(base + 2 * ACTOR_SIZE, base, 4), 2);
    CHECK_EQ(slot_of(base + 2 * ACTOR_SIZE + 8, base, 4), -1);   // not at a record start
    CHECK_EQ(slot_of(base + 3 * ACTOR_SIZE, base, 3), -1);       // past the slots in use
    CHECK_EQ(slot_of(base, nullptr, 4), -1);
}

// --- ENDEXTRAP: continue each bone half a tick past tick k ---

struct BonedActor : TestActor {
    std::vector<uint8_t> header = std::vector<uint8_t>(16, 0);
    std::vector<uint8_t> bones;
    explicit BonedActor(int nbones) : bones((size_t)nbones * BONE_SIZE, 0) {
        at<uint16_t>(0) = 0x1234;                      // actor id
        *(uint16_t*)(header.data() + MODEL_HEADER_BONE_COUNT) = (uint16_t)nbones;
        at<uint8_t*>(OFF_MODEL_HEADER) = header.data();
        at<uint8_t*>(OFF_BONES) = bones.data();
    }
    float& angle(int bone, int ch) { return *(float*)(bones.data() + bone * BONE_SIZE + BONE_ANGLE[ch]); }
    float& mirror(int bone, int ch) { return *(float*)(bones.data() + bone * BONE_SIZE + BONE_ANGLE[ch] + 0x30); }
};

static void angle_buffers() {
    if (!g_cur_ang) {
        g_prev_ang = (float*)calloc((size_t)MAX_ACTORS * MAX_BONES * 3, sizeof(float));
        g_cur_ang = (float*)calloc((size_t)MAX_ACTORS * MAX_BONES * 3, sizeof(float));
    }
}

TEST(smooth003_end_extrapolate_continues_half_a_tick) {
    angle_buffers();
    BonedActor a(2);
    a.angle(0, 0) = 0.10f;                            // tick k-1
    a.angle(1, 0) = 0.0f;
    a.angle(1, 1) = 6.2f;
    record_angles(5, a.p());
    a.angle(0, 0) = 0.20f;                            // tick k
    a.angle(1, 0) = 1.0f;                             // moved 0.16 turn in one tick: a cut, not continued
    a.angle(1, 1) = 0.05f;                            // crossed 2*pi going up
    record_angles(5, a.p());
    end_extrapolate(5, a.p());
    CHECK(close_to(a.angle(0, 0), 0.25f));
    CHECK(close_to(a.mirror(0, 0), 0.25f));
    CHECK(close_to(a.angle(1, 0), 1.0f));
    CHECK(close_to(a.angle(1, 1), 0.05f + 0.5f * (0.05f + 6.2831853f - 6.2f)));
}

TEST(smooth003_end_extrapolate_needs_two_ticks_of_the_same_actor) {
    angle_buffers();
    BonedActor a(1);
    a.angle(0, 0) = 0.10f;
    record_angles(6, a.p());
    end_extrapolate(6, a.p());                        // only one tick recorded
    CHECK(close_to(a.angle(0, 0), 0.10f));
    a.angle(0, 0) = 0.20f;
    record_angles(6, a.p());
    a.at<uint16_t>(0) = 0x9999;                       // the slot now holds another actor
    end_extrapolate(6, a.p());
    CHECK(close_to(a.angle(0, 0), 0.20f));
}

// MIDCHECK: a render-only sample is bad when it is far from the half-way angle between the two logic ticks.
TEST(smooth003_midcheck_quarter_turn_is_bad) {
    float err = 0;
    CHECK(midcheck_bad(0.25f, 0.0f, 0.25f, &err));                  // the log's bone 130: keys agree, sample 0
    CHECK(err > 0.24f && err < 0.26f);
}

TEST(smooth003_midcheck_half_way_is_good) {
    CHECK(!midcheck_bad(0.10f, 0.15f, 0.20f, nullptr));
    CHECK(!midcheck_bad(0.10f, 0.11f, 0.10f, nullptr));             // within MIDCHECK_MIN_ERR
    CHECK(!midcheck_bad(0.0f, 0.15f, 0.40f, nullptr));              // fast bone: within half its own movement
}

TEST(smooth003_midcheck_wraps_around_a_turn) {
    CHECK(!midcheck_bad(0.48f, -0.49f, -0.46f, nullptr));           // 0.48 -> 0.54 (= -0.46); mid 0.51 = -0.49
    CHECK(midcheck_bad(0.48f, 0.0f, -0.46f, nullptr));
}

// SANITY puts back the whole bone when one of its angles jumps (the log's id 0x001A bone 14: ch 0 0.25 -> 0.5 and ch 2
// 0.1393 -> -0.1106 at the half sample, keys in the other equivalent angle triple).
TEST(smooth003_sanity_restores_all_three_angles) {
    const int slot = 3, nb = 2;
    std::vector<uint8_t> actor(0x910, 0), header(0x40, 0), bones(nb * BONE_SIZE, 0);
    *(uint16_t*)&actor[0] = 0x001A;
    *(uint8_t**)&actor[OFF_MODEL_HEADER] = header.data();
    *(uint16_t*)&header[MODEL_HEADER_BONE_COUNT] = nb;
    *(uint8_t**)&actor[OFF_BONES] = bones.data();
    std::vector<float> prev(MAX_ACTORS * MAX_BONES * 3, 0), cur(MAX_ACTORS * MAX_BONES * 3, 0);
    float* old_prev = g_prev_ang; float* old_cur = g_cur_ang;
    g_prev_ang = prev.data(); g_cur_ang = cur.data();
    g_angles_ticks[slot] = 2; g_angles_actor_id[slot] = 0x001A;
    const float k[3] = { 0.25f, 0.20f, 0.1393f }, half[3] = { 0.5f, 0.21f, -0.1106f };
    float* p = prev.data() + (size_t)slot * MAX_BONES * 3; float* c = cur.data() + (size_t)slot * MAX_BONES * 3;
    for (int ch = 0; ch < 3; ch++) {
        p[3 + ch] = c[3 + ch] = k[ch];                                         // bone 1 still over the last tick
        *(float*)&g_sanity_bone_save[BONE_SIZE + BONE_ANGLE[ch]] = k[ch] / TURNS_PER_RADIAN;
        *(float*)&bones[BONE_SIZE + BONE_ANGLE[ch]] = half[ch] / TURNS_PER_RADIAN;
    }
    sanity_check(slot, actor.data(), nb, 0);
    for (int ch = 0; ch < 3; ch++)
        CHECK(fabsf(*(float*)&bones[BONE_SIZE + BONE_ANGLE[ch]] - k[ch] / TURNS_PER_RADIAN) < 1e-5f);   // ch 1 too
    g_prev_ang = old_prev; g_cur_ang = old_cur;
}

// DRAWCHECK: over three drawn frames, a bone far from half way between its neighbours is logged; an actor whose bone
// array cannot be read is skipped (and logged) while the others are still checked.
TEST(smooth003_drawcheck_logs_odd_bone_and_skips_unreadable_actor) {
    fake::reset();
    const int nb = 2;
    std::vector<uint8_t> table(2 * ACTOR_SIZE, 0), header(0x40, 0), bones(nb * BONE_SIZE, 0);
    uint8_t* good = table.data();
    uint8_t* bad = table.data() + ACTOR_SIZE;
    *(uint16_t*)good = 0x001A; good[OFF_ACTIVE] = 1;
    *(uint8_t**)(good + OFF_MODEL_HEADER) = header.data();
    *(uint16_t*)&header[MODEL_HEADER_BONE_COUNT] = nb;
    *(uint8_t**)(good + OFF_BONES) = bones.data();
    *(uint16_t*)bad = 0x00C9; bad[OFF_ACTIVE] = 1;
    *(uint8_t**)(bad + OFF_MODEL_HEADER) = header.data();
    *(uintptr_t*)(bad + OFF_BONES) = 0x10;                       // freed / garbage: reading it faults
    uint8_t* table_ptr = table.data(); int32_t count = 2;
    uint8_t* volatile* old_table = g_actor_table; volatile int32_t* old_count = g_actor_count; float* old_ang = g_dc_ang; float* old_pos = g_dc_pos;
    g_actor_table = (uint8_t* volatile*)&table_ptr; g_actor_count = &count;
    std::vector<float> ang((size_t)3 * DC_ACTORS * DC_BONES * 3, 0);
    std::vector<float> pos((size_t)3 * DC_ACTORS * DC_BONES * 3, 0);
    g_dc_ang = ang.data(); g_dc_pos = pos.data(); g_dc_frames = 0; g_dc_head = 0; g_dc_window = 0; g_dc_window_lines = 0; g_opt_drawcheck = 1;
    float* bone1_ch0 = (float*)&bones[BONE_SIZE + BONE_ANGLE[0]];
    const float frames[3] = { 0.10f, 0.40f, 0.12f };              // bone 1 sticks out on the middle frame
    // bone 0 flips to the other equivalent triple of the same rotation on the middle frame: not odd
    float* bone0 = (float*)&bones[BONE_ANGLE[0]];
    const float same[3][3] = { { 0.0f, 0.02f, 0.1f }, { 0.5f, 0.48f, 0.6f }, { 0.0f, 0.02f, 0.1f } };
    int kinds[3] = { INV_LOGIC, INV_RENDER_ONLY, INV_LOGIC };
    // bone 1 is drawn 10 units from bone 0, and 3 units off to the side on the middle frame
    float* bone1_world = (float*)&bones[BONE_SIZE + BONE_WORLD_T];
    const float side[3] = { 0.0f, 3.0f, 0.0f };
    for (int f = 0; f < 3; f++) {
        *bone1_ch0 = frames[f] / TURNS_PER_RADIAN;
        bone1_world[0] = 10.0f; bone1_world[1] = side[f]; bone1_world[2] = 0.0f;
        for (int c = 0; c < 3; c++) bone0[c] = same[f][c] / TURNS_PER_RADIAN;
        drawcheck_post(INV_BACKLOG);                                 // not drawn: ignored
        drawcheck_post(kinds[f]);
    }
    CHECK(fake::log_contains("render-only frame, slot=0 id=0x001A bone 1 odd by"));
    drawcheck_report();
    CHECK(fake::log_contains("angles: odd bone frames render-only 1, logic 0; most often (id bone render-only/logic): 0x001A b1 1/0"));
    CHECK(fake::log_contains("world: odd bone frames render-only 1, logic 0; most often (id bone render-only/logic): 0x001A b1 1/0"));
    CHECK(fake::log_contains("id=0x001A bone 1 odd by"));
    CHECK(!fake::log_contains("id=0x001A bone 0 odd by"));
    CHECK(fake::log_contains("slot=1 id=0x00C9 unreadable"));
    g_actor_table = old_table; g_actor_count = old_count; g_dc_ang = old_ang; g_dc_pos = old_pos; g_dc_frames = 0;
}

// MESHCHECK: a cloth vertex drawn off to the side on the middle frame is odd; a mesh moving steadily is not.
TEST(smooth003_meshcheck_finds_a_vertex_off_on_the_middle_frame) {
    const uint32_t n = 3;
    float v0[n * 4] = { 0, 0, 0, 1,  10, 0, 0, 1,  0, 10, 0, 1 };
    float v1[n * 4] = { 1, 0, 0, 1,  11, 0, 0, 1,  1, 10, 0, 1 };   // moved 1 to the side
    float v2[n * 4] = { 2, 0, 0, 1,  12, 0, 0, 1,  2, 10, 0, 1 };
    float worst = 0;
    CHECK_EQ(mesh_odd(v0, v1, v2, n, &worst), 0);
    v1[2 * 4 + 2] = 4.0f;                                          // vertex 2 sticks out by 4 on the middle frame
    CHECK_EQ(mesh_odd(v0, v1, v2, n, &worst), 1);
    CHECK(worst > 0.2f && worst < 0.4f);
}
