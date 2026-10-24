// SPDX-License-Identifier: Apache-2.0
// Row registry for FFX-2 (same model as the FFX mod's rows.cpp / rollback.cpp, own state).
// A row = one ledger row's mitigation, toggled by its ID in [rows] of fps60x2.ini; install() runs once on the init
// thread after the signature scan, pre()/post() around every Sg_MainLoop invocation while the row is on.
// The rollback engine (spans copied out before each render-only invocation and back after it): x2_rollback.cpp.
#include "x2.h"
#include <string.h>

namespace x2 {

// ---------------------------------------------------------------- registry
extern Row row_MAG008, row_MAG009, row_LOOP005, row_LOOP004, row_UI001, row_UI017, row_W2A002, row_CLK002, row_MAG006, row_FX010DRAWOPS, row_FX012, row_UI014, row_TEXANIM001, row_DIAGRATE, row_MENU001, row_MENU002, row_TITLE001, row_DIAGDRAWSTEP, row_SMOOTH001, row_SMOOTH002, row_SMOOTH003, row_MAG016, row_MAG030, row_MAG020, row_MAG026, row_MAG025, row_MAG019, row_MAG027, row_MAGSTOCK, row_MAG031, row_MAG032, row_MAG033, row_MAG035, row_CHR001, row_CHR002, row_TASK001, row_HD001, row_SR003, row_DIAGPRESENT, row_VSYNC, row_DIAGPPPOPS, row_DIAGFTBLINK, row_DIAGOP114, row_DIAGFLASH, row_DIAGPSET, row_DIAGHIT, row_DIAGCRASH, row_CFGMENU, row_DEVTURBO, row_PACETURBO, row_LOAD001, row_DEVBISECT, row_DEVIMMORTAL, row_DEVTOPMOST, row_DIAGBLINK, row_DIAGAUTOBLINK, row_DEVEFFTEST, row_DIAGLUMA, row_DIAGCAPTURE, row_DIAGPPPSLOT;

static Row* g_rows[] = {
    &row_MAG008,                                 // rows/X2-MAG-008.cpp
    &row_MAG009,                                 // rows/X2-MAG-009.cpp
    &row_LOOP005,                                // rows/X2-LOOP-005.cpp
    &row_LOOP004,                                // rows/X2-LOOP-004.cpp
    &row_UI001,                                  // rows/X2-UI-001.cpp
    &row_UI017,                                  // rows/X2-UI-017.cpp
    &row_W2A002,                                 // rows/X2-W2A-002.cpp
    &row_CLK002,                                 // rows/X2-CLK-002.cpp
    &row_MAG006,                                 // rows/X2-MAG-006.cpp
    &row_FX010DRAWOPS,                           // rows/X2-FX-010-DRAWOPS.cpp
    &row_FX012,                                  // rows/X2-FX-012.cpp (pppop_flash on paused steps)
    &row_UI014,                                  // rows/X2-UI-014.cpp
    &row_TEXANIM001,                             // rows/X2-TEXANIM-001.cpp
    &row_MAG016,                                 // rows/X2-MAG-016.cpp (record driver, 79 magic DLLs)
    &row_MAG030,                                 // rows/X2-MAG-030.cpp (timeline driver, 92 magic DLLs)
    &row_MAG020,                                 // rows/X2-MAG-020.cpp (family-X node walk)
    &row_MAG026,                                 // rows/X2-MAG-026.cpp (family-X records; after MAG-006, MAG-020)
    &row_MAG025,                                 // rows/X2-MAG-025.cpp (OPU effect-unit walker)
    &row_MAG019,                                 // rows/X2-MAG-019.cpp (VFX shader lookup guard)
    &row_MAG027,                                 // rows/X2-MAG-027.cpp (cross-fade timeline boundary events)
    &row_MAGSTOCK,                               // rows/X2-MAG-STOCK.cpp (stock pacing while listed DLLs load)
    &row_MAG031,                                 // rows/X2-MAG-031.cpp (pause flag left by an unloaded DLL)
    &row_MAG033,                                 // rows/X2-MAG-033.cpp (magic_0146 state-1 null read)
    &row_MAG035,                                 // rows/X2-MAG-035.cpp (auto pass for blinking walker-template DLLs)
    &row_MAG032,                                 // rows/X2-MAG-032.cpp (frame-buffer readback parity)
    &row_CHR001,                                 // rows/X2-CHR-001.cpp (re-list loop-3 chr models on render-only)
    &row_CHR002,                                 // rows/X2-CHR-002.cpp (battle idle held on its last frame loops)
    &row_TASK001,                                // rows/X2-TASK-001.cpp (actor render tasks replayed)
    &row_HD001,                                  // rows/X2-HD-001.cpp (HD pass order on render-only frames, Ultima glow blink)
    &row_SR003,                                  // rows/X2-SR-003.cpp (soft reset, field + battle)
    &row_MENU002,                                // rows/X2-MENU-002.cpp (field main menu at 60 Hz)
    &row_MENU001,                                // rows/X2-MENU-001.cpp
    &row_TITLE001,                               // rows/X2-TITLE-001.cpp
    &row_SMOOTH001,                              // rows/X2-SMOOTH-001.cpp (60 Hz promotion)
    &row_SMOOTH002,                              // rows/X2-SMOOTH-002.cpp (60 Hz promotion)
    &row_SMOOTH003,                              // rows/X2-SMOOTH-003.cpp (60 Hz promotion, limbs)
    &row_DIAGRATE,                               // rows/X2-DIAG-RATE.cpp (diagnostic, default 1 while in progress)
    &row_DIAGDRAWSTEP,                           // rows/X2-DIAG-DRAWSTEP.cpp (diagnostic, default 1 while in progress)
    &row_DIAGPRESENT,                            // rows/X2-DIAG-PRESENT.cpp (diagnostic, default 1 while in progress)
    &row_VSYNC,                                  // rows/X2-DIAG-PRESENT.cpp (V-Sync, default 0; shares the Present hook)
    &row_DIAGPPPOPS,                             // rows/X2-DIAG-PPPOPS.cpp (diagnostic, default 1 while in progress)
    &row_DIAGFTBLINK,                            // rows/X2-DIAG-FTBLINK.cpp (diagnostic, default 1 while in progress)
    &row_DIAGOP114,                              // rows/X2-DIAG-OP114.cpp (diagnostic, default 1 while in progress)
    &row_DIAGFLASH,                              // rows/X2-DIAG-FLASH.cpp (diagnostic, default 1 while in progress)
    &row_DIAGLUMA,                               // rows/X2-DIAG-LUMA.cpp (diagnostic, default 0)
    &row_DIAGPSET,                               // rows/X2-DIAG-PSET.cpp (diagnostic, default 1 while in progress)
    &row_DIAGHIT,                                // rows/X2-DIAG-HIT.cpp (diagnostic, default 1 while in progress)
    &row_DIAGCRASH,                              // rows/X2-DIAG-CRASH.cpp (diagnostic, default 1 while in progress)
    &row_DIAGBLINK,                              // rows/X2-DIAG-BLINK.cpp (diagnostic, default 1 while in progress)
    &row_DIAGAUTOBLINK,                          // rows/X2-DIAG-AUTOBLINK.cpp (diagnostic, default 1 while in progress)
    &row_DIAGCAPTURE,                            // rows/X2-DIAG-CAPTURE.cpp (diagnostic, default 0; after X2-DIAG-BLINK, uses its draw hooks)
    &row_DIAGPPPSLOT,                            // rows/X2-DIAG-PPPSLOT.cpp (diagnostic, default 0; CRASH-0946)
    &row_CFGMENU,                                // rows/X2-CFG-MENU.cpp (mod options on the Config page)
    &row_DEVTURBO,                               // rows/X2-DEV-TURBO.cpp (test tool, hold numpad 0)
    &row_DEVIMMORTAL,                            // rows/X2-DEV-IMMORTAL.cpp (test tool, numpad 7 toggles)
    &row_DEVTOPMOST,                             // rows/X2-DEV-TOPMOST.cpp (test tool, numpad 6 toggles)
    &row_DEVEFFTEST,                             // rows/X2-DEV-EFFTEST.cpp (test tool, numpad 5 toggles)
    &row_PACETURBO,                              // rows/X2-PACE-TURBO.cpp (game booster x2 / x4 at 60 Hz)
    &row_LOAD001,                                // rows/X2-LOAD-001.cpp
    &row_DEVBISECT,                              // rows/X2-DEV-BISECT.cpp (test tool, F8; must stay last)
    nullptr
};

void rows_install_all() {
    int n = 0, on = 0;
    for (Row** p = g_rows; *p; p++) {
        Row& r = **p;
        n++;
        r.on = 0;
        if (!config_row(r.id, r.default_on)) {
            log_line("row %-12s OFF (ini) - %s", r.id, r.summary);
            continue;
        }
        bool ok = false;
        __try { ok = r.install ? r.install() : true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        r.on = ok ? 1 : 0;
        r.installed = r.on;
        if (ok) on++;
        log_line("row %-12s %s - %s", r.id, ok ? "ON" : "FAILED to install, stays off", r.summary);
    }
    log_line("rows: %d registered, %d on", n, on);
}

bool row_on(const char* id) {
    for (Row** p = g_rows; *p; p++) if (strcmp((*p)->id, id) == 0) return (*p)->on != 0;
    return true;                                 // not a registered row: never gated
}

// Rows are not installed at runtime (their installs patch game code, possibly code running at that moment): a row
// that was off at startup stays off until the next start.
bool row_set(Row& r, bool on) {
    if (on && !r.installed) {
        log_line("row %-12s not installed at startup - takes effect at the next start", r.id);
        return false;
    }
    if ((r.on != 0) != on) log_line("row %-12s %s at runtime", r.id, on ? "ON" : "OFF");
    r.on = on ? 1 : 0;
    return true;
}

Row** x2_row_list() { return g_rows; }

void rows_pre() {
    if (perf_on()) {
        for (Row** p = g_rows; *p; p++) if ((*p)->on && (*p)->pre) { LONGLONG t = perf_now(); (*p)->pre(); perf_row((int)(p - g_rows), perf_now() - t); }
        return;
    }
    for (Row** p = g_rows; *p; p++) if ((*p)->on && (*p)->pre) (*p)->pre();
}

void rows_post() {
    if (perf_on()) {
        for (Row** p = g_rows; *p; p++) if ((*p)->on && (*p)->post) { LONGLONG t = perf_now(); (*p)->post(); perf_row((int)(p - g_rows), perf_now() - t); }
        return;
    }
    for (Row** p = g_rows; *p; p++) if ((*p)->on && (*p)->post) (*p)->post();
}

// Dev hot reload: each installed row undoes the state it holds between invocations or outside its hooks.
void rows_stop_all() {
    for (Row** p = g_rows; *p; p++)
        if ((*p)->installed && (*p)->stop) {
            __try { (*p)->stop(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { log_line("rows: %s stop faulted", (*p)->id); }
        }
}

} // namespace x2
