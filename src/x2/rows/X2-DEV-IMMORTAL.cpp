// SPDX-License-Identifier: Apache-2.0
// X2-DEV-IMMORTAL (test tool): a key makes the party take no HP or MP damage, for long test fights.
//
// Numpad 7 by default toggles the game's own leftover debug flag "party takes no damage" (gva 0xDF68B8): the game's
// damage functions return early while it is set. Other code and battle scripts also write the flag, so while on, the
// row writes 1 before every invocation; turning it off puts back the value it had when it was turned on. Statuses that
// are not damage (petrify, instant KO) are not covered (H). Evidence: re/ledger/DEV-IMMORTAL.md.
// ini [rows]: X2-DEV-IMMORTAL=1, X2-DEV-IMMORTAL-KEY=<virtual-key code, decimal; 103 = numpad 7>,
// X2-DEV-IMMORTAL-START=1 (start immortal).
#include "../x2.h"

namespace x2 {

extern Row row_DEVIMMORTAL;

static volatile uint8_t* g_flag = nullptr;
static int g_key = 0x67;
static bool g_on = false, g_held = false;
static uint8_t g_saved = 0;

// FUN_0061b750 side checks: CALL side; ADD ESP,4; CMP EAX,1; JNZ; CMP byte [enemy flag],0; JNZ; PUSH [EBP+8];
// CALL side; ADD ESP,4; TEST EAX,EAX; JNZ; CMP byte [party flag],AL; JNZ  (party flag disp32 at +0x27)
static const char* const SIG_FLAG =
    "E8 ?? ?? ?? ?? 83 C4 04 83 F8 01 75 09 80 3D ?? ?? ?? ?? 00 75 ?? FF 75 08 E8 ?? ?? ?? ?? 83 C4 04 85 C0 75 08 "
    "38 05 ?? ?? ?? ?? 75";

static void set_on(bool on) {
    if (on == g_on) return;
    __try {
        if (on) { g_saved = *g_flag; *g_flag = 1; }
        else *g_flag = g_saved;
    } __except (EXCEPTION_EXECUTE_HANDLER) { log_line("X2-DEV-IMMORTAL: flag write faulted"); return; }
    g_on = on;
    log_line("X2-DEV-IMMORTAL: party %s", on ? "takes no HP/MP damage" : "takes damage again");
}

static void pre() {
    bool down = (GetAsyncKeyState(g_key) & 0x8000) != 0;
    if (down) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        down = pid == GetCurrentProcessId();
    }
    if (down && !g_held) set_on(!g_on);
    g_held = down;
    if (g_on) {
        __try { *g_flag = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

static bool install() {
    g_key = config_row("X2-DEV-IMMORTAL-KEY", 0x67);
    uint8_t* at = sig_find("X2-DEV-IMMORTAL party flag check in FUN_0061b750", SIG_FLAG, 0x0061B7C5);
    if (!at) return false;
    g_flag = abs_operand("immortal flag", at + 0x27);   // relocated disp32 = runtime address
    if (!g_flag || rt_to_gva((const void*)g_flag) != 0x00DF68B8) {
        log_line("  X2-DEV-IMMORTAL: party flag operand is not 0x00DF68B8 - row off");
        return false;
    }
    log_line("  X2-DEV-IMMORTAL: party flag gva 0x00DF68B8 (%p); key 0x%X toggles", g_flag, g_key);
    if (config_row("X2-DEV-IMMORTAL-START", 0)) set_on(true);
    return true;
}

Row row_DEVIMMORTAL = {
    "X2-DEV-IMMORTAL",
    "test tool: a key (X2-DEV-IMMORTAL-KEY, default numpad 7) toggles the game's debug flag: party takes no HP/MP damage",
    1, install, pre, nullptr, 0
};

} // namespace x2
