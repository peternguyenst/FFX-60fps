// SPDX-License-Identifier: Apache-2.0
// X2-SR-003 (feature, port of FFX SR-003 + SR-008): soft reset. Holding L1+L2+R1+R2+Start+Select returns to the title.
// Field: the game's own "return to title" request DAT_016bc51c = 1 (the pause screen's item writes it); the next
// Sg_MainLoop calls the game-over return to title FUN_00728340 (FFX FUN_0086da10 shape: stops sound/events, forces the
// event context flag, stages map 0x17) at 0x605182 and sets it to 2.
// Battle: the fight is ended as a defeat through the game's own path: bit 0x200 ("loss allowed") of DAT_0119347c is
// cleared and the pending result DAT_00df84b5 = 1 (what the party check FUN_0061ef70 returns when the party is wiped);
// the battle tears itself down (state 0x26, FUN_00607c70) and the result handler FUN_0071b640 calls FUN_00728340.
// FFX-2 needs no state 1 -> 2 kick (case 1 re-reads the result every battle tick). The bit is held clear until the
// battle is gone. Never result 6 (quits the app).
// ini (fps60x2.ini, as FFX fps60.ini [fps60]): [x2] soft_reset=<buttons joined by +> or 0/off; default
// L1+L2+R1+R2+START+SELECT. Names: L1 L2 R1 R2 L3 R3 START SELECT TRIANGLE CIRCLE CROSS SQUARE UP DOWN LEFT RIGHT, or a
// raw mask 0x090F. FFX-2 pad bits as FFX (SOFTRESET-X2.md: L2 1, R2 2, L1 4, R1 8, Select 0x100, Start 0x800, S;
// the face/d-pad names by PS2 layout, H).
// Evidence: outer repo re/ledger/SOFTRESET-X2.md (S unless tagged H). Every data address comes from a signature
// operand; the few without one are used only when every signature sits at its expected VA (this build, S).
#include "../x2.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace x2 {

extern Row row_SR003;

static volatile int32_t*  g_title_req = nullptr;   // DAT_016bc51c: 1 = title return requested (Sg_MainLoop), 2 = done
static volatile uint32_t* g_battle_flags = nullptr;   // DAT_0119347c: bit 0x200 = loss allowed
static volatile int8_t*   g_pending_result = nullptr;      // DAT_00df84b5: pending battle result (party check)
static volatile int8_t*   g_battle_state = nullptr;      // DAT_00df68a0: 0 none, 1 running, 0x26 teardown
static volatile int8_t*   g_battle_result = nullptr;     // DAT_00df68a2: final battle result
static uint8_t*           g_pad = nullptr;         // pad block 0x0119ED48 (index byte +0x9C, held word +4, 0x20 per pad)
static uint16_t           g_chord = 0x090F;
static char               g_chord_text[96];
static bool               g_chord_armed = true;

static const struct { const char* name; uint16_t bit; } BUTTONS[] = {
    {"L2", 0x0001}, {"R2", 0x0002}, {"L1", 0x0004}, {"R1", 0x0008},
    {"TRIANGLE", 0x0010}, {"CIRCLE", 0x0020}, {"CROSS", 0x0040}, {"SQUARE", 0x0080},
    {"SELECT", 0x0100}, {"L3", 0x0200}, {"R3", 0x0400}, {"START", 0x0800},
    {"UP", 0x1000}, {"RIGHT", 0x2000}, {"DOWN", 0x4000}, {"LEFT", 0x8000},
};

// "L1+L2+R1+R2+START+SELECT" (any case, separators + , space) or a raw mask "0x090F"; 0 = invalid (FFX SR-003)
static uint16_t parse_chord(const char* s) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (uint16_t)strtoul(s, nullptr, 16);
    uint16_t mask = 0;
    char tok[16];
    while (*s) {
        while (*s == '+' || *s == ',' || *s == ' ' || *s == '\t') s++;
        int n = 0;
        while (*s && *s != '+' && *s != ',' && *s != ' ' && *s != '\t') { if (n < 15) tok[n++] = (char)toupper((unsigned char)*s); s++; }
        tok[n] = 0;
        if (!n) break;
        bool found = false;
        for (auto& b : BUTTONS) if (!strcmp(tok, b.name)) { mask |= b.bit; found = true; }
        if (!found) { log_line("  X2-SR-003: unknown button '%s' in soft_reset", tok); return 0; }
    }
    return mask;
}
static int                g_battle_reset_ticks = 0;       // logic ticks since a battle reset, 0 = none

