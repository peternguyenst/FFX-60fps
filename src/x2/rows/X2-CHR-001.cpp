// SPDX-License-Identifier: Apache-2.0
// X2-CHR-001: characters' extra models (Full Throttle's swords and fire) are drawn on every frame.
//
// Character models get onto the HD renderer's model list only through the third loop of Ch_DrawMain, and in battle that
// list is cleared every invocation. The loop runs for an actor only when its "step" flag is set: by the logic actor
// update, and on render-only invocations by X2-SMOOTH-003's half step. Actors that take no half step (Full Throttle's
// swords) were listed on logic invocations only, so drawn every second frame.
// The fix: the models that loop adds on a logic invocation are recorded; at the end of each render-only invocation, a
// recorded model missing from the list is added again, if an active actor still owns it. Nothing else is re-run
// (no double speed); the model draws with the last tick's pose, as in the stock game. Evidence: re/ledger/FT-BLINK.md.
#include "../x2.h"

namespace x2 {

typedef void(__cdecl* add_t)(void* hdmodel);
enum { MAXREC = 128, STRIDE = 0x910 };

static add_t     g_add = nullptr;                // FUN_00449f20 (cdecl, plain RET)
static uint8_t*  g_site = nullptr;               // 0x6DCD98
static uint8_t** g_hdframe = nullptr;            // &DAT_00dcda04
static uint8_t* volatile* g_table = nullptr;     // actor array pointer, count, stride 0x910
static volatile int32_t*  g_acount = nullptr;
static void*     g_rec[MAXREC];
static int       g_nrec = 0;
static bool      g_patched = false;
static volatile LONG g_readded = 0, g_invs = 0;
extern Row row_CHR001;

static void __cdecl h_add(void* hdmodel) {
    if (!render_only() && g_nrec < MAXREC) g_rec[g_nrec++] = hdmodel;
    g_add(hdmodel);
}

static bool in_list(uint8_t* hf, void* m) {
    int32_t n = *(int32_t*)(hf + 0xE9CC);
    void** arr = *(void***)(hf + 0xE9D4);
    if (!arr || n < 0 || n > 4096) return true;               // unknown state: do nothing
    for (int i = 0; i < n; i++) if (arr[i] == m) return true;
    return false;
}

static bool owned(void* m) {
    uint8_t* base = *g_table;
    int n = *g_acount;
    if (!base || n <= 0 || n > 512) return false;
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * STRIDE;
        if (a[2] && *(void**)(a + 0x844) == m) return true;
    }
    return false;
}

static void pre() {
    if (!g_patched) {
        g_patched = true;
        if (!patch_call_rel32(g_site, (void*)g_add, (void*)&h_add)) { log_line("X2-CHR-001: CALL patch failed - row off"); row_CHR001.on = 0; return; }
        log_line("X2-CHR-001: loop-3 model-list add CALL patched");
    }
    if (!render_only()) g_nrec = 0;                           // a logic invocation records its own adds
}

static void post() {
    if (g_disabled || !render_only() || !g_nrec) return;
    __try {
        uint8_t* hf = *g_hdframe;
        if (!hf) return;
        int added = 0;
        for (int i = 0; i < g_nrec; i++) {
            void* m = g_rec[i];
            if (!m || in_list(hf, m) || !owned(m)) continue;
            g_add(m);
            added++;
        }
        if (added) {
            LONG n = InterlockedAdd(&g_readded, added);
            if (InterlockedIncrement(&g_invs) == 1)
                log_line("X2-CHR-001: first render-only re-add of %d chr model(s) missing from the HD list", added);
            (void)n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (g_readded && now - last >= 10000) { last = now; log_line("X2-CHR-001: models re-added so far %ld (in %ld render-only invocations)", g_readded, g_invs); }
}

static bool install() {
    uint8_t* a = sig_find("X2-CHR-001 loop-3 model add",
        "FF B6 44 08 00 00 E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 08 E8 ?? ?? ?? ?? 3D 2B 02 00 00", 0x6DCD92);
    if (!a) return false;
    g_site = a + 6;
    uint8_t* thunk = call_target(g_site);
    // FUN_00449f20: PUSH EBP; MOV EBP,ESP; PUSH [EBP+8]; MOV ECX,[hdframe]; CALL add; POP EBP; RET (plain)
    if (!thunk || !sig_match(thunk, "55 8B EC FF 75 08 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 5D C3")) {
        log_line("  X2-CHR-001: add thunk not as proven - off");
        return false;
    }
    uint8_t* add = call_target(thunk + 12);
    // FUN_004664a0: INC [ECX+0xE9CC]; ... MOV EAX,[ESI+8] (array +0xE9D4); RET 4
    if (!add || !sig_match(add, "55 8B EC FF 81 CC E9 00 00 56 8D B1 CC E9 00 00 8B 0E 3B 4E 04") ||
        !sig_match(add + 0x2F, "8B 0E 8B 46 08")) {
        log_line("  X2-CHR-001: list add layout (+0xE9CC count, +0xE9D4 array) not as proven - off");
        return false;
    }
    g_add = (add_t)thunk;
    g_hdframe = (uint8_t**)abs_operand("hdframe ptr", thunk + 8);
    uint8_t* tb = sig_find("X2-CHR-001 actor table", "8B 0D ?? ?? ?? ?? 8B 35 ?? ?? ?? ?? 69 C9 10 09 00 00 03 CE", 0x6DBAA5);
    if (!g_hdframe || !tb) return false;
    g_acount = (volatile int32_t*)abs_operand("actor count", tb + 2);
    g_table = (uint8_t* volatile*)abs_operand("actor table", tb + 8);
    if (!g_acount || !g_table) return false;
    log_line("  X2-CHR-001: loop-3 add CALL gva=0x%08X (expected 0x006DCD98), thunk gva=0x%08X, hdframe ptr gva=0x%08X",
             rt_to_gva(g_site), rt_to_gva(thunk), rt_to_gva(g_hdframe));
    return true;
}

Row row_CHR001 = {
    "X2-CHR-001",
    "chr models listed by Ch_DrawMain loop 3 on a logic invocation are re-listed on render-only invocations (Full Throttle swords)",
    1, install, pre, post, 0
};

} // namespace x2
