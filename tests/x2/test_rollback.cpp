// SPDX-License-Identifier: Apache-2.0
// Tests for the rollback engine (x2_rollback.cpp): spans copied out before a render-only invocation and back after it.
#include "test.h"
#include "fakes.h"
#include <string.h>

using namespace x2;

// Put the pacing core into the next logic or render-only invocation (see test_pacing.cpp).
static void start_pacing() {
    g_cfg.pacing = 0;
    pacing_pre();
    pacing_turbo(false);
    pacing_menudiv2_set(false);
    g_cfg.pacing = 1;
    g_divider_hooked = true;
    fake::backlog_count = 0;
    fake::divider = 2;
    pacing_divider_request(2);
}
static void logic_invocation() { start_pacing(); pacing_pre(); }                      // first one is logic
static void render_only_invocation() { start_pacing(); pacing_pre(); pacing_pre(); }   // the second is render-only

TEST(rollback_undoes_changes_made_on_a_render_only_invocation) {
    static uint8_t block[16];
    memset(block, 1, sizeof(block));
    CHECK(rollback_add_static("T-STATIC", block, sizeof(block)));
    render_only_invocation();
    CHECK_EQ(pacing_kind(), INV_RENDER_ONLY);
    rollback_pre();
    memset(block, 9, sizeof(block));                 // the game changes the block during the invocation
    rollback_post();
    CHECK_EQ(block[0], 1);
    CHECK_EQ(block[15], 1);
}

TEST(rollback_leaves_logic_invocations_alone) {
    static uint8_t block[8];
    memset(block, 1, sizeof(block));
    CHECK(rollback_add_static("T-LOGIC", block, sizeof(block)));
    logic_invocation();
    CHECK_EQ(pacing_kind(), INV_LOGIC);
    rollback_pre();
    memset(block, 9, sizeof(block));
    rollback_post();
    CHECK_EQ(block[0], 9);                            // logic changes stay
}

TEST(rollback_skips_spans_of_a_row_that_is_off) {
    static uint8_t block[8];
    memset(block, 1, sizeof(block));
    CHECK(rollback_add_static("T-OFF", block, sizeof(block)));
    fake::rows_off = { "T-OFF" };
    render_only_invocation();
    rollback_pre();
    memset(block, 9, sizeof(block));
    rollback_post();
    CHECK_EQ(block[0], 9);
    fake::rows_off.clear();
}

// A dynamic span: the resolver says where the block is this frame (or that it is not there).
struct Where { uint8_t* base; uint32_t len; bool present; };
static bool resolve(void* ctx, uint8_t** base, uint32_t* len) {
    Where* w = (Where*)ctx;
    if (!w->present) return false;
    *base = w->base;
    *len = w->len;
    return true;
}

TEST(rollback_dynamic_span_follows_the_resolver_and_caps_the_length) {
    static uint8_t block[32];
    memset(block, 1, sizeof(block));
    static Where w = { block, 32, true };            // asks for 32 bytes, only 16 registered
    CHECK(rollback_add_dynamic("T-DYN", resolve, &w, 16));
    render_only_invocation();
    rollback_pre();
    memset(block, 9, sizeof(block));
    rollback_post();
    CHECK_EQ(block[0], 1);
    CHECK_EQ(block[15], 1);
    CHECK_EQ(block[16], 9);                           // past the registered maximum: not rolled back
    w.present = false;                                // the block is gone this frame
    memset(block, 1, sizeof(block));
    rollback_pre();
    memset(block, 9, sizeof(block));
    rollback_post();
    CHECK_EQ(block[0], 9);
}

TEST(rollback_rejects_bad_spans) {
    fake::reset();
    static uint8_t block[4];
    CHECK(!rollback_add_static("T-NULL", nullptr, 4));
    CHECK(!rollback_add_static("T-ZERO", block, 0));
    CHECK(!rollback_add_static("T-HUGE", block, (16u << 20) + 1));
    CHECK(!rollback_add_dynamic("T-NOFN", nullptr, nullptr, 4));
}

TEST(rollback_disables_a_span_that_faults) {
    fake::reset();
    uint8_t* page = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(rollback_add_static("T-FAULT", page, 64));
    DWORD old = 0;
    VirtualProtect(page, 4096, PAGE_NOACCESS, &old);  // the memory goes away
    render_only_invocation();
    rollback_pre();
    rollback_post();
    CHECK(fake::log_contains("T-FAULT span"));
    CHECK(fake::log_contains("faulted on snapshot - disabled"));
    fake::log.clear();
    VirtualProtect(page, 4096, PAGE_READWRITE, &old);
    rollback_pre();                                   // disabled: not tried again
    rollback_post();
    CHECK(!fake::log_contains("T-FAULT"));
    VirtualFree(page, 0, MEM_RELEASE);
}
