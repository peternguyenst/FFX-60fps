// SPDX-License-Identifier: Apache-2.0
// X2-DEV-TURBO (test tool): while a key is held, the game runs X2-DEV-TURBO-SPEED times faster.
//
// It uses the game's own booster path: the main loop scales its time step by the booster factor, the bigger step
// becomes extra logic invocations without drawing (backlog), and frame skipping is allowed only while the booster is
// on. The row forces both while the key is held; pacing sees the extra invocations as backlog. See re/MAINLOOP.md §4-§5.
// X2-DEV-TURBO-SOUND (on): the sound is switched into the booster's turbo mode on press and back on release (unless
// the game's own booster is on).
// X2-DEV-TURBO-VOICE (on): voiced scenes wait in real time, which the faster time step cannot shorten:
//   - the voice-busy query (FUN_00495dc0, sound command 0x11 = the playing voice's two state flags) answers "not
//     busy" while the key is held, so scripts waiting for the line to end go on;
//   - in SyncData scenes Sg_WaitFrameSync ignores the time step and follows a wall-clock timeline; while the key is
//     held its start time is moved back by the extra time, so the game's own catch-up runs the scene faster (capped
//     below the 30 timeline entries one frame can hold).
// Evidence: re/ledger/TURBO-X2.md (voice section).
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

// X2-DEV-TURBO-VOICE
typedef uint32_t(__cdecl* voice_busy_t)(void);
typedef void(__cdecl* wait_sync_t)(float delta);
static const float MAX_SYNC_SHIFT_S = 0.25f;      // keeps one frame's catch-up below the 30-entry timeline table
static uint8_t*      g_voice_busy_fn = nullptr;   // FUN_00495dc0
static voice_busy_t  g_voice_busy_tramp = nullptr;
static uint8_t*      g_wait_sync_fn = nullptr;    // Sg_WaitFrameSync FUN_00606900
static wait_sync_t   g_wait_sync_tramp = nullptr;
static volatile int32_t*  g_sync_mode = nullptr;   // [0xDF6508] 1 = SyncData timeline running
static volatile int32_t*  g_sync_unset = nullptr;  // [0xD47F4C] 1 = timeline start not taken yet
static volatile uint64_t* g_sync_start = nullptr;  // [0xDF6480] timeline start (us)
static volatile LONG g_voice_idle_answers = 0, g_sync_shifts = 0;

// cdecl(float delta) -> float in ST0. The original still runs (its own booster state and events).
static float __cdecl h_scale(float delta) {
    float r = g_scale_tramp(delta);
    return g_held && row_DEVTURBO.on ? delta * (float)g_speed : r;
}

static int __cdecl h_active() {
    return g_held && row_DEVTURBO.on ? 1 : g_active_tramp();
}

bool devturbo_held() { return g_held && row_DEVTURBO.on; }

static uint32_t __cdecl h_voice_busy() {
    if (devturbo_held()) {
        if (InterlockedIncrement(&g_voice_idle_answers) == 1)
            log_line("X2-DEV-TURBO-VOICE: first voice-busy query answered idle under turbo");
        return 0;
    }
    return g_voice_busy_tramp();
}

// delta is already multiplied by the turbo speed (Sg_TurboDeltaScale, hooked above)
static void __cdecl h_wait_sync(float delta) {
    if (devturbo_held() && *g_backlog_count == 0 && *g_sync_mode == 1 && *g_sync_unset != 1 && delta > 0) {
        float extra = delta * (1.0f - 1.0f / (float)g_speed);
        if (extra > MAX_SYNC_SHIFT_S) extra = MAX_SYNC_SHIFT_S;
        *g_sync_start -= (uint64_t)(extra * 1e6f);
        if (InterlockedIncrement(&g_sync_shifts) == 1)
            log_line("X2-DEV-TURBO-VOICE: first SyncData timeline shift (%.0f ms)", extra * 1000.0f);
    }
    g_wait_sync_tramp(delta);
}

