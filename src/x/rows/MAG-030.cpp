// SPDX-License-Identifier: Apache-2.0
// MAG-030: battle OPP effect units step inside their draw. OppMain_EffectWalk (0x80BD50, ledger FX-004..007) runs once
// per invocation from the second loop, and the EXE handlers in its table (0xC48E78, 20 slots) for op 1 (keyframes),
// op 9 (particle sub-units in *(rec+0xA8)), op 10 (texture scroll) and op 17 (trail ring in *(rec+0xBC)) advance their
// state on every call; none of them tests the engine's own pause flag scratch+0x212. At N passes they run N times per
// logic tick (U 24/10/2026, 120 fps: Wakka's ball effect fast after MAG-029 fixed the orbs and breath).
// On render-only invocations this row lets the handler run (it draws), then puts the record (0x100 bytes) and the op's
// buffer back as they were before the call. A unit that ended during the call (+0xB0 bit 0x8000, or a new op byte) is
// left as the handler made it. Every non-null slot is wrapped so the log shows which ops a scene runs on render-only
// invocations; only ops in MAG-030-OPS (hex list, default 1,9,A,11) are restored.
// Details: docs/fps60/rows/MAG-030.md
#include "../fps60.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void(__cdecl* opp_fn_t)(uint8_t* scratch, uint8_t* ctx);

enum { SLOTS = 20, REC = 0x100, BUF_MAX = 0x10000 };

static void**   g_table = nullptr;
static opp_fn_t g_orig[SLOTS];
static uint32_t g_restore_mask = (1u << 1) | (1u << 9) | (1u << 10) | (1u << 17);
static bool     g_patched = false;
static volatile LONG g_calls[SLOTS], g_restored[SLOTS], g_ended[SLOTS], g_toolarge = 0;

// MAG-030-LIFE (diag, default 0): unit lifetimes in logic ticks, to compare stock (DEV-STOCK) with mod pacing.
// A unit is a record seen by a handler on a logic/stock invocation; it ends at +0xB0 bit 0x8000 or when no handler
// sees it for 2 ticks. Game thread only.
enum { LIFE_MAX = 1024 };
struct Life { uint8_t* rec; uint32_t born, last; uint8_t op; };
static int      g_life_on = 0;
static Life     g_life[LIFE_MAX];
static uint32_t g_tick = 0;                       // logic ticks (logic + stock invocations)
static int      g_live_now = 0, g_live_peak[2];   // [0] mod, [1] stock
static uint32_t g_ends[2], g_life_sum[2], g_life_max[2], g_new[2];

static void life_end(Life& e, int stock) {
    uint32_t t = e.last - e.born + 1;
    g_ends[stock]++;
    g_life_sum[stock] += t;
    if (t > g_life_max[stock]) g_life_max[stock] = t;
    e.rec = nullptr;
}

static void life_seen(uint8_t* rec, int op, bool ended) {
    int kind = pacing_kind();
    if (kind != INV_LOGIC && kind != INV_STOCK) return;
    int stock = kind == INV_STOCK, freei = -1;
    for (int i = 0; i < LIFE_MAX; i++) {
        if (g_life[i].rec == rec) {
            if (g_life[i].last != g_tick) { g_life[i].last = g_tick; g_live_now++; }
            if (ended) life_end(g_life[i], stock);
            return;
        }
        if (!g_life[i].rec && freei < 0) freei = i;
    }
    if (freei < 0 || ended) return;
    g_life[freei] = { rec, g_tick, g_tick, (uint8_t)op };
    g_new[stock]++;
    g_live_now++;
}

