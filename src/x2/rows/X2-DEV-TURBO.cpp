// SPDX-License-Identifier: Apache-2.0
// X2-DEV-TURBO (test tool, port of FFX DEV-TURBO core + SOUND): while a key is held, the game runs X2-DEV-TURBO-SPEED
// times faster through its own booster path. Sg_MainLoop scales its delta by the booster factor (FUN_0051c1b0, called at
// 0x60514A: delta * [0xD42280 + idx*4] when the index [0xDEAF74] != 0, plain RETs 0x51C221 / 0x51C25F); the larger
// delta becomes a backlog count, which the runner executes with skip-render invocations. Frameskip is allowed only while
// the booster is on (FUN_006044b0 -> FUN_0051c1a0 `XOR EAX,EAX; CMP [idx],EAX; SETNZ AL; RET`, its only caller). The row
// forces both while the key is held (outer repo re/MAINLOOP.md §4-§5, S). Pacing sees the extra invocations as backlog.
// X2-DEV-TURBO-SOUND (default 1): the booster switches the sound into its turbo mode on engage / release by calling
// FUN_006066f0(1 / 0) (cdecl(int), state [0xDF6404], plain RET 0x60673A; same bytes as FFX FUN_00821c60, S); the key
// path calls it on the key edges, a release is skipped while the game's own booster is engaged ([0xDEAF78]).
// Not ported: FFX DEV-TURBO-VOICE (voice-busy query, SyncData base shift); voiced cutscenes may still wait at 1x (H).
// ini [rows]: X2-DEV-TURBO=1, X2-DEV-TURBO-KEY=<virtual-key code, decimal; 96 = numpad 0>, X2-DEV-TURBO-SPEED=<2..30>.
#include "../x2.h"