// Finds both voice hooks; either can stay off on its own.
static void install_voice() {
    // MOV ECX,[sound manager]; PUSH 0x11; CALL command; RET
    g_voice_busy_fn = sig_find("X2-DEV-TURBO-VOICE voice-busy query FUN_00495dc0",
        "8B 0D ?? ?? ?? ?? 6A 11 E8 ?? ?? ?? ?? C3", 0x495DC0);
    // PUSH EBP; MOV EBP,ESP; SUB ESP,0x10; FLD [carry]; FLD ST0; FLD [EBP+8]; FLD ST0; FADDP ST2
    g_wait_sync_fn = sig_find("X2-DEV-TURBO-VOICE Sg_WaitFrameSync",
        "55 8B EC 83 EC 10 D9 05 ?? ?? ?? ?? D9 C0 D9 45 08 D9 C0 DE C2 DD 05", 0x606900);
    if (g_wait_sync_fn) {
        uint8_t* f = g_wait_sync_fn;
        // +0x4B CMP [backlog],0; JNZ / +0x67 CMP [mode],1; JNZ / +0x74 CMP [unset],1; PUSH ESI; MOV / +0x99 MOV [start],EAX
        bool shape = sig_match(f + 0x4B, "83 3D ?? ?? ?? ?? 00 0F 85") && sig_match(f + 0x67, "83 3D ?? ?? ?? ?? 01 0F 85") &&
                     sig_match(f + 0x74, "83 3D ?? ?? ?? ?? 01 56 C7 05") && sig_match(f + 0x99, "A3 ?? ?? ?? ?? 89 15");
        if (shape && abs_operand("backlog", f + 0x4D) == (uint8_t*)g_backlog_count) {
            g_sync_mode = (volatile int32_t*)abs_operand("SyncData mode", f + 0x69);
            g_sync_unset = (volatile int32_t*)abs_operand("SyncData start unset", f + 0x76);
            g_sync_start = (volatile uint64_t*)abs_operand("SyncData start", f + 0x9A);
        }
        if (!g_sync_mode || !g_sync_unset || !g_sync_start) {
            g_wait_sync_fn = nullptr;
            log_line("  X2-DEV-TURBO-VOICE: Sg_WaitFrameSync not as proven - SyncData shift off");
        }
    }
    log_line("  X2-DEV-TURBO-VOICE: voice-busy gva=0x%08X (0x00495DC0), Sg_WaitFrameSync gva=0x%08X (0x00606900), "
             "SyncData mode/unset/start gva=0x%08X/0x%08X/0x%08X (0x00DF6508/0x00D47F4C/0x00DF6480)",
             g_voice_busy_fn ? rt_to_gva(g_voice_busy_fn) : 0, g_wait_sync_fn ? rt_to_gva(g_wait_sync_fn) : 0,
             g_sync_mode ? rt_to_gva((const void*)g_sync_mode) : 0, g_sync_unset ? rt_to_gva((const void*)g_sync_unset) : 0,
             g_sync_start ? rt_to_gva((const void*)g_sync_start) : 0);
}

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
    if (config_row("X2-DEV-TURBO-VOICE", 1)) install_voice();
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
        if (g_voice_busy_fn && !hook_entry("X2-DEV-TURBO-VOICE voice", g_voice_busy_fn, (void*)&h_voice_busy,
                                           (void**)&g_voice_busy_tramp))
            g_voice_busy_fn = nullptr;
        if (g_wait_sync_fn && !hook_entry("X2-DEV-TURBO-VOICE SyncData", g_wait_sync_fn, (void*)&h_wait_sync,
                                          (void**)&g_wait_sync_tramp))
            g_wait_sync_fn = nullptr;
        log_line("X2-DEV-TURBO: hooked on the game thread (voice %d, SyncData %d)", g_voice_busy_fn != nullptr,
                 g_wait_sync_fn != nullptr);
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