static uint8_t* at_gva(uint32_t va) { return exe_base() + (va - 0x400000); }
template <typename T> static T read_gva(uint32_t va) { return *(volatile T*)at_gva(va); }

static uint16_t pad_held() {
    uint8_t idx = g_pad[0x9C];
    return *(volatile uint16_t*)(g_pad + (idx & 3) * 0x20 + 4);
}

// FFX SR-005 equivalents (SOFTRESET-X2.md "Port plan"): 0 = may reset, else the first blocking condition.
static const char* reset_blocked_by() {
    int8_t bs = *g_battle_state;
    if (bs != 0 && bs != 1)                              return "battle starting or ending";
    if (bs == 1 && (*g_pending_result != 0 || *g_battle_result != 0)) return "battle result already decided";
    if (read_gva<int32_t>(0x016BC55C) != 0)                    return "menu screen running";
    if (read_gva<int32_t>(0x016BC524) != 0 || read_gva<int32_t>(0x016BC560) != 0 || read_gva<int32_t>(0x016BC528) != 0) return "paused";
    if (read_gva<int32_t>(0x00DF6870) != -1)                   return "screen request pending";
    if (read_gva<int32_t>(0x0119F358) != 0)                    return "map change in flight";
    if (bs == 0 && read_gva<uint32_t>(0x00D61748) == 0x17)     return "at the title";
    if (*g_title_req == 1)                               return "title return already requested";
    uint8_t* fmv = *(uint8_t**)at_gva(0x00DC594C);
    if (fmv && *(volatile uint8_t*)(fmv + 0x6CC) != 0)   return "movie playing";   // byte flag (X2-W2A-002)
    return nullptr;
}

static void battle_reset_tick() {
    int8_t bs = *g_battle_state;
    if (bs == 0) { log_line("X2-SR-003: battle torn down after %d ticks (title load follows)", g_battle_reset_ticks); g_battle_reset_ticks = 0; return; }
    *g_battle_flags &= ~0x200u;                             // a script may set it again while the battle ends
    if (++g_battle_reset_ticks == 300)
        log_line("X2-SR-003: battle still up after 10 s (state 0x%02X, result %d/%d) - a GAME OVER window may wait for a button (H)",
                 (uint8_t)bs, (int)*g_pending_result, (int)*g_battle_result);
    if (g_battle_reset_ticks > 1800) { log_line("X2-SR-003: battle did not end in 60 s (state 0x%02X) - given up", (uint8_t)bs); g_battle_reset_ticks = 0; }
}

static void post() {
    if (g_disabled || pacing_kind() == INV_RENDER_ONLY) return;
    if (g_battle_reset_ticks) { battle_reset_tick(); return; }
    bool chord = (pad_held() & g_chord) == g_chord;
    if (!chord) { g_chord_armed = true; return; }
    if (!g_chord_armed) return;
    g_chord_armed = false;                                     // once per chord: release to re-arm
    if (const char* why = reset_blocked_by()) { log_line("X2-SR-003: soft reset chord ignored (%s)", why); return; }
    if (*g_battle_state == 1) {
        *g_battle_flags &= ~0x200u;
        *g_pending_result = 1;
        g_battle_reset_ticks = 1;
        log_line("X2-SR-003: soft reset in battle -> defeat (loss-allowed bit cleared), game over -> title follows");
        return;
    }
    log_line("X2-SR-003: soft reset -> title (map 0x%X)", read_gva<uint32_t>(0x00D61748));
    *g_title_req = 1;
}

