// SPDX-License-Identifier: Apache-2.0
// Fakes for everything the tested x2 files call outside themselves: game memory becomes plain variables, and calls
// into the log, the ini and other rows are recorded so a test can look at them.
#pragma once
#include "../../src/x2/x2.h"
#include <string>
#include <vector>

namespace fake {

// Game memory the pacing code reads and writes (x2_sigs.cpp points these at FFX-2.exe in the real mod).
extern int8_t   exec_byte;        // the game's exec flag: 1 = this invocation runs logic, 0 = draw only
extern uint32_t divider;          // the game's frame divider: 1 = every vsync (60 Hz), 2 = every second (30 Hz)
extern int32_t  backlog_count;    // invocations the game still wants to run this host frame
extern uint32_t frame_counter;

extern std::vector<uint32_t> divider_calls;   // every value the mod passed to the game's divider setter
extern std::vector<int> exec_calls;           // every value the mod passed to the game's exec setter
extern std::vector<std::string> log;          // every log_line, formatted

struct PassedDll { uint8_t* base; std::string name; };
extern std::vector<PassedDll> passed;         // mag008_pass_late calls

extern x2::magic_load_fn magic_on_load;       // what the row gave magic_mods_subscribe
extern x2::magic_unload_fn magic_on_unload;
extern std::vector<std::string> rows_off;          // row_on(id) is false for these

void set_ini(const char* key, int value);     // config_row(key, ...) returns value from now on
void set_ini_str(const char* key, const char* value);
void reset();                                 // empty every recording and every ini override

bool log_contains(const char* text);

} // namespace fake
