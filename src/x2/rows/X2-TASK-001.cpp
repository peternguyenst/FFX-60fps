// SPDX-License-Identifier: Apache-2.0
// X2-TASK-001 (FFX-2 only): actor render tasks queued by a script command are drawn on logic invocations only.
// The actor-script command FUN_00695240 (script op table 0xD4FF30) queues one-shot tasks through FUN_00680030
// (cdecl(fn, a1, a2, lifetime), group 0x60, plain RET): sub-op 0xA -> 0x694C70, 0xB -> 0x694EF0, 0x12 -> 0x6951D0
// (draws through FUN_00696cc0 and sets the map/char draw-enable source DAT_00e48ee0 = 1), all with lifetime 1. The
// task runner FUN_00680980 (Sg_MainLoop second loop) runs them once, in the invocation that queued them. Scripts run on
// logic invocations only, so at 60 fps these draws exist on every second frame: U 16/10/2026, a battle move used by
// Seymour (enemy model and orb missing on alternate frames). H: that this command is the one that move uses.
// Fix: the lifetime-1 tasks of those three functions queued on a logic invocation are queued again, with the same
// arguments, at the start of the next render-only invocation (they run in its task pass). The task data (actor
// +0xBC block) lives across ticks; only the script that queued it can free it, and it does not run in between.
// Also restores the stock DAT_00e48ee0 sequence (1 every invocation instead of 1,0,1,0).
#include "../x2.h"

namespace x2 {

extern Row row_TASK001;

typedef int(__cdecl* create_t)(uint32_t fn, int a1, int a2, int lifetime);
static create_t g_create = nullptr;               // MinHook trampoline
static uint8_t* g_create_fn = nullptr;
static uint32_t g_fns[3] = {};                    // runtime addresses of 0x694C70, 0x694EF0, 0x6951D0
static bool     g_hooked = false;

struct Rec { uint32_t fn; int a1, a2; };
enum { MAXREC = 32 };
static Rec  g_rec[MAXREC];
static int  g_nrec = 0;
static bool g_overflow = false;
static volatile LONG g_queued = 0, g_replayed = 0, g_failed = 0;

static int __cdecl h_create(uint32_t fn, int a1, int a2, int lifetime) {
    if (row_TASK001.on && !g_disabled && lifetime == 1 && pacing_kind() == INV_LOGIC &&
        (fn == g_fns[0] || fn == g_fns[1] || fn == g_fns[2])) {
        if (g_nrec < MAXREC) { g_rec[g_nrec++] = { fn, a1, a2 }; InterlockedIncrement(&g_queued); }
        else g_overflow = true;
    }
    return g_create(fn, a1, a2, lifetime);
}

static bool install() {
    // FUN_00680030: PUSH EBP; MOV EBP,ESP; PUSH 0x60; PUSH [EBP+0x14]; PUSH [EBP+0x10]; PUSH [EBP+0xC]; PUSH [EBP+8];
    // CALL FUN_00680120; ADD ESP,0x14; POP EBP; RET
    g_create_fn = sig_find("X2-TASK-001 task create group 0x60 FUN_00680030",
        "55 8B EC 6A 60 FF 75 14 FF 75 10 FF 75 0C FF 75 08 E8 ?? ?? ?? ?? 83 C4 14 5D C3", 0x680030);
    // FUN_00695240 sub-op 0x12: PUSH 1; PUSH 0; PUSH <iVar8>; PUSH 0x6951D0; CALL FUN_00680030
    uint8_t* q = sig_find("X2-TASK-001 script op 0x12 render task",
        "E8 ?? ?? ?? ?? 6A 01 6A 00 57 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 44 83 C3 06", 0x695898);
    if (q) q += 5;                               // PUSH 1 at 0x69589D
    // sub-ops 0xA / 0xB: PUSH 1; PUSH <pc>; PUSH <iVar8>; PUSH 0x694C70 / 0x694EF0; CALL FUN_00680030
    if (!g_create_fn || !q) return false;
    uint32_t base = (uint32_t)exe_base() - 0x400000;
    uint32_t f12 = *(uint32_t*)(q + 6);         // PUSH 1; PUSH 0; PUSH EDI; PUSH imm32 (+5); CALL (+10)
    if (call_target(q + 10) != g_create_fn || f12 != base + 0x6951D0) {
        log_line("  X2-TASK-001: op 0x12 site does not queue 0x6951D0 through FUN_00680030 - refused");
        return false;
    }
    // the 0xA / 0xB pushes sit in the same function; verified by value at runtime (this build only)
    uint8_t* pa = exe_base() + (0x69556B - 0x400000 - 1);
    uint8_t* pb = exe_base() + (0x69558E - 0x400000 - 1);
    if (pa[0] != 0x68 || *(uint32_t*)(pa + 1) != base + 0x694C70 || pb[0] != 0x68 || *(uint32_t*)(pb + 1) != base + 0x694EF0 ||
        call_target(pa + 5) != g_create_fn || call_target(pb + 5) != g_create_fn) {
        log_line("  X2-TASK-001: op 0xA/0xB sites not as expected (other build) - refused");
        return false;
    }
    g_fns[0] = base + 0x694C70;
    g_fns[1] = base + 0x694EF0;
    g_fns[2] = f12;
    log_line("  X2-TASK-001: task create gva=0x%08X, op sites 0x%08X/0x%08X/0x%08X; hook at the first pre()",
             rt_to_gva(g_create_fn), rt_to_gva(pa), rt_to_gva(pb), rt_to_gva(q));
    return true;
}

static void pre() {
    if (!g_hooked) {
        g_hooked = true;                         // game thread, between invocations
        if (!hook_entry("X2-TASK-001", g_create_fn, (void*)&h_create, (void**)&g_create)) {
            log_line("X2-TASK-001: hook failed - row off");
            row_TASK001.on = 0;
        }
        return;
    }
    if (g_disabled || !g_create) return;
    int kind = pacing_kind();
    if (kind == INV_RENDER_ONLY) {
        for (int i = 0; i < g_nrec; i++) {
            if (g_create(g_rec[i].fn, g_rec[i].a1, g_rec[i].a2, 1) < 0) InterlockedIncrement(&g_failed);
            else InterlockedIncrement(&g_replayed);
        }
    }
    g_nrec = 0;                                  // logic: this invocation's script queues afresh; others: drop
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_replayed) {
        reported = 1;
        log_line("X2-TASK-001: first actor render task replayed on a render-only invocation (queued %ld)", g_queued);
    }
    static DWORD last = 0;
    static LONG last_q = 0;
    DWORD now = GetTickCount();
    if (now - last >= 10000 && g_queued != last_q) {
        last = now;
        last_q = g_queued;
        log_line("X2-TASK-001: queued %ld, replayed %ld, failed %ld%s", g_queued, g_replayed, g_failed,
                 g_overflow ? " (record list overflowed)" : "");
    }
}

Row row_TASK001 = {
    "X2-TASK-001",
    "actor render tasks of script command FUN_00695240 (0xA/0xB/0x12) replayed on render-only invocations (battle move blink)",
    1, install, pre, post, 0
};

} // namespace x2
