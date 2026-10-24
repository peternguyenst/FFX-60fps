// SPDX-License-Identifier: Apache-2.0
// fps60: the 60fps mod for FFX HD (fps60_ffx.dll, loaded by the shared loader src/loader). pacing.cpp makes every other Sg_MainLoop invocation
// render-only; rollback.cpp and rows.cpp keep game state stepping once per logic tick.
// Addresses: gva = Ghidra VA, rt = runtime; every address comes from sigs.cpp, none is hardcoded.
// Details: docs/fps60/architecture.md
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------- log.cpp
void log_open();                                 // fps60.log next to FFX.exe
void log_line(const char* fmt, ...);             // timestamped, flushed per line
void game_dir(wchar_t* out, size_t cap);         // directory of FFX.exe, with trailing '\'
bool hot_copy();                                 // this copy was loaded by a dev hot reload (fps60_live_*.dll)

// ---------------------------------------------------------------- config.cpp
enum AltBacklog { ALT_BACKLOG_LOGIC = 0, ALT_BACKLOG_TOGGLE = 1 };
enum ExecMethod { EXEC_SETTER = 0, EXEC_BYTE = 1 };

struct Config {
    int enabled;                 // 0 = nothing installed (the loader still forwards dbghelp)
    int pacing;                  // 1 = mod pacing (divider 1 + exec alternation), 0 = stock pacing
    int fps;                     // 60 / 120 / 240: drawn frames per second ([fps60] fps), see rate.cpp
    int alt_backlog;             // skip-render backlog invocations: logic = exec 1, parity unchanged
    int exec_method;
    int rollback;                // master switch for the rollback engine
    int unpack_timeout_s;
    int log_pacing_events;       // log divider requests / re-asserts (rate-limited)
    int crash_dump_mem;          // 1 = the game's crash dump also holds heap + module data (MiniDumpWithPrivateReadWriteMemory)
};
extern Config g_cfg;
void config_load();
int  config_row(const char* row_id, int def);   // [rows] ROW_ID=0|1
void config_str(const char* sect, const char* key, char* buf, int n, const char* def);
bool config_write(const char* sect, const char* key, const char* value);   // game-folder fps60.ini
// text.cpp: the mod's text in the game's current text language (text/strings.json -> text_strings.h).
#include "text_strings.h"
bool ffx_text_init();                            // resolves the language getter once (install time)
int  ffx_text_lang();                            // current text language (1 = English if unknown)
const uint8_t* ffx_text(int id);                 // T_* id -> game-encoded text for the current language


// ---------------------------------------------------------------- sigs.cpp (ported from FRAMEDIFF)
struct Sym {
    const char* name;
    uint8_t*    rt;              // resolved runtime address (nullptr = unresolved)
    uint32_t    expected_gva;    // logged only
    bool        required;
};
bool        sigs_text_ready();                   // cheap readiness probe (DRM unpack)
bool        sigs_resolve_all();                  // false = a REQUIRED sig failed -> install nothing
uint8_t*    sym_rt(const char* name);            // nullptr if unknown / unresolved
int         sym_count();
const Sym*  sym_at(int i);
uint32_t    rt_to_gva_exe(const void* rt);
uint8_t*    exe_base();
uint32_t    exe_size();
uint8_t*    sig_find_unique(const char* label, const char* pattern, uint32_t expected_gva); // rows
uint8_t*    call_target(const uint8_t* site);    // target of E8 rel32, or null
uint8_t*    jmp_target(const uint8_t* site);     // target of E9 rel32, or null
uint8_t*    read_abs(const uint8_t* at);         // absolute address operand at `at` (relocated)
bool        sig_match(const void* at, const char* pattern);   // bytes at a resolved address (fault-safe)
// Scan a loaded module's executable sections; *hits = match count (capped at 9); `accept` filters.
uint8_t*    sig_scan_module(uint8_t* base, uint32_t size, const char* pattern, int* hits,
                            bool (*accept)(const uint8_t*) = nullptr);

typedef void(__cdecl* set_exec_t)(int);
typedef int (__cdecl* get_exec_t)(void);
typedef void(__cdecl* set_divider_t)(uint32_t);
extern set_exec_t    g_set_exec;
extern get_exec_t    g_get_exec;
extern set_divider_t g_set_divider_orig;         // trampoline once hooked, else the original
extern void*         g_sg_mainloop;
extern uint8_t*      g_mainloop_callsite;        // the E8 of CALL Sg_MainLoop in the runner
extern volatile int8_t*   g_exec_byte;
extern volatile uint32_t* g_divider;
extern volatile int32_t*  g_backlog_count;
extern volatile int32_t*  g_skip_render;
extern volatile uint32_t* g_frame_counter;
extern volatile uint32_t* g_vblank;