static bool install() {
    struct S { const char* label; const char* pat; uint32_t expect; uint8_t* at; } s[] = {
        { "X2-SR-003 game-over return to title FUN_00728340",
          "E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 01 6A FF E8 ?? ?? ?? ?? 83 C4 08 E8 ?? ?? ?? ?? 85 C0 78 0E", 0x728340 },
        { "X2-SR-003 Sg_MainLoop title request",
          "83 3D ?? ?? ?? ?? 01 D9 55 F4 D9 55 F0 D9 5D EC 75 0F E8 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 02 00 00 00", 0x605170 },
        { "X2-SR-003 battle result loss test",
          "8B 0D ?? ?? ?? ?? 53 8B 1D ?? ?? ?? ?? C7 05 ?? ?? ?? ?? ?? ?? ?? ?? 89 0D ?? ?? ?? ?? 83 F8 01 75 12 F7 C1 00 02 00 00 75 0A E8",
          0x71B6FC },
        { "X2-SR-003 pad held FUN_00745d00",
          "55 8B EC 8B 45 08 03 45 0C 8D 0C 40 C1 E1 06 81 C1 ?? ?? ?? ?? 0F B6 81 9C 00 00 00 03 45 10 83 E0 03 C1 E0 05 66 8B 44 08 04 5D C3",
          0x745D00 },
        { "X2-SR-003 party check FUN_0061ef70",
          "55 8B EC 51 80 3D ?? ?? ?? ?? 00 53 0F B6 1D ?? ?? ?? ?? 89 5D FC 0F 85", 0x61EF70 },
        { "X2-SR-003 battle teardown test",
          "80 3D ?? ?? ?? ?? 26 74 52 A0 ?? ?? ?? ?? 3C 05 74 49 3C 06 0F 85", 0x61E5E2 },
    };
    bool same_build = true;
    for (auto& x : s) {
        x.at = sig_find(x.label, x.pat, x.expect);
        if (!x.at) return false;
        if (rt_to_gva(x.at) != x.expect) same_build = false;
    }
    if (!same_build) {
        log_line("  X2-SR-003: a signature moved (other build); the fixed menu/pause/map addresses would be stale - refused");
        return false;
    }
    uint8_t* fn = s[0].at;
    uint8_t* req_site = s[1].at;
    // the title request calls the thunk 0x72DBD0 -> FUN_00728340 (E8 at +0x12) and then writes 2 to the same flag
    uint8_t* thunk = call_target(req_site + 0x12);
    if (!thunk || thunk[0] != 0xE9 || thunk + 5 + *(int32_t*)(thunk + 1) != fn ||        // 0x72DBD0: JMP FUN_00728340
        *(uint32_t*)(req_site + 2) != *(uint32_t*)(req_site + 0x19)) {
        log_line("  X2-SR-003: title request at gva=0x%08X does not reach FUN_00728340 - refused", rt_to_gva(req_site));
        return false;
    }
    if (call_target(s[2].at + 0x2A) != fn) {
        log_line("  X2-SR-003: battle result handler does not call FUN_00728340 at +0x2A - refused");
        return false;
    }
    g_title_req = (volatile int32_t*)abs_operand("title request", req_site + 2);
    g_battle_flags = (volatile uint32_t*)abs_operand("battle call flags", s[2].at + 2);
    g_pad       = abs_operand("pad block", s[3].at + 0x11);
    g_pending_result    = (volatile int8_t*)abs_operand("pending battle result", s[4].at + 0xF);
    g_battle_state    = (volatile int8_t*)abs_operand("battle state", s[5].at + 2);
    g_battle_result   = (volatile int8_t*)abs_operand("battle result", s[5].at + 0xA);
    if (!g_title_req || !g_battle_flags || !g_pad || !g_pending_result || !g_battle_state || !g_battle_result) return false;
    config_str("x2", "soft_reset", g_chord_text, sizeof(g_chord_text), "L1+L2+R1+R2+START+SELECT");
    uint16_t m = parse_chord(g_chord_text);
    if (!m || (m & (m - 1)) == 0) {
        log_line("  X2-SR-003: soft_reset '%s' is invalid or a single button; using L1+L2+R1+R2+START+SELECT", g_chord_text);
        m = 0x090F;
    }
    g_chord = m;
    log_line("  X2-SR-003: reset gva=0x%08X; request 0x%08X, loss flags 0x%08X, result 0x%08X, state 0x%08X/0x%08X, pad 0x%08X; "
             "chord '%s' (mask 0x%04X), field and battle", rt_to_gva(fn), rt_to_gva((const void*)g_title_req),
             rt_to_gva((const void*)g_battle_flags), rt_to_gva((const void*)g_pending_result), rt_to_gva((const void*)g_battle_state),
             rt_to_gva((const void*)g_battle_result), rt_to_gva(g_pad), g_chord_text, g_chord);
    return true;
}

Row row_SR003 = {
    "X2-SR-003",
    "soft reset: hold the [x2] soft_reset chord (default L1+L2+R1+R2+Start+Select) to return to the title (game's title request; in battle via a forced defeat)",
    1, install, nullptr, post, 0
};

} // namespace x2
