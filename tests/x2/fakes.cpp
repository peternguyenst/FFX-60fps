// SPDX-License-Identifier: Apache-2.0
#include "fakes.h"
#include <map>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace fake {

int8_t   exec_byte = 1;
uint32_t divider = 2;
int32_t  backlog_count = 0;
uint32_t frame_counter = 0;

std::vector<uint32_t> divider_calls;
std::vector<int> exec_calls;
std::vector<std::string> log;
std::vector<PassedDll> passed;
x2::magic_load_fn magic_on_load = nullptr;
x2::magic_unload_fn magic_on_unload = nullptr;

static std::map<std::string, int> g_ini;
static std::map<std::string, std::string> g_ini_str;

void set_ini(const char* key, int value) { g_ini[key] = value; }
void set_ini_str(const char* key, const char* value) { g_ini_str[key] = value; }

void reset() {
    divider_calls.clear();
    exec_calls.clear();
    log.clear();
    passed.clear();
    g_ini.clear();
    g_ini_str.clear();
}

bool log_contains(const char* text) {
    for (const std::string& line : log)
        if (line.find(text) != std::string::npos) return true;
    return false;
}

// The game's setters: write the value where the game keeps it, and remember the call.
static void __cdecl game_set_exec(int v) { exec_calls.push_back(v); exec_byte = (int8_t)v; }
static void __cdecl game_set_divider(uint32_t v) { divider_calls.push_back(v); divider = v; }

} // namespace fake

namespace x2 {

// --- x2_sigs.cpp: addresses found in FFX-2.exe, here pointing at the fake game memory ---
set_exec_t         g_set_exec = fake::game_set_exec;
uint8_t*           g_get_exec = nullptr;
set_divider_t      g_set_divider = fake::game_set_divider;
uint8_t*           g_sg_mainloop = nullptr;
uint8_t*           g_mainloop_callsite = nullptr;
volatile int8_t*   g_exec_byte = &fake::exec_byte;
volatile uint32_t* g_divider = &fake::divider;
volatile int32_t*  g_backlog_count = &fake::backlog_count;
volatile uint32_t* g_frame_counter = &fake::frame_counter;

bool sig_match(const void*, const char*) { return false; }
uint8_t* abs_operand(const char*, const uint8_t*) { return nullptr; }
// Nothing of the game is there to find: a row's install() fails cleanly. Tests call its helpers directly instead.
uint8_t* sig_find(const char*, const char*, uint32_t) { return nullptr; }
uint8_t* exe_base() { return nullptr; }
uint32_t rt_to_gva(const void*) { return 0; }
uint8_t* call_target(const uint8_t*) { return nullptr; }
bool in_image(const void*) { return false; }

// --- x2_hooks.cpp: nothing is patched in a test ---
bool patch_call_rel32(uint8_t*, void*, void*) { return false; }
bool patch_u32(void*, uint32_t) { return false; }
bool hook_entry(const char*, void*, void*, void**) { return false; }
void hook_remove(void*) {}

// --- x2_perf.cpp ---
bool perf_on() { return false; }
LONGLONG perf_now() { return 0; }
void perf_walk(LONGLONG) {}

// --- x2_hooks.cpp ---
volatile LONG g_disabled = 0;
bool g_divider_hooked = true;

// --- x2_config.cpp ---
Config g_cfg = { 1, 1, 0, 60, 1 };

int config_row(const char* row_id, int def) {
    auto it = fake::g_ini.find(row_id);
    return it == fake::g_ini.end() ? def : it->second;
}

void config_str(const char*, const char* key, char* out, int size, const char* def) {
    auto it = fake::g_ini_str.find(key);
    strncpy_s(out, size, it == fake::g_ini_str.end() ? def : it->second.c_str(), _TRUNCATE);
}

bool on_wine() { return false; }

// --- x2_log.cpp ---
void log_line(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fake::log.push_back(buf);
}

// --- x2_magic_mods.cpp ---
bool magic_mods_subscribe(const char*, magic_load_fn on_load, magic_unload_fn on_unload) {
    fake::magic_on_load = on_load;
    fake::magic_on_unload = on_unload;
    return true;
}

// --- X2-MAG-008 ---
bool mag008_pass_late(uint8_t* base, const char* name) {
    fake::passed.push_back({ base, name });
    return true;
}

} // namespace x2
