// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-HIT (diagnostic, off): logs every battle hit check, to find hits that get lost.
//
// For every call of the hit check (FUN_006423d0: attacker, target, command, mode) it logs the invocation kind, the
// target's hit counters (done / total / missed) and the results of the check's two inner tests (FUN_00643110, and the
// angle / distance / height test FUN_0063f010). A failed check before any hit uses up all remaining hits on that
// target (no number, no reaction). At most 3000 lines. Written for Blitz Ace's lost hits (fixed by X2-MAG-030-DMG);
// re/ledger/DMGNUM.md.
#include "../x2.h"

namespace x2 {

extern Row row_DIAGHIT;

typedef int(__cdecl* check_t)(uint32_t att, uint32_t tgt, uint8_t* cmd, int mode);
typedef int(__cdecl* sel_t)(uint32_t, uint32_t, uint32_t, uint8_t*, int*, int);
typedef int(__cdecl* ang_t)(uint32_t, uint32_t, uint32_t, float);
static uint8_t* g_check_fn = nullptr; static check_t g_check = nullptr;
static uint8_t* g_sel_site[2] = {}; static sel_t g_sel = nullptr;
static uint8_t* g_ang_site = nullptr; static ang_t g_ang = nullptr;
static int  g_last_sel = -9, g_last_ang = -9;
static LONG g_lines = 0, g_inv = 0;

static int __cdecl h_sel(uint32_t a, uint32_t b, uint32_t c, uint8_t* d, int* e, int f) {
    int r = g_sel(a, b, c, d, e, f);
    g_last_sel = r;
    return r;
}
static int __cdecl h_ang(uint32_t a, uint32_t b, uint32_t c, float lim) {
    int r = g_ang(a, b, c, lim);
    g_last_ang = r;
    return r;
}
static int __cdecl h_check(uint32_t att, uint32_t tgt, uint8_t* cmd, int mode) {
    g_last_sel = g_last_ang = -9;
    int done = -1, total = -1, miss = -1, type = -1;
    __try { done = cmd[0x50 + tgt]; total = cmd[0x6F + tgt]; miss = cmd[0x31 + tgt]; type = cmd[0x28]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    int r = g_check(att, tgt, cmd, mode);
    if (InterlockedIncrement(&g_lines) <= 3000)
        log_line("HIT #%ld %s check att %u tgt %u cmd %p type %d mode %d | done %d/%d miss %d | sel %d angle %d -> %d%s", g_inv,
                 render_only() ? "R" : "L", att, tgt, cmd, type, mode, done, total, miss, g_last_sel, g_last_ang, r,
                 r ? "" : "  <-- FAIL");
    return r;
}

static bool install() {
    g_check_fn = sig_find("X2-DIAG-HIT hit check FUN_006423d0",
        "55 8B EC 83 EC 08 53 8B 5D 08 56 57 53 C7 45 FC 03 00 00 00 BE 02 00 00 00 89 5D 08", 0x6423D0);
    if (!g_check_fn) return false;
    uint8_t* s0 = g_check_fn + 0x5C; uint8_t* s1 = g_check_fn + 0x8E; uint8_t* a0 = g_check_fn + 0x7A;
    uint8_t* t0 = call_target(s0); uint8_t* t1 = call_target(s1); uint8_t* ta = call_target(a0);
    if (!t0 || t0 != t1 || !ta || !in_image(t0) || !in_image(ta) ||
        !sig_match(ta, "55 8B EC 83 EC 48 A1 ?? ?? ?? ?? 33 C5 89 45 FC 53 56 8B 75 0C 57 56 E8")) {
        log_line("  X2-DIAG-HIT: inner CALLs not as proven - off");
        return false;
    }
    g_sel_site[0] = s0; g_sel_site[1] = s1; g_sel = (sel_t)t0;
    g_ang_site = a0; g_ang = (ang_t)ta;
    log_line("  X2-DIAG-HIT: check 0x%08X (0x006423D0), select 0x%08X (0x00643110), angle 0x%08X (0x0063F010)",
             rt_to_gva(g_check_fn), rt_to_gva(t0), rt_to_gva(ta));
    return true;
}

static void pre() {
    static bool done = false;
    if (done) return;
    done = true;                                 // game thread, between invocations
    bool ok = patch_call_rel32(g_sel_site[0], (void*)g_sel, (void*)&h_sel) &&
              patch_call_rel32(g_sel_site[1], (void*)g_sel, (void*)&h_sel) &&
              patch_call_rel32(g_ang_site, (void*)g_ang, (void*)&h_ang) &&
              hook_entry("X2-DIAG-HIT", g_check_fn, (void*)&h_check, (void**)&g_check);
    log_line("X2-DIAG-HIT: hooks %s", ok ? "on" : "FAILED");
}

static void post() { g_inv++; }

Row row_DIAGHIT = {
    "X2-DIAG-HIT",
    "diagnostic: battle hit checks (FUN_006423d0) with counters and inner test results per invocation kind",
    0, install, pre, post, 0
};

} // namespace x2
