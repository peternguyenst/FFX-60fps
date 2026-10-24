// SPDX-License-Identifier: Apache-2.0
// X2-CHR-002 (FFX-2 only, stock look fix): battle idles are play-once motions (loops 1) that the battle script restarts
// whenever they end. While the script is busy (another actor's long attack) the idle ends and the actor holds its last
// frame until the script runs again: Seymour stands frozen for ~10 s during Tidus's Blitz Ace. U 18/10/2026: also with
// the mod off ("the game original have that issue too ... it look weird"). D 18/10 (X2-SMOOTH-003-FREEZEDIAG trace, id
// 0x113F): motion seq 0xF09CAD1 start 0 end 0x31FF len 0x3200 loops 1 reaches clock 0x3180 at 00:44:02.13 and holds
// until 00:44:11.91; between turns the game restarts the same motion from its end every 1-2 s (00:44:34.6 -> 35.6:
// same seq, clock 0x3180 -> 0x80).
// Fix: per actor slot, a motion the game itself restarted from its last frame (same seq, clock back near start) is
// remembered as a game-looped motion. When such a motion then sits on its last frame for X2-CHR-002-HOLD logic ticks
// (default 3), its clock is put back to its start, as the script's restart does. Other play-once motions (hits, deaths,
// poses) are never touched, nor script-driven actors (+0x1A0 bit 0x40). Actor fields as X2-SMOOTH-003 (PORT-SMOOTH3.md
// §5, S): clock +0x750, seq +0x724, start +0x728, end +0x72C, loops +0x734 (s16). That a clock write restarts the motion
// cleanly is H (the sampler Ch_DecodeKeys reads clock and end each step, S).
#include "../x2.h"
#include <string.h>

namespace x2 {

extern Row row_CHR002;

enum { MAXA = 512, NSEQ = 4 };
static const uint32_t STRIDE = 0x910, ACTIVE = 0x2, FLAGS = 0x1A0, F_SCRIPT = 0x40;
static const uint32_t M_SEQ = 0x724, M_START = 0x728, M_END = 0x72C, M_LOOPS = 0x734, M_CLOCK = 0x750;

static uint8_t* volatile* g_table = nullptr;
static volatile int32_t*  g_count = nullptr;
static int g_hold_ticks = 3;

struct Slot { uint16_t id; uint32_t seq; int32_t clock; int held; uint32_t looped[NSEQ]; uint8_t nlooped; };
static Slot g_s[MAXA];
static LONG g_wraps = 0, g_learned = 0;

static bool at_end(int32_t clock, int32_t end) { return clock + 0x100 > end; }

static bool is_looped(const Slot& s, uint32_t seq) {
    for (int k = 0; k < s.nlooped; k++) if (s.looped[k] == seq) return true;
    return false;
}

static void scan() {
    uint8_t* base = *g_table;
    int n = *g_count;
    if (!base || n <= 0) return;
    if (n > MAXA) n = MAXA;
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * STRIDE;
        Slot& s = g_s[i];
        uint16_t id = *(uint16_t*)a;
        if (a[ACTIVE] == 0 || s.id != id) { memset(&s, 0, sizeof(s)); s.id = a[ACTIVE] ? id : 0; continue; }
        uint32_t seq = *(uint32_t*)(a + M_SEQ);
        int32_t clock = *(int32_t*)(a + M_CLOCK), start = *(int32_t*)(a + M_START), end = *(int32_t*)(a + M_END);
        bool once = *(int16_t*)(a + M_LOOPS) == 1 && seq != 0 && (*(uint32_t*)(a + FLAGS) & F_SCRIPT) == 0 && end > start;
        if (once && seq == s.seq && at_end(s.clock, end) && clock < start + 0x400 && clock < s.clock && !is_looped(s, seq)) {
            // the game restarted this motion from its last frame: it is used as a loop
            if (s.nlooped < NSEQ) s.looped[s.nlooped++] = seq;
            else { memmove(s.looped, s.looped + 1, sizeof(s.looped) - sizeof(uint32_t)); s.looped[NSEQ - 1] = seq; }
            if (InterlockedIncrement(&g_learned) <= 20)
                log_line("X2-CHR-002: slot %d id 0x%04X: motion 0x%08X (end 0x%X) restarted by the game - treated as a loop", i, id, seq, end);
        }
        if (once && seq == s.seq && clock == s.clock && at_end(clock, end) && is_looped(s, seq)) {
            if (++s.held >= g_hold_ticks) {
                *(int32_t*)(a + M_CLOCK) = start;
                clock = start;
                s.held = 0;
                LONG c = InterlockedIncrement(&g_wraps);
                if (c <= 20 || c % 100 == 0)
                    log_line("X2-CHR-002: slot %d id 0x%04X: motion 0x%08X held on its last frame - restarted (%ld so far)", i, id, seq, c);
            }
        } else s.held = 0;
        s.seq = seq;
        s.clock = clock;
    }
}

static void pre() {
    int k = pacing_kind();
    if (k == INV_RENDER_ONLY) return;                     // logic ticks only (render-only steps are SMOOTH-003's half steps)
    __try { scan(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { row_CHR002.on = 0; log_line("X2-CHR-002: fault - row off"); }
}

static bool install() {
    uint8_t* tb = sig_find("X2-CHR-002 actor table", "8B 0D ?? ?? ?? ?? 8B 35 ?? ?? ?? ?? 69 C9 10 09 00 00 03 CE", 0x6DBAA5);
    if (!tb) return false;
    g_count = (volatile int32_t*)abs_operand("actor count", tb + 2);
    g_table = (uint8_t* volatile*)abs_operand("actor table", tb + 8);
    if (!g_count || !g_table) return false;
    g_hold_ticks = config_row("X2-CHR-002-HOLD", 3);
    if (g_hold_ticks < 1) g_hold_ticks = 1;
    log_line("  X2-CHR-002: actors gva=0x%08X (0x01CC9604) count gva=0x%08X (0x01CC9600); restart a game-looped motion held %d ticks",
             rt_to_gva((const void*)g_table), rt_to_gva((const void*)g_count), g_hold_ticks);
    return true;
}

Row row_CHR002 = {
    "X2-CHR-002",
    "battle idles that end while the script is busy (Seymour frozen during Blitz Ace) loop instead of holding their last frame",
    1, install, pre, nullptr, 0
};

} // namespace x2
