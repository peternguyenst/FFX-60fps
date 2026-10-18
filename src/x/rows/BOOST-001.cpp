// SPDX-License-Identifier: Apache-2.0
// BOOST-001 (default on): the game's speed booster (2x / 4x) at 60 Hz. The booster does not run extra ticks itself:
// FUN_006f7340 0x6F7340 (first call in Sg_MainLoop) multiplies the frame delta by table {1,2,4}[DAT_00ce82b4] while
// it is engaged (DAT_00ce82b8 = 1), and Sg_WaitFrameSync turns delta into a backlog count in 30 Hz ticks
// (ftol(delta * 30), only when >= 2 vblanks; re/ledger/CLK.md CLK-009, re/MAINLOOP.md section 5). At stock 30 Hz that
// gives 2 / 4 ticks per frame. Under the mod's pacing delta is 1/(N * 29.97) s, too short to produce those counts.
// While the booster is engaged this row shortens the logic period instead (pacing_force_div, kind INV_BACKLOG): a
// factor of f asks for a logic tick every N/f invocations, which is f ticks per stock tick at any frame rate. At
// 60 fps that is every invocation for 2x (60 ticks/s) and for 4x (the game's own backlog tick carries the rest,
// 120/s), as before. Read once per invocation; the flag is the previous invocation's (one frame late at the edges).
#include "../fps60.h"

static volatile int32_t* g_index = nullptr;   // DAT_00ce82b4: 0 off, 1 = 2x, 2 = 4x
static volatile int32_t* g_engaged = nullptr; // DAT_00ce82b8: set by FUN_006f7340 when the scale applied
static volatile float*   g_table = nullptr;   // DAT_00c38e1c {1,2,4}
static bool g_on = false;
static int  g_every = 0;                      // invocations per logic tick while engaged

// The booster's factor f means f logic ticks per stock 30 Hz tick, so a logic tick every N / f invocations
// instead of every N: at 60 fps that is every invocation for both 2x and 4x (the game's own backlog tick adds
// the rest of 4x, as before); at 240 fps it is every 4th for 2x and every 2nd for 4x.
static int every_for(int32_t idx) {
    int f = (int)g_table[idx];
    int n = rate_mult();
    int e = f > 0 ? n / f : n;
    return e < 1 ? 1 : e;
}

static void pre() {
    int32_t idx = *g_index;
    bool on = !g_disabled && *g_engaged != 0 && idx > 0 && idx < 3;
    int every = on ? every_for(idx) : 0;
    if (on != g_on || every != g_every) {
        g_on = on;
        g_every = every;
        pacing_force_div(every);
        if (on) log_line("BOOST-001: booster %.0fx engaged - a logic tick every %d invocation%s", g_table[idx],
                         every, every == 1 ? "" : "s");
        else    log_line("BOOST-001: booster off - 1 logic tick in %d invocations", rate_mult());
    }
}

static bool install() {
    // FUN_006f7340: PUSH EBP; MOV EBP,ESP; CMP byte [enabled],0; JZ far; PUSH EBX; CALL; CMP EAX,1; SETZ BH; CALL
    uint8_t* f = sig_find_unique("BOOST-001 booster delta scale FUN_006f7340",
        "55 8B EC 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 53 E8 ?? ?? ?? ?? 83 F8 01 0F 94 C7 E8", 0x6F7340);
    if (!f) return false;
    // 0x6F73CD MOV EAX,[index]; TEST EAX,EAX; JZ; CMP dword [engaged],0; FLD float [EAX*4+table]
    if (!sig_match(f + 0x8D, "A1 ?? ?? ?? ?? 85 C0 74 ?? 83 3D ?? ?? ?? ?? 00 D9 04 85")) {
        log_line("  BOOST-001: FUN_006f7340 body has an unexpected shape - refused");
        return false;
    }
    g_index = (volatile int32_t*)read_abs(f + 0x8E);
    g_engaged = (volatile int32_t*)read_abs(f + 0x98);
    g_table = (volatile float*)read_abs(f + 0xA0);
    if (!g_index || !g_engaged || !g_table) return false;
    log_line("  BOOST-001: index gva=0x%08X engaged gva=0x%08X table gva=0x%08X {%.0f,%.0f,%.0f}",
             rt_to_gva_exe((void*)g_index), rt_to_gva_exe((void*)g_engaged), rt_to_gva_exe((void*)g_table),
             g_table[0], g_table[1], g_table[2]);
    return true;
}

static void stop() {
    if (g_on) pacing_force_div(0);
    g_on = false;
    g_every = 0;
}

Row row_BOOST001 = {
    "BOOST-001",
    "game speed booster 2x/4x at the stock rates under the mod's pacing (shorter logic period while engaged)",
    1, install, pre, nullptr, 0, stop
};