// The op's own buffer: op 9 sub-units (header 0x20, count at +0, 0x50 per entry), op 17 trail ring (count at +4,
// data from +0x10, 0x20 per entry). Other ops keep their state in the record.
static uint8_t* op_buffer(int op, uint8_t* rec, uint32_t* len) {
    uint8_t* buf = nullptr;
    uint16_t n = 0;
    if (op == 9) {
        if (!safe_copy(&buf, rec + 0xA8, 4) || !buf || !safe_copy(&n, buf, 2)) return nullptr;
        *len = 0x20 + (uint32_t)n * 0x50;
    } else if (op == 17) {
        if (!safe_copy(&buf, rec + 0xBC, 4) || !buf || !safe_copy(&n, buf + 4, 2)) return nullptr;
        *len = 0x10 + (uint32_t)n * 0x20;
    } else {
        return nullptr;
    }
    if (*len > BUF_MAX) { InterlockedIncrement(&g_toolarge); return nullptr; }
    return buf;
}

static void call_op(int op, uint8_t* scratch, uint8_t* ctx) {
    if (g_life_on && !g_disabled && pacing_kind() != INV_BUILD) {
        uint8_t* rec = nullptr;
        uint16_t f0 = 0, f1 = 0;
        bool have = safe_copy(&rec, scratch + 0x220, 4) && rec && safe_copy(&f0, rec + 0xB0, 2);
        g_orig[op](scratch, ctx);
        if (have) {
            safe_copy(&f1, rec + 0xB0, 2);
            life_seen(rec, op, (f1 & 0x8000) && !(f0 & 0x8000));
        }
        return;
    }
    if (g_disabled || !row_MAG030.on || pacing_kind() != INV_BUILD) { g_orig[op](scratch, ctx); return; }
    InterlockedIncrement(&g_calls[op]);
    uint8_t* rec = nullptr;
    if (!(g_restore_mask >> op & 1) || !safe_copy(&rec, scratch + 0x220, 4) || !rec) { g_orig[op](scratch, ctx); return; }
    static uint8_t save_rec[REC], save_buf[BUF_MAX];       // game thread only
    uint32_t len = 0;
    if (!safe_copy(save_rec, rec, REC)) { g_orig[op](scratch, ctx); return; }
    uint8_t* buf = op_buffer(op, rec, &len);
    if (buf && !safe_copy(save_buf, buf, len)) buf = nullptr;
    g_orig[op](scratch, ctx);
    uint16_t flags = 0;
    uint8_t opb = 0;
    safe_copy(&flags, rec + 0xB0, 2);
    safe_copy(&opb, rec + 0xBB, 1);
    uint16_t flags0 = *(uint16_t*)(save_rec + 0xB0);
    if (((flags & 0x8000) && !(flags0 & 0x8000)) || opb != save_rec[0xBB]) { InterlockedIncrement(&g_ended[op]); return; }
    safe_copy(rec, save_rec, REC);
    if (buf) safe_copy(buf, save_buf, len);
    InterlockedIncrement(&g_restored[op]);
}

template <int I> static void __cdecl h_op(uint8_t* scratch, uint8_t* ctx) { call_op(I, scratch, ctx); }
static const opp_fn_t g_wrap[SLOTS] = {
    h_op<0>, h_op<1>, h_op<2>, h_op<3>, h_op<4>, h_op<5>, h_op<6>, h_op<7>, h_op<8>, h_op<9>,
    h_op<10>, h_op<11>, h_op<12>, h_op<13>, h_op<14>, h_op<15>, h_op<16>, h_op<17>, h_op<18>, h_op<19>,
};

static uint32_t parse_mask(const char* s) {
    uint32_t m = 0;
    while (s && *s) {
        char* end = nullptr;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s) { s++; continue; }
        if (v < SLOTS) m |= 1u << v;
        s = end;
    }
    return m;
}

