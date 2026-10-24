// SPDX-License-Identifier: Apache-2.0
// X2-MAG-020: "family X" spells step their effect nodes once per tick (no soft-locks or double-speed effects).
//
// These spell DLLs walk their effect nodes from their draw. The walk runs each node's state handler (waits, spawns,
// and the "walk complete" signal the battle waits for), so at 60 fps every handler ran twice per tick (in FFX this
// soft-locked Bad Breath). On render-only invocations the walk only calls each live node's draw callback; state
// handlers and the removal of finished nodes run on logic invocations. The CALL of the node walk (gva 0x7DEB36) goes
// to our wrapper, patched on the game thread. Evidence: re/ledger/FAMX.md; signatures tools/sigs_x2_famx.txt.
#include "../x2.h"

namespace x2 {

typedef void(__cdecl* walk_t)(uint8_t* head);
typedef void(__cdecl* node_fn_t)(uint8_t* node);

static walk_t   g_walk = nullptr;
static uint8_t* g_site = nullptr;
static bool     g_patched = false;
static volatile LONG g_draw_only = 0;
extern Row row_MAG020;

static void __cdecl h_walk(uint8_t* head) {
    if (row_MAG020.on && !g_disabled && render_only()) {
        int guard = 0;
        for (uint8_t* nd = head; nd && guard < 4096; nd = *(uint8_t**)(nd + 8), guard++) {
            uint32_t* u = (uint32_t*)nd;
            void* cb = (void*)(uintptr_t)u[0x2C];
            if (cb && !(u[0] & 4)) mag026_draw_record(head, nd, cb);   // X2-MAG-026 off: plain cb(nd)
        }
        InterlockedIncrement(&g_draw_only);
        return;
    }
    g_walk(head);
}

static bool install() {
    // FUN_007dead0: MOV ECX,[EDI+0xC]; LEA ESI,[EDI+0x1C]; PUSH ESI; MOV [EAX],ECX; CALL walk; PUSH ESI; CALL empty?;
    // ADD ESP,0x20; TEST EAX,EAX; JZ; PUSH [EDI+4]; CALL battle signal
    uint8_t* at = sig_find("X2-MAG-020 node walk call (FUN_007dead0)",
        "8B 4F 0C 8D 77 1C 56 89 08 E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 20 85 C0 74 0B FF 77 04 E8", 0x7DEB2D);
    if (!at) return false;
    g_site = at + 9;
    uint8_t* walk = call_target(g_site);
    // FUN_007ea8d0: for each node FUN_007e91c0(node) (state handler loop, then node[0x2C]); then free flag-4 nodes
    if (!walk || !sig_match(walk, "55 8B EC 56 8B 75 08 57 8B FE 85 F6 74 ?? 8B FF 57 E8")) {
        log_line("  X2-MAG-020: CALL target %p is not the node walk FUN_007ea8d0 - refused", walk);
        return false;
    }
    g_walk = (walk_t)walk;
    log_line("  X2-MAG-020: node walk CALL gva=0x%08X (expected 0x007DEB36) -> FUN gva=0x%08X (expected 0x007EA8D0)",
             rt_to_gva(g_site), rt_to_gva(walk));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!patch_call_rel32(g_site, (void*)g_walk, (void*)&h_walk)) {
        log_line("X2-MAG-020: CALL patch failed - row off");
        row_MAG020.on = 0;
        return;
    }
    log_line("X2-MAG-020: family-X node walk patched: render-only invocations draw nodes only");
}

static void post() {
    static DWORD last = 0;
    static LONG seen = 0;
    DWORD now = GetTickCount();
    if (now - last >= 10000 && g_draw_only != seen) {
        last = now;
        seen = g_draw_only;
        log_line("X2-MAG-020: family-X node walks drawn only on render-only invocations so far: %ld", seen);
    }
}

Row row_MAG020 = {
    "X2-MAG-020",
    "family-X effect node walk (FUN_007dead0): render-only invocations draw nodes only; state handlers at logic rate",
    1, install, pre, post, 0
};

} // namespace x2