// ---------------------------------------------------------------- hooks.cpp
bool hooks_install_mainloop();                   // init thread
void hooks_install_divider_on_game_thread();     // called from the first pre()
extern volatile LONG g_disabled;                 // any fault -> 1, hooks become pass-through
extern bool g_divider_hooked;
bool patch_call_rel32(uint8_t* site, void* old_target, void* new_target);

// ---------------------------------------------------------------- rate.cpp
// Timebase multiplier. Game logic stays at 29.97 Hz; the mod draws N passes per logic tick,
// N = fps / 30, for any multiple of 30 from 60 to 240. N = 2 is the original 60 fps behaviour.
int      rate_fps();                             // configured frame rate (60 / 120 / 240)
int      rate_mult_cfg();                        // configured N
int      rate_mult();                            // N in force: 2 until RATE-001 holds the limiter at N * 29.97
bool     rate_set_fps(int fps);                  // the player asked for this rate (CFG-MENU); false = refused
bool     rate_step_fps(int fps);                 // PERF-002 stepped the rate; what the player asked for stands
int      rate_user_fps();                        // the rate the player asked for, whatever PERF-002 stepped to
bool     rate_fps_ok(int fps);                   // a supported rate: a multiple of 30 from 60 to 240
uint32_t rate_gen();                             // bumped whenever rate_mult() changes (rows re-seed clocks)
void     rate_limiter_engaged(bool on);          // RATE-001: the host limiter now runs at the mod's rate
void     rate_init();                            // after config_load: clamp [fps60] fps, log the timebase
void     rate001_limiter(bool want);             // rows/RATE-001.cpp: hold the host limiter at the mod's rate
bool     rate001_limiter_set(int mult);          // rows/RATE-001.cpp: the configured N changed

// ---------------------------------------------------------------- pacing.cpp
// Per Sg_MainLoop invocation, in this order: pacing_pre, rollback_pre, rows_pre,
// Sg_MainLoop, rows_post, rollback_post, pacing_post (capture points interleaved, see below).
enum InvKind { INV_LOGIC = 0, INV_BUILD = 1, INV_BACKLOG = 2, INV_STOCK = 3 };
void pacing_pre();
void pacing_post();
bool pacing_divider_request(uint32_t req);       // from the divider hook; true = swallow
bool pacing_active();                            // 60 Hz pacing currently applied
int  pacing_kind();                              // InvKind of the current invocation
void pacing_hold_stock(const char* who, bool hold); // a row asks for stock pacing (e.g. FMV)
void pacing_set(bool on);                         // runtime 60 Hz on/off ([fps60] pacing; CFG-MENU)
void pacing_force_div(int every);                // logic tick every `every` invocations, kind INV_BACKLOG; 0 = off
int  pacing_pass();                              // 0..N-1 inside the current logic period (0 = the logic tick)
float pacing_t();                                // (pass + 1) / N: where in the tick this pass draws (1 = tick k)
bool sr008_battle_reset_pending();               // rows/SR-003.cpp: SR-008 soft reset in battle under way

// ---------------------------------------------------------------- rollback.cpp
// A span is snapshotted at the start of every INV_BUILD invocation and written back at its end,
// so render-only invocations cannot advance it. Resolvers run at every snapshot (heap pointers).
typedef bool (*span_resolver_t)(void* ctx, uint8_t** base, uint32_t* len);
bool rollback_add_static(const char* row_id, uint8_t* rt, uint32_t len);
bool rollback_add_dynamic(const char* row_id, span_resolver_t fn, void* ctx, uint32_t max_len);
void rollback_pre();
void rollback_post();
void rollback_report();                          // install summary
int  rollback_span_count();