static bool install() {
    // OppMain_EffectWalk EXE dispatch: MOVZX EAX,AL; MOV EAX,[EAX*4+table]; TEST EAX,EAX; JNZ call; PUSH msg
    uint8_t* at = sig_find_unique("MAG-030 OPP handler table dispatch (OppMain_EffectWalk)",
        "0F B6 C0 8B 04 85 ?? ?? ?? ?? 85 C0 0F 85 ?? ?? ?? ?? 68", 0x80C3C5);
    if (!at) return false;
    g_table = (void**)read_abs(at + 6);
    if (!g_table) return false;
    char ops[128];
    config_str("rows", "MAG-030-OPS", ops, sizeof(ops), "");
    if (ops[0]) g_restore_mask = parse_mask(ops);
    g_life_on = config_row("MAG-030-LIFE", 0);
    // The handlers the ledger names (FX-004..006), checked so a shifted table is refused.
    static const struct { int op; uint32_t gva; } known[] = { {1, 0x811120}, {9, 0x8124B0}, {10, 0x811530}, {17, 0x811BC0} };
    for (auto& k : known) {
        void* want = exe_base() + (k.gva - 0x400000);
        if (g_table[k.op] != want) {
            log_line("  MAG-030: table gva=0x%08X slot %d = %p, not the op handler gva=0x%08X - refused",
                     rt_to_gva_exe(g_table), k.op, g_table[k.op], k.gva);
            return false;
        }
    }
    log_line("  MAG-030: OPP table gva=0x%08X, restore mask 0x%05X (MAG-030-OPS)", rt_to_gva_exe(g_table), g_restore_mask);
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    int n = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (!g_table[i]) continue;
        g_orig[i] = (opp_fn_t)g_table[i];
        void* w = (void*)g_wrap[i];
        if (!patch_write(&g_table[i], &w, 4)) { log_line("MAG-030: slot %d patch failed", i); continue; }
        n++;
    }
    log_line("MAG-030: %d OPP table slots wrapped on the game thread", n);
}

static void life_post() {
    int kind = pacing_kind();
    if (kind != INV_LOGIC && kind != INV_STOCK) return;
    int stock = kind == INV_STOCK;
    if (g_live_now > g_live_peak[stock]) g_live_peak[stock] = g_live_now;
    g_live_now = 0;
    for (int i = 0; i < LIFE_MAX; i++)            // not seen for 2 ticks: ended without the flag (freed/reused)
        if (g_life[i].rec && g_tick - g_life[i].last >= 2) life_end(g_life[i], stock);
    g_tick++;
    static uint32_t n = 0;
    if (++n % 300) return;
    for (int k = 0; k < 2; k++) {
        if (!g_ends[k] && !g_new[k] && !g_live_peak[k]) continue;
        log_line("MAG-030-LIFE %s: units new %u, ended %u, life avg %.1f max %u logic ticks, peak live %d",
                 k ? "stock" : "mod", g_new[k], g_ends[k], g_ends[k] ? (double)g_life_sum[k] / g_ends[k] : 0.0,
                 g_life_max[k], g_live_peak[k]);
        g_new[k] = g_ends[k] = g_life_sum[k] = g_life_max[k] = 0;
        g_live_peak[k] = 0;
    }
}

static void post() {
    if (g_life_on && !g_disabled) life_post();
    static uint32_t tick = 0;
    if (++tick % (300 * (uint32_t)rate_mult()) != 0) return;
    char line[512];
    int k = 0;
    for (int i = 0; i < SLOTS; i++) {
        LONG c = InterlockedExchange(&g_calls[i], 0), r = InterlockedExchange(&g_restored[i], 0),
             e = InterlockedExchange(&g_ended[i], 0);
        if (c) k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " op%d:%ld/%ld/%ld", i, c, r, e);
    }
    if (k) log_line("MAG-030: render-only OPP calls (op:calls/restored/ended):%s%s", line,
                    g_toolarge ? " (buffers over 64 KB skipped)" : "");
}

Row row_MAG030 = {
    "MAG-030",
    "battle OPP effect units (ops 1/9/10/17): render-only draws put record and buffer back (Wakka's ball)",
    1, install, pre, post, 0
};
