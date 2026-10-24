// SPDX-License-Identifier: Apache-2.0
// Tests for x2_pacing.cpp, the 60 Hz core. The rule under test: keep the game's frame divider at 1 and flip the exec
// flag 1/0 on every invocation, so invocations alternate between logic (game runs and draws) and render-only (draw
// again). The game's own 30 Hz requests are swallowed; anything that needs stock pacing gets it back.
#include "test.h"
#include "fakes.h"

using namespace x2;

// Every test starts from the same state: 60 Hz on, the game at its normal divider 2, nothing holding stock pacing.
static void fresh() {
    g_cfg.pacing = 0;
    pacing_pre();                      // leaves 60 Hz if a previous test left it on (alternation back to logic first)
    pacing_turbo(false);
    pacing_menudiv2_set(false);
    g_cfg.pacing = 1;
    g_divider_hooked = true;
    fake::backlog_count = 0;
    fake::divider = 2;
    fake::exec_byte = 1;
    pacing_divider_request(2);         // the game asks for 30 Hz, as it does at boot
    fake::reset();
}

static int run_one() {
    pacing_pre();
    return pacing_kind();
}

TEST(pacing_swallows_the_games_30hz_request) {
    fresh();
    CHECK(pacing_divider_request(2));
}

TEST(pacing_lets_a_60hz_request_through_and_stays_out) {
    fresh();
    CHECK(!pacing_divider_request(1));
    CHECK_EQ(run_one(), INV_STOCK);    // the game already runs every vsync: nothing to do
}

TEST(pacing_off_lets_everything_through) {
    fresh();
    g_cfg.pacing = 0;
    CHECK(!pacing_divider_request(2));
    CHECK_EQ(run_one(), INV_STOCK);
    g_cfg.pacing = 1;
}

TEST(pacing_alternates_logic_and_render_only) {
    fresh();
    const int expected_kind[] = { INV_LOGIC, INV_RENDER_ONLY, INV_LOGIC, INV_RENDER_ONLY, INV_LOGIC, INV_RENDER_ONLY };
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(run_one(), expected_kind[i]);
        CHECK_EQ(fake::exec_byte, expected_kind[i] == INV_LOGIC ? 1 : 0);
        CHECK_EQ(render_only(), expected_kind[i] == INV_RENDER_ONLY);
    }
    CHECK_EQ(fake::divider, 1);
    CHECK_EQ(fake::divider_calls.size(), 1);   // set once, not every invocation
}

TEST(pacing_backlog_invocations_run_logic_without_breaking_the_alternation) {
    fresh();
    CHECK_EQ(run_one(), INV_LOGIC);
    fake::backlog_count = 2;                    // the game is behind and runs a catch-up invocation
    CHECK_EQ(run_one(), INV_BACKLOG);
    CHECK_EQ(fake::exec_byte, 1);
    fake::backlog_count = 0;
    CHECK_EQ(run_one(), INV_RENDER_ONLY);             // the alternation resumes where it was
    CHECK_EQ(run_one(), INV_LOGIC);
}

TEST(pacing_turbo_runs_logic_on_every_invocation) {
    fresh();
    pacing_turbo(true);
    for (int i = 0; i < 4; i++) CHECK_EQ(run_one(), INV_LOGIC);
    pacing_turbo(false);
    CHECK_EQ(run_one(), INV_LOGIC);             // after turbo the alternation restarts with logic
    CHECK_EQ(run_one(), INV_RENDER_ONLY);
}

TEST(pacing_stock_hold_gives_the_game_its_pacing_back) {
    fresh();
    run_one();
    run_one();
    pacing_hold_stock("test", true);            // e.g. an FMV starts
    CHECK_EQ(run_one(), INV_STOCK);
    CHECK_EQ(fake::divider, 2);                 // the divider the game asked for
    CHECK_EQ(fake::exec_byte, 1);
    CHECK(!pacing_divider_request(2));          // while held, game requests reach the game
    pacing_hold_stock("test", false);
    CHECK_EQ(run_one(), INV_LOGIC);
    CHECK_EQ(fake::divider, 1);
}

