// SPDX-License-Identifier: Apache-2.0
// Tests for X2-SAVE-001 (completion with its decimal on the save / load list): the completion read from a save file
// and the decimal added to the formatted text. The row's .cpp is included so its static helpers can be called.
#include "test.h"
#include "fakes.h"
#include "../../src/x2/rows/X2-SAVE-001.cpp"
#include <vector>

using namespace x2;

// A save file with `total` events, `not_seen` of them still set in the event table, and the header's whole percent.
static std::vector<uint8_t> make_save(int32_t total, int not_seen, uint8_t header_percent) {
    std::vector<uint8_t> f(READ_LEN, 0);
    memcpy(&f[EVENT_TOTAL], &total, sizeof total);
    for (int i = 0; i < not_seen; ++i) f[EVENT_BITS + i / 8] |= (uint8_t)(1u << (i % 8));
    f[HEADER_COMPLETION] = header_percent;
    return f;
}

TEST(save001_tenths_from_real_saves) {
    // values from the user's saves (24/10): ffx2_037 14.6%, ffx2_000 22.4%, ffx2_002 4.0%
    std::vector<uint8_t> a = make_save(525, 452, 14), b = make_save(525, 413, 22), c = make_save(525, 505, 4);
    CHECK_EQ(completion_tenths(a.data(), a.size()), 146);
    CHECK_EQ(completion_tenths(b.data(), b.size()), 224);
    CHECK_EQ(completion_tenths(c.data(), c.size()), 40);
}

TEST(save001_header_disagrees_or_short_file) {
    std::vector<uint8_t> f = make_save(525, 452, 15);
    CHECK_EQ(completion_tenths(f.data(), f.size()), -1);
    f = make_save(525, 452, 14);
    CHECK_EQ(completion_tenths(f.data(), f.size() - 1), -1);
    f = make_save(0, 0, 100);
    CHECK_EQ(completion_tenths(f.data(), f.size()), -1);
}

TEST(save001_capped_at_100) {
    std::vector<uint8_t> f = make_save(525, 0, 100);   // 105% by the formula; the game caps the header at 100
    CHECK_EQ(completion_tenths(f.data(), f.size()), FULL_TENTHS);
}

TEST(save001_adds_tenths_after_the_number) {
    const char text[] = "Completion \x05" "0%";     // placeholder 05 30
    char out[0x80] = "Completion 16%";
    CHECK(add_tenths_digit(text, out, sizeof out, 16, 4));
    CHECK(strcmp(out, "Completion 16\x48\x34%") == 0);
}

TEST(save001_number_at_start_or_end) {
    char out[0x80] = "7%";
    CHECK(add_tenths_digit("\x05" "0%", out, sizeof out, 7, 2));
    CHECK(strcmp(out, "7\x48\x32%") == 0);
    char out2[0x80] = "Done: 100";
    CHECK(add_tenths_digit("Done: \x05" "0", out2, sizeof out2, 100, 0));
    CHECK(strcmp(out2, "Done: 100\x48\x30") == 0);
}

TEST(save001_leaves_text_alone_when_unsure) {
    char out[0x80] = "Completion 16%";
    CHECK(!add_tenths_digit("Completion \x05" "0%", out, sizeof out, 17, 4));   // number not where expected
    CHECK(!add_tenths_digit("Completion \x05" "0%", out, sizeof out, 16, 10));  // not a digit
    CHECK(strcmp(out, "Completion 16%") == 0);
    char small[16] = "Completion 16%";                                         // 14 chars + 2 does not fit
    CHECK(!add_tenths_digit("Completion \x05" "0%", small, sizeof small, 16, 4));
    CHECK(strcmp(small, "Completion 16%") == 0);
}