// ---------------------------------------------------------------- rows.cpp
// A row = one ledger row's mitigation. install() runs once on the init thread after the
// signature scan; pre()/post() run around every invocation while the row is enabled.
struct Row {
    const char* id;              // ledger row ID, also the [rows] ini key
    const char* summary;
    int         default_on;
    bool (*install)();           // returns false = row stays off (logged)
    void (*pre)();               // optional
    void (*post)();              // optional
    int         on;              // resolved: ini toggle && install ok
    void (*stop)();              // optional: undo state held between invocations (dev hot reload, reload.cpp)
    int         installed;       // install() ran and succeeded (at startup, or later through row_set)
};
extern Row row_MAG008;                           // rows/MAG-008.cpp
extern Row row_MAG006;                           // rows/MAG-006.cpp
extern Row row_CLK013;                           // rows/CLK-013.cpp (stock pacing in SyncData cutscenes)
extern Row row_W2A002;                           // rows/W2A-002.cpp
extern Row row_CLK002;                           // rows/CLK-002.cpp
extern Row row_MAG009;                           // rows/MAG-009.cpp
extern Row row_LOOP004;                          // rows/LOOP-004.cpp
extern Row row_LOOP005;                          // rows/LOOP-005.cpp
extern Row row_MAG018;                           // rows/MAG-018.cpp
extern Row row_MAG017;                           // rows/MAG-017.cpp
extern Row row_MAG016;                           // rows/MAG-016.cpp
extern Row row_MAG019;                           // rows/MAG-019.cpp
extern Row row_MAG020;                           // rows/MAG-020.cpp
extern Row row_MAG021;                           // rows/MAG-021.cpp
extern Row row_MAG022;                           // rows/MAG-022.cpp
extern Row row_MAG023;                           // rows/MAG-023.cpp (motion/throw script, 118 DLLs)
extern Row row_MAG024;                           // rows/MAG-024.cpp (thrown-item arc node, 5 DLLs)
extern Row row_MAG025;                           // rows/MAG-025.cpp (DLL-called effect-unit walks, draw only on render-only)
extern Row row_MAG026;                          // rows/MAG-026.cpp (family-X record draws held on render-only)
extern Row row_MAG027;                           // rows/MAG-027.cpp (event timeline walker, 22 DLLs: starts on logic only)
extern Row row_MAG028;                           // rows/MAG-028.cpp (particle pause flag back to the game's value)
extern Row row_MAG029;                           // rows/MAG-029.cpp (effect-unit particle programs paused on render-only)
extern Row row_MAG030;                           // rows/MAG-030.cpp (OPP effect units restored after render-only draws)
extern Row row_FX011;                            // rows/FX-011.cpp (draw packet pools full: drop the sprite, no crash)
extern Row row_DIAGFRAMETIME;                    // rows/DIAG-FRAMETIME.cpp (frame time histogram, VRR check)
extern Row row_RATE001;                          // rows/RATE-001.cpp (host frame limiter retargeted to N * 29.97 Hz)
void     mag026_draw_record(uint8_t* head, uint8_t* nd, void* cb); // MAG-020 draw-only walk -> MAG-026
extern Row row_UI001;                            // rows/UI-001.cpp
extern Row row_UI017;                           // rows/UI-017.cpp (pad latch per logic tick, K4)
extern Row row_VSYNC;                            // rows/VSYNC.cpp (option: Present with SyncInterval 1)
extern Row row_UI013;                            // rows/UI-013.cpp (distortion grid wave step 1/N per invocation)
extern Row row_UISPH001;                        // rows/UI-SPH-001.cpp (Sphere Grid update per logic tick)
extern Row row_UI020;                           // rows/UI-020.cpp (save/load list held repeat per logic tick)
extern Row row_DEVSTOCK;                        // rows/DEV-STOCK.cpp (test tool: Ctrl+F5 toggles stock pacing)
extern Row row_DEVTURBO;                        // rows/DEV-TURBO.cpp (test tool: hold a key to fast-forward)
extern Row row_DEVDMG;                           // rows/DEV-DMG.cpp (test tool: fixed party hit damage)
extern Row row_QOLSAVEICON;                     // rows/QOL-SAVEICON.cpp (option: no autosave chocobo)
extern Row row_UIBTLL1;                         // rows/UI-BTLL1.cpp (battle L1 party swap presses kept)
extern Row row_UI014;                           // rows/UI-014.cpp (minimap arrow pulse per logic tick)
extern Row row_PERF001;                         // rows/PERF-001.cpp (1 ms timer, no power throttling)
extern Row row_PERF002;                         // rows/PERF-002.cpp (stock 30 fps when 60 is not held)
extern Row row_QOLSENSOR;                       // rows/QOL-SENSOR.cpp (gameplay: party always has Sensor)
extern Row row_BOOST001;                        // rows/BOOST-001.cpp (game speed booster 2x/4x at 60 Hz)
extern Row row_QOLAP;
extern Row row_QOLRETRY;                        // rows/QOL-RETRY.cpp (gameplay: Retry / Title after a wipe)                           // rows/QOL-AP.cpp (gameplay: whole party earns AP)
extern Row row_CFGMENU;                         // rows/CFG-MENU.cpp (mod options in the game's Config page)
extern Row row_TEXANIM001;                      // rows/TEXANIM-001.cpp (chr texture animation per logic tick)
extern Row row_DIAG_DYNGEO;                    // rows/DIAG-DYNGEO.cpp (dynamic-geometry upload diag)
extern Row row_SR003;                          // rows/SR-003.cpp (soft reset chord -> title)
extern Row row_LAB001;                          // rows/LAB-001.cpp (combat lab diagnostic, default off)
extern Row row_UISPH002;                        // rows/UI-SPH-002.cpp (Sphere Grid particles paused on render-only)
uint8_t* mag006_entry();                        // Ppp_ProgramStep entry (null if MAG-006 is off)
bool     mag006_pause_exe_caller(uint8_t* ret); // MAG-006 pauses this EXE call site like a DLL caller
extern Row row_FX010;                            // rows/FX-010.cpp
extern Row row_DIAG_BTLSIG;                      // rows/DIAG-BTLSIG.cpp (diagnostic, default off)
extern Row row_DIAG_PAUSEWATCH;                  // rows/DIAG-PAUSEWATCH.cpp (diagnostic, default off)
extern Row row_DIAG_HANGDUMP;                    // rows/DIAG-HANGDUMP.cpp (diagnostic, observe only)
extern Row row_DIAG_TEXTDUMP;                    // rows/DIAG-TEXTDUMP.cpp (diagnostic, default off)
extern Row row_DIAG_DRAWSTEP;                    // rows/DIAG-DRAWSTEP.cpp (diagnostic, default off)
extern Row row_SMOOTH001;                        // rows/SMOOTH-001.cpp (60 Hz camera; master [rows] SMOOTH=0)
extern Row row_SMOOTH002;                        // rows/SMOOTH-002.cpp (60 Hz actor roots; master [rows] SMOOTH=0)
extern Row row_SMOOTH003;                        // rows/SMOOTH-003.cpp (60 Hz limbs; master [rows] SMOOTH=1, row default 1)
// Shared between rows
extern void (*g_mag008_build_hook)();            // MAG-008: called where the exec==0 dispatch is skipped
void* mag008_dispatch();                         // MAG-008: MsBattleScene_DrawDispatch (null if not installed)
const char* mag021_state();                     // MAG-021: window state, for the crash line
bool  mag021_in_window(const void* base);        // MAG-021: module replayed in the current window
int   famE_find_setter(uint8_t* mod, uint32_t size, uint8_t** hit); // MAG-018: family-E exec-pause setter count
int   qol_ap_rate();                             // QOL-AP: AP percent for members who did not fight
void  qol_ap_set_rate(int pct);                  // QOL-AP: runtime change (CFG-MENU), clamped 0..100
void rows_install_all();
bool row_on(const char* id);                     // false = that row is turned off (rollback skips its spans)
// Runtime switch, game thread only (CFG-MENU). On: a row that is off in the ini is installed now (its pre() applies
// its patches on the next invocation). Off: Row::stop undoes what the row holds, then pre/post stop running.
// Returns the row's resulting on state.
bool row_set(Row& r, bool on);
void rows_pre();
void rows_stop_all();                           // dev hot reload: Row::stop of every row that is on
void rows_post();
int        rows_count();
const Row* row_at(int i);