TEST(pacing_holds_nest) {
    fresh();
    pacing_hold_stock("a", true);
    pacing_hold_stock("b", true);
    pacing_hold_stock("a", false);
    CHECK_EQ(run_one(), INV_STOCK);             // "b" still holds
    pacing_hold_stock("b", false);
    CHECK_EQ(run_one(), INV_LOGIC);
}

TEST(pacing_release_without_hold_does_not_go_negative) {
    fresh();
    pacing_hold_stock("x", false);
    pacing_hold_stock("y", true);
    CHECK_EQ(run_one(), INV_STOCK);             // one hold must still count as held
    pacing_hold_stock("y", false);
    CHECK_EQ(run_one(), INV_LOGIC);
}

TEST(pacing_leave_now_restores_stock_at_once) {
    fresh();
    run_one();
    run_one();
    pacing_leave_now("load");
    CHECK_EQ(fake::divider, 2);
    CHECK_EQ(fake::exec_byte, 1);
    CHECK_EQ(run_one(), INV_LOGIC);             // the next invocation re-enters 60 Hz with logic
}

TEST(pacing_flip_phase_repeats_one_kind) {
    fresh();
    CHECK_EQ(run_one(), INV_LOGIC);
    pacing_flip_phase();
    CHECK_EQ(run_one(), INV_LOGIC);             // repeated
    CHECK_EQ(run_one(), INV_RENDER_ONLY);
}

TEST(pacing_inject_backlog_adds_one_catch_up_invocation) {
    fresh();
    CHECK_EQ(run_one(), INV_LOGIC);
    pacing_inject_backlog();
    CHECK_EQ(run_one(), INV_BACKLOG);
    CHECK_EQ(fake::backlog_count, 2);
    fake::backlog_count = 0;
    CHECK_EQ(run_one(), INV_RENDER_ONLY);
}

TEST(pacing_waits_for_the_divider_hook) {
    fresh();
    g_divider_hooked = false;
    CHECK_EQ(run_one(), INV_STOCK);
    CHECK(fake::divider_calls.empty());
    g_divider_hooked = true;
}

TEST(pacing_reasserts_divider_1_if_the_game_changed_it) {
    fresh();
    run_one();
    fake::divider = 2;                          // the game wrote its divider without the setter
    run_one();
    CHECK_EQ(fake::divider, 1);
    CHECK(fake::log_contains("re-asserted 1"));
}

TEST(pacing_exec_method_1_writes_the_byte_directly) {
    fresh();
    g_cfg.exec_method = 1;
    run_one();
    run_one();
    CHECK_EQ(fake::exec_byte, 0);
    CHECK(fake::exec_calls.empty());            // the game's setter was not called
    g_cfg.exec_method = 0;
}

TEST(menudiv2_keeps_held_menus_at_30hz) {
    // X2-PACE-MENUDIV2 (Steam Deck "Fast Results"): under a stock hold the game's request for 60 Hz is swallowed,
    // and every host frame gets two invocations so the menu animation keeps its speed.
    fresh();
    pacing_menudiv2_set(true);
    pacing_hold_stock("menu", true);
    CHECK(pacing_divider_request(1));           // swallowed: stays at 2
    CHECK_EQ(run_one(), INV_STOCK);
    CHECK_EQ(fake::backlog_count, 2);           // two invocations this host frame
    pacing_hold_stock("menu", false);
    pacing_divider_request(2);
    pacing_menudiv2_set(false);
}

TEST(menudiv2_off_lets_the_menu_request_through) {
    fresh();
    pacing_hold_stock("menu", true);
    CHECK(!pacing_divider_request(1));
    pacing_hold_stock("menu", false);
}
