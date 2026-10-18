// SPDX-License-Identifier: Apache-2.0
// x2: the 60fps mod for FFX-2 HD, built as fps60_ffx2.dll. The shared loader (dbghelp.dll, src/loader) loads it
// when the host is FFX-2.exe. Its own project and code path: it links nothing from the FFX mod (fps60.h).
// Files: fps60x2.ini (optional settings) / fps60x2.log next to FFX-2.exe.
// Addresses: gva = Ghidra VA (base 0x400000), rt = runtime; every address comes from x2_sigs.cpp.
// Details (outer repo G:\Work\ffx2-fps60): docs/PLAN.md, re/MAINLOOP.md
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

namespace x2 {


// ---------------------------------------------------------------- x2_log.cpp
void log_open();                                 // fps60x2.log next to FFX-2.exe (keeps 3 old sessions)
void log_line(const char* fmt, ...);
void game_dir(wchar_t* out, size_t cap);         // with trailing '\'

// ---------------------------------------------------------------- x2_config.cpp
struct Config {
    int  enabled;                                // [x2] enabled
    int  pacing;                                 // 1 = 60 Hz (divider 1 + exec alternation), 0 = stock
    int  exec_method;                            // 0 = game setter, 1 = raw byte write
    int  unpack_timeout_s;
    int  log_pacing_events;
};
extern Config g_cfg;
void config_load();
int  config_row(const char* row_id, int def);    // [rows] X2-ROW-ID=0|1
void config_str(const char* sect, const char* key, char* out, int size, const char* def);   // ASCII string key
bool config_write(const char* sect, const char* key, const char* value);   // fps60x2.ini (X2-CFG-MENU)

// ---------------------------------------------------------------- x2_sigs.cpp
uint8_t*    exe_base();
uint32_t    rt_to_gva(const void* rt);
bool        sigs_text_ready();                   // DRM unpack probe
bool        sigs_resolve_all();                  // false = a required sig failed -> install nothing
bool        safe_read_u32(const void* p, uint32_t* out);
bool        safe_copy(void* dst, const void* src, size_t n);
// Row helpers: a pattern must match exactly once in .text (logged; expected_gva is logged only).
uint8_t*    sig_find(const char* label, const char* pattern, uint32_t expected_gva);
bool        sig_match(const void* at, const char* pattern);   // bytes at a resolved address (fault-safe)
uint8_t*    abs_operand(const char* name, const uint8_t* at); // absolute address operand, must be inside FFX-2.exe
uint8_t*    call_target(const uint8_t* site);    // target of E8 rel32, or null
bool        in_image(const void* p);

typedef void(__cdecl* set_exec_t)(int);
typedef void(__cdecl* set_divider_t)(uint32_t);
extern set_exec_t         g_set_exec;
extern uint8_t*           g_get_exec;           // Sg_GetCurExecFrames (effective getter)
extern set_divider_t      g_set_divider;         // trampoline once hooked, else the original
extern uint8_t*           g_sg_mainloop;
extern uint8_t*           g_mainloop_callsite;   // E8 of CALL Sg_MainLoop in the runner
extern volatile int8_t*   g_exec_byte;
extern volatile uint32_t* g_divider;
extern volatile int32_t*  g_backlog_count;
extern volatile uint32_t* g_frame_counter;

// ---------------------------------------------------------------- x2_hooks.cpp
extern volatile LONG g_disabled;                 // any fault -> 1, hooks pass through
extern bool g_divider_hooked;
bool hooks_install_mainloop();                   // init thread
bool patch_call_rel32(uint8_t* site, void* old_target, void* new_target);   // verified CALL retarget
bool patch_u32(void* at, uint32_t v);            // 4-byte write through VirtualProtect (atomic if aligned)
bool hook_entry(const char* name, void* target, void* detour, void** orig); // MinHook entry hook
void hook_remove(void* target);                  // undo hook_entry (rows that install several hooks as one)
void hooks_install_divider_on_game_thread();     // first pre()

// ---------------------------------------------------------------- x2_pacing.cpp
enum InvKind { INV_LOGIC = 0, INV_BUILD = 1, INV_BACKLOG = 2, INV_STOCK = 3 };
void pacing_pre();
void pacing_post();
bool pacing_divider_request(uint32_t req);       // true = swallow
int  pacing_kind();
inline bool render_only() { return pacing_kind() == INV_BUILD; }
void pacing_hold_stock(const char* who, bool hold); // a row asks for stock pacing (e.g. FMV)
void pacing_set(bool on);                         // runtime 60 Hz on/off ([x2] pacing; X2-CFG-MENU)

// ---------------------------------------------------------------- x2_magic_mods.cpp
typedef void (*magic_load_fn)(uint8_t* base, uint32_t size, const char* name);   // loader lock
typedef void (*magic_unload_fn)(uint8_t* base);
bool magic_mods_subscribe(const char* row, magic_load_fn on_load, magic_unload_fn on_unload);  // init thread

// ---------------------------------------------------------------- x2_rows.cpp
// A row = one ledger row's mitigation (ID "X2-..." = [rows] key). install() runs once on the init thread after
// the signature scan; pre()/post() run around every invocation while the row is on.
struct Row {
    const char* id;
    const char* summary;
    int         default_on;
    bool (*install)();           // false = row stays off (logged)
    void (*pre)();               // optional
    void (*post)();              // optional
    int         on;
    int         installed;       // install() ran and succeeded at startup
};
void rows_install_all();
bool row_on(const char* id);
bool perf_on();                                 // X2-DIAG-PERF (x2_perf.cpp)
LONGLONG perf_now();
void perf_row(int idx, LONGLONG dt);
void perf_walk(LONGLONG dt);
void perf_inv_begin();
void perf_inv_end();
bool row_set(Row& r, bool on);                  // runtime on/off of an installed row (false: not installed)
bool menu002_user_on();                         // X2-MENU-002 user switch (Num 9, X2-CFG-MENU)
void menu002_set_user(bool on);
const uint8_t* x2_text(int id);                 // T_* id (x2_text_strings.h) -> game-encoded text, current language
void rows_pre();
void rows_post();

// Cross-row hooks
uint8_t* mag006_entry();                         // Ppp_ProgramStep entry (null if X2-MAG-006 is off)
bool     mag006_pause_exe_caller(uint8_t* ret);  // X2-MAG-006 pauses this EXE call site like a DLL caller
typedef void (*step_note_fn)(void* prog, int cls, int parts_before, int parts_after, int ret);
void*    mag006_cur_step(int* cls);               // program of the running particle step (cls 1 DLL, 2 EXE, 3 field)
void     mag006_set_step_note(step_note_fn fn);   // diagnostics: called after each Ppp_ProgramStep
bool     pppops_prog_marked(void* prog);          // X2-MAG-006-UNPAUSE-OPS test (X2-DIAG-PPPOPS)
void     mag026_draw_record(uint8_t* head, uint8_t* nd, void* cb);   // X2-MAG-020 draw-only walk -> X2-MAG-026
bool     mag008_list_active();                 // X2-MAG-008: a PASS-LIST DLL (or its chain) is loaded

// Rollback: a span is snapshotted at the start of every render-only invocation and written back at its end.
typedef bool (*span_resolver_t)(void* ctx, uint8_t** base, uint32_t* len);
bool rollback_add_static(const char* row_id, uint8_t* rt, uint32_t len);
bool rollback_add_dynamic(const char* row_id, span_resolver_t fn, void* ctx, uint32_t max_len);
void rollback_pre();
void rollback_post();

} // namespace x2
