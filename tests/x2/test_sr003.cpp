// SPDX-License-Identifier: Apache-2.0
// Tests for X2-SR-003 (soft reset): reading the button chord from the ini. The row's .cpp is included so its static
// helpers can be called; it is not compiled on its own in x2_tests.
#include "test.h"
#include "fakes.h"
#include "../../src/x2/rows/X2-SR-003.cpp"

using namespace x2;

TEST(sr003_default_chord) {
    CHECK_EQ(parse_chord("L1+L2+R1+R2+START+SELECT"), 0x090F);
}

TEST(sr003_chord_ignores_case_and_accepts_other_separators) {
    CHECK_EQ(parse_chord("l1, l2 r1+r2 start\tselect"), 0x090F);
}

TEST(sr003_chord_every_button) {
    CHECK_EQ(parse_chord("L2"), 0x0001);
    CHECK_EQ(parse_chord("R2"), 0x0002);
    CHECK_EQ(parse_chord("L1"), 0x0004);
    CHECK_EQ(parse_chord("R1"), 0x0008);
    CHECK_EQ(parse_chord("TRIANGLE"), 0x0010);
    CHECK_EQ(parse_chord("CIRCLE"), 0x0020);
    CHECK_EQ(parse_chord("CROSS"), 0x0040);
    CHECK_EQ(parse_chord("SQUARE"), 0x0080);
    CHECK_EQ(parse_chord("SELECT"), 0x0100);
    CHECK_EQ(parse_chord("L3"), 0x0200);
    CHECK_EQ(parse_chord("R3"), 0x0400);
    CHECK_EQ(parse_chord("START"), 0x0800);
    CHECK_EQ(parse_chord("UP"), 0x1000);
    CHECK_EQ(parse_chord("RIGHT"), 0x2000);
    CHECK_EQ(parse_chord("DOWN"), 0x4000);
    CHECK_EQ(parse_chord("LEFT"), 0x8000);
}

TEST(sr003_chord_raw_mask) {
    CHECK_EQ(parse_chord("0x090F"), 0x090F);
    CHECK_EQ(parse_chord("0X0003"), 0x0003);
}

TEST(sr003_chord_unknown_button_is_invalid) {
    fake::reset();
    CHECK_EQ(parse_chord("L1+JUMP"), 0);
    CHECK(fake::log_contains("unknown button 'JUMP'"));
}

TEST(sr003_chord_empty_is_invalid) {
    CHECK_EQ(parse_chord(""), 0);
    CHECK_EQ(parse_chord(" + , "), 0);
}

TEST(sr003_chord_repeated_button_counts_once) {
    CHECK_EQ(parse_chord("L1+L1+START"), 0x0804);
}