// ---------------------------------------------------------------- capture.cpp (TRIAGE E0)
// FRAMEDIFF's capture inside fps60, observe only ([capture] in fps60.ini, default off). hooks.cpp
// calls capture_point() at five points per invocation (see the order below and capture.cpp):
//   fp_pre:  PRE_OUTER, pacing_pre, PRE_PACED, rollback_pre, rows_pre, PRE_INNER
//   fp_post: POST_INNER, rows_post, rollback_post, pacing_post, POST_OUTER
// boundary=outer diffs PRE_OUTER..POST_OUTER, boundary=inner diffs PRE_INNER..POST_INNER.
enum CapPoint { CAP_PRE_OUTER = 0, CAP_PRE_PACED = 1, CAP_PRE_INNER = 2, CAP_POST_INNER = 3, CAP_POST_OUTER = 4 };
void capture_init();                             // init thread; enabled=0 -> nothing allocated or registered
void capture_point(int point);                   // no-op unless enabled
void capture_divider_event(uint32_t req, bool swallowed); // divider hook, after pacing decided
void capture_on_fault(const char* where, DWORD code);     // disables the capture only
void capture_abort(const char* why);             // fps60 itself faulted

// ---------------------------------------------------------------- diag.cpp (re/ledger/HANG-SPELLLOAD.md)
// DIAG-HANGDUMP breadcrumbs. Game thread: one where-word, plain stores, read by the watchdog:
//   bits 0..7 phase (W_*), bits 8..15 row index (rows.cpp table, 0xFF = none), bits 16..23 sub-step of a row.
enum WherePhase { W_BETWEEN = 0, W_PRE = 1, W_PACING_PRE = 2, W_ROLLBACK_PRE = 3, W_ROW_PRE = 4, W_MAINLOOP = 5,
                  W_ROW_POST = 6, W_ROLLBACK_POST = 7, W_PACING_POST = 8 };