namespace x2 {

extern Row row_DEVTURBO;

typedef float(__cdecl* scale_t)(float delta);
typedef int(__cdecl* active_t)(void);
typedef void(__cdecl* sound_t)(int on);

static uint8_t* g_scale_fn = nullptr;     // FUN_0051c1b0
static uint8_t* g_active_fn = nullptr;    // FUN_0051c1a0
static scale_t  g_scale_tramp = nullptr;
static active_t g_active_tramp = nullptr;
static sound_t  g_sound_fn = nullptr;     // FUN_006066f0
static volatile int32_t* g_engaged = nullptr;   // [0xDEAF78] the game's booster is engaged
static bool     g_patched = false;
static int      g_key = 0x60, g_speed = 10;
static volatile bool g_held = false;

// cdecl(float delta) -> float in ST0. The original still runs (its own booster state and events).
static float __cdecl h_scale(float delta) {
    float r = g_scale_tramp(delta);
    return g_held && row_DEVTURBO.on ? delta * (float)g_speed : r;
}

static int __cdecl h_active() {
    return g_held && row_DEVTURBO.on ? 1 : g_active_tramp();
}

bool devturbo_held() { return g_held && row_DEVTURBO.on; }

static bool install() {
    g_key = config_row("X2-DEV-TURBO-KEY", 0x60);
    g_speed = config_row("X2-DEV-TURBO-SPEED", 10);
    if (g_speed < 2) g_speed = 2;
    if (g_speed > 30) g_speed = 30;
    // PUSH EBP; MOV EBP,ESP; CMP byte [enabled],0; JZ far; CALL; MOV [b],AL; TEST AL,AL; JZ; MOV EAX,[idx]; TEST EAX,EAX;
    // JZ; CMP [engaged],0; FLD [EAX*4+table]; FMUL [EBP+8]; FSTP [EBP+8]
    g_scale_fn = sig_find("X2-DEV-TURBO booster delta scale FUN_0051c1b0",
        "55 8B EC 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? A2 ?? ?? ?? ?? 84 C0 74 ?? A1 ?? ?? ?? ?? 85 C0 "
        "74 ?? 83 3D ?? ?? ?? ?? 00 D9 04 85 ?? ?? ?? ?? D8 4D 08 D9 5D 08", 0x51C1B0);
    if (!g_scale_fn) return false;
    uint8_t* idx = abs_operand("booster index", g_scale_fn + 0x1F);
    g_engaged = (volatile int32_t*)abs_operand("booster engaged", g_scale_fn + 0x29);
    if (!idx || !g_engaged || !sig_match(g_scale_fn + 0x6D, "D9 45 08 5D C3") || !sig_match(g_scale_fn + 0xAB, "D9 45 08 5D C3")) {
        log_line("  X2-DEV-TURBO: FUN_0051c1b0 body not as proven - off");
        return false;
    }
    // FUN_0051c1a0 sits right before it: XOR EAX,EAX; CMP [idx],EAX; SETNZ AL; RET (same index global)
    g_active_fn = g_scale_fn - 0x10;
    if (!sig_match(g_active_fn, "33 C0 39 05 ?? ?? ?? ?? 0F 95 C0 C3") || abs_operand("booster index", g_active_fn + 4) != idx) {
        log_line("  X2-DEV-TURBO: FUN_0051c1a0 not at scale - 0x10 - off");
        return false;
    }
    if (config_row("X2-DEV-TURBO-SOUND", 1)) {
        // the engage call at +0x5B: PUSH 1; CALL FUN_006066f0
        uint8_t* s = sig_match(g_scale_fn + 0x59, "6A 01 E8") ? call_target(g_scale_fn + 0x5B) : nullptr;
        if (s && sig_match(s, "55 8B EC A1 ?? ?? ?? ?? 56 85 C0 75 13 8B 75 08 85 F6 74 34 C7 05 ?? ?? ?? ?? 01 00 00 00 EB 12 83 F8 01 75 23"))
            g_sound_fn = (sound_t)s;
        else
            log_line("  X2-DEV-TURBO-SOUND: booster sound switch not as proven - off");
    }
    log_line("  X2-DEV-TURBO: scale gva=0x%08X (0x0051C1B0), active gva=0x%08X (0x0051C1A0), index gva=0x%08X (0x00DEAF74), "
             "engaged gva=0x%08X (0x00DEAF78), sound gva=0x%08X (0x006066F0); hold VK 0x%02X for x%d",
             rt_to_gva(g_scale_fn), rt_to_gva(g_active_fn), rt_to_gva(idx), rt_to_gva((const void*)g_engaged),
             g_sound_fn ? rt_to_gva((const void*)g_sound_fn) : 0, g_key, g_speed);
    return true;
}

static void pre() {
    if (!g_patched) {
        g_patched = true;
        if (!hook_entry("X2-DEV-TURBO scale", g_scale_fn, (void*)&h_scale, (void**)&g_scale_tramp)) {
            log_line("X2-DEV-TURBO: hook failed - row off");
            row_DEVTURBO.on = 0;
            return;
        }
        if (!hook_entry("X2-DEV-TURBO active", g_active_fn, (void*)&h_active, (void**)&g_active_tramp)) {
            hook_remove(g_scale_fn);
            log_line("X2-DEV-TURBO: hook failed - row off");
            row_DEVTURBO.on = 0;
            return;
        }
        log_line("X2-DEV-TURBO: hooked on the game thread");
    }
    bool down = (GetAsyncKeyState(g_key) & 0x8000) != 0;
    if (down) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        down = pid == GetCurrentProcessId();
    }
    if (down != g_held) {
        g_held = down;
        log_line("X2-DEV-TURBO: %s (x%d)", down ? "on" : "off", g_speed);
        if (g_sound_fn && (down || *g_engaged == 0)) {
            __try { g_sound_fn(down ? 1 : 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_sound_fn = nullptr; log_line("X2-DEV-TURBO-SOUND: sound switch faulted - off"); }
        }
    }
}

Row row_DEVTURBO = {
    "X2-DEV-TURBO",
    "test tool: hold a key (X2-DEV-TURBO-KEY, default numpad 0) to run the game X2-DEV-TURBO-SPEED times faster via its own booster path",
    1, install, pre, nullptr, 0
};

} // namespace x2
