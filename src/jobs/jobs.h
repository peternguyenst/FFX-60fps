// SPDX-License-Identifier: Apache-2.0
// jobs: the Jobs mod for FFX HD (no Sphere Grid, levels and caps, jobs, Lua), built as jobs_ffx.dll. The shared
// loader (dbghelp.dll, src/loader) loads it when the host is FFX.exe, next to fps60_ffx.dll if that is present.
// Its own project and code path: it links nothing from the fps60 mod (fps60.h).
// Files next to FFX.exe: jobs.ini (optional settings), jobs.log, jobs/scripts/*.lua, jobs/character-info/...
// Addresses: gva = Ghidra VA (base 0x400000), rt = runtime; every address comes from jobs_sigs.cpp.
// Design and RE (outer repo): docs/jobs/DESIGN.md, re/ledger/JOBS-CHR.md (rows JOB-Cnn).
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

namespace jobs {

// ---------------------------------------------------------------- jobs_log.cpp
void log_open();                                 // jobs.log next to FFX.exe (keeps 3 old sessions)
void log_line(const char* fmt, ...);
void game_dir(wchar_t* out, size_t cap);         // with trailing '\'

// ---------------------------------------------------------------- jobs_config.cpp
struct Config {
    int  enabled;                                // [jobs] enabled
    int  unpack_timeout_s;                       // [jobs] unpack_timeout_s: wait for the Steam DRM to unpack .text
    int  script_poll_ms;                         // [jobs] script_poll_ms: how often jobs/scripts is checked for edits
};
extern Config g_cfg;
void config_load();

// ---------------------------------------------------------------- jobs_sigs.cpp
uint8_t*  exe_base();
uint32_t  rt_to_gva(const void* rt);
bool      sigs_text_ready();                     // DRM unpack probe
bool      sigs_resolve_all();                    // false = a required sig failed -> install nothing
bool      safe_copy(void* dst, const void* src, size_t n);

// Party block (JOB-C01..C13): 0x12 records of 0x94 bytes. Index 0..6 party, 7 Seymour, 8.. Aeons.
constexpr int PLY_COUNT  = 0x12;
constexpr int PLY_STRIDE = 0x94;
extern uint8_t* g_ply;                           // rt of DAT_0113205c (gva), from FUN_00785330

// Record offsets (JOBS-CHR "Party block").
namespace ply {
constexpr int BASE_HP   = 0x04;  // u32
constexpr int BASE_MP   = 0x08;  // u32
constexpr int BASE_STAT = 0x0C;  // u8 x8: str def mag mdef agi luck eva acc
constexpr int AP_TOTAL  = 0x14;  // u32
constexpr int AP        = 0x18;  // u32, toward the next level
constexpr int HP        = 0x1C;  // u32 current
constexpr int MP        = 0x20;  // u32 current
constexpr int MAX_HP    = 0x24;  // u32 final
constexpr int MAX_MP    = 0x28;  // u32 final
constexpr int FLAGS     = 0x2C;  // u8, bit 0 = joined (JOB-C29)
constexpr int STAT      = 0x2F;  // u8 x8 final
constexpr int LEVEL     = 0x3B;  // u8 (vanilla: unspent S.Lv)
constexpr int CAP       = 0x3C;  // u8 (vanilla: spent S.Lv; mod: 0x80 | tier)
}

// ---------------------------------------------------------------- jobs_lua.cpp
void lua_start();                                // load jobs/scripts/main.lua and start the reload watcher

} // namespace jobs