extern volatile LONG g_where;
inline void where_set(int phase, int row) { g_where = (LONG)(phase | (row << 8)); }
inline void where_sub(int sub) { g_where = (LONG)((g_where & 0xFFFF) | (sub << 16)); }  // inside Sg_MainLoop
void where_text(LONG w, char* out, size_t cap);
// Loader callbacks (any thread, under the loader lock): which row's callback, which step, which module.
void ldr_enter(const char* row, ULONG reason, const void* base);
void ldr_step(int step);
void ldr_leave();
void ldr_text(char* out, size_t cap);
DWORD log_cs_owner();                            // log.cpp: thread id holding the log CS, 0 if free
bool  log_line_try(const char* fmt, ...);        // log.cpp: like log_line, but gives up if the log CS is held
void* proxy_real_minidump();                     // proxy.cpp: the System32 MiniDumpWriteDump (resolved once)

// ---------------------------------------------------------------- safe memory helpers (sigs.cpp)
bool  safe_read_u32(const void* p, uint32_t* out);
bool  safe_copy(void* dst, const void* src, size_t n);
// ---------------------------------------------------------------- entry hooks (entry_hook.cpp, MinHook)
bool  hook_init();                                // once, before the first hook_entry (init thread)
bool  hook_entry(const char* name, void* target, void* detour, void** orig);  // create + enable; logs failures
bool  hook_remove(const char* name, void* target);  // disable + free the trampoline (module unload)
void  hook_disable_all();                         // MH_DisableHook(MH_ALL_HOOKS): originals back (dev hot reload)
// Byte patches of game code or tables: every write goes through patch_write, which keeps the original bytes so
// patch_revert_all can put them back (dev hot reload). n <= 16; an aligned 4-byte write is atomic.
bool  patch_write(void* at, const void* bytes, size_t n);
void  patch_revert_all();
void  pacing_shutdown();                          // pacing.cpp: hand the game its own divider/exec back
void  log_close();                                // log.cpp
void  log_ring(const char* fmt, ...);             // log.cpp: keep a diagnostic line in memory only
void  log_ring_dump(const char* why);             // log.cpp: write the kept lines to the log
HMODULE reload_next_module();                    // reload.cpp: the copy a dev hot reload loaded (nullptr if none)
#ifdef FPS60_DEV
// Dev build only (FPS60_DEV): edge of a key press while the game window is focused. `prev` keeps the key state.
bool  dev_key(int vk, bool* prev);
void  reload_poll();                              // reload.cpp: Num 9 = load fps60_next.dll in place of this copy
#endif
// ---------------------------------------------------------------- magic overlay loads (magic_mods.cpp)
// Callbacks run under the loader lock, for magic_*.dll only. At unload the image is still mapped.
typedef void (*magic_load_fn)(uint8_t* base, uint32_t size, const char* name);
typedef void (*magic_unload_fn)(uint8_t* base);
bool  magic_mods_subscribe(const char* row, magic_load_fn on_load, magic_unload_fn on_unload);  // init thread
void* exec_alloc(size_t n);
void  on_fault(const char* where, DWORD code);   // disables everything, logs once
