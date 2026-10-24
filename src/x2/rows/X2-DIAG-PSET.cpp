// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-PSET (diagnostic, off): logs who sets the global particle pause flag.
//
// Every call of pppFpStopStatus (it sets the flag to its argument; callers: a battle-script command and the debug
// menu) is logged with the invocation kind and three return addresses, and one line when the flag has stayed set for
// 5 s. Written for the battle soft-lock that X2-MAG-031 fixes (re/ledger/HANG-0073.md).
#include "../x2.h"

namespace x2 {

extern Row row_DIAGPSET;

typedef void(__cdecl* set_t)(int);
static uint8_t* g_fn = nullptr;
static set_t    g_orig = nullptr;
static volatile int32_t* g_flag = nullptr;
static LONG     g_calls = 0, g_inv = 0;

static void __cdecl h_set(int v) {
    void* fr[4] = {};
    USHORT k = RtlCaptureStackBackTrace(1, 3, fr, nullptr);
    uint32_t r[3] = {};
    for (int i = 0; i < k && i < 3; i++) r[i] = in_image(fr[i]) ? rt_to_gva(fr[i]) : (uint32_t)(uintptr_t)fr[i];
    if (InterlockedIncrement(&g_calls) <= 2000)
        log_line("PSET #%ld %s pppFpStopStatus(%d) (was %d) <- %08X <- %08X <- %08X", g_inv, render_only() ? "R" : "L", v,
                 g_flag ? (int)*g_flag : -99, r[0], r[1], r[2]);
    g_orig(v);
}

static bool install() {
    // PUSH EBP; MOV EBP,ESP; PUSH ESI; MOV ESI,[EBP+8]; PUSH ESI; PUSH fmt; CALL log; ADD ESP,8; MOV [flag],ESI; POP; POP; RET
    g_fn = sig_find("X2-DIAG-PSET pppFpStopStatus",
        "55 8B EC 56 8B 75 08 56 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 08 89 35 ?? ?? ?? ?? 5E 5D C3", 0x810EF0);
    if (!g_fn) return false;
    g_flag = (volatile int32_t*)abs_operand("particle pause flag", g_fn + 0x17);
    if (!g_flag) return false;
    log_line("  X2-DIAG-PSET: pppFpStopStatus gva=0x%08X, flag gva=0x%08X (expected 0x01CF579C)", rt_to_gva(g_fn),
             rt_to_gva((const void*)g_flag));
    return true;
}

static void pre() {
    static bool done = false;
    if (done) return;
    done = true;
    log_line("X2-DIAG-PSET: hook %s", hook_entry("X2-DIAG-PSET", g_fn, (void*)&h_set, (void**)&g_orig) ? "on" : "FAILED");
}

static void post() {
    g_inv++;
    static DWORD since = 0;
    static bool told = false;
    if (!g_flag || *g_flag == 0) { since = 0; told = false; return; }
    DWORD now = GetTickCount();
    if (!since) since = now;
    if (!told && now - since >= 5000) {
        told = true;
        log_line("X2-DIAG-PSET: particle pause flag = %d at the end of invocations for 5 s (soft-lock?)", (int)*g_flag);
    }
}

Row row_DIAGPSET = {
    "X2-DIAG-PSET",
    "diagnostic: pppFpStopStatus calls (particle pause flag set by battle scripts) and a 5 s stuck-flag warning",
    0, install, pre, post, 0
};

} // namespace x2
