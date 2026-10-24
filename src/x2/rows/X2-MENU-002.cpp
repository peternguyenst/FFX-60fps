// SPDX-License-Identifier: Apache-2.0
// X2-MENU-002: the field main menu at 60 fps without running 2x. X2-MENU-001 holds stock pacing (30 fps) in every
// PS2-layer menu; for the field main menu (mode 0 / 0x4000000) this row keeps the 60 Hz host and halves the menu
// logic instead: Menu_CoroutineIterate 0x7645E0 alternates logic and render-only passes.
//   render-only pass: the step gate CALL 0x51C100 (0x764824) returns 0x7FFFFFFF so the step block is skipped; the
//     always-run task step CALL Menu_TasksStep(1) (0x764894) is skipped; the cursor counter INC [0x1CCE9B8]
//     (0x76473A, before the gate) is undone at the gate, and so is INC byte [0x16BC586] (0x764734; X2-MENU-002-PARITY:
//     the Items->Use item/target cursors blink and animate from it, U 16/10/2026 "blink twice as fast", ITEM-CURSOR.md); the fade ramp [0x16BDCE8] (0x7649BB, after the draws that read
//     it) is put back after the pass.
// Logic / render-only = the pacing core's alternation (render_only()), so X2-UI-017's pad latch lands on logic passes.
//   logic pass: everything runs; the slides are 16-byte tween records stepped by Tween_Step 0x772EB0 (value +0xE,
//     phase +0xA), read by the draws through Tween_GetValue 0x773010 / Tween_GetPhase 0x772FF0. Each step is recorded
//     (before/after); in the draw part of the logic pass the getters return the midpoint, so the drawn sequence is
//     k-0.5 (logic), k (render-only), k+0.5 ... A record whose value changed after the step (retarget) is not blended.
// U 14/10/2026: "main menu animation seems 30fps, can we make it 60fps without speeding it up" - windows sliding first.
// Evidence: outer repo re/ledger/MENU-SMOOTH.md, re/ledger/MENU.md §2/§5 (S).
#include "../x2.h"

namespace x2 {

typedef int(__cdecl* step_gate_fn)();
typedef void(__cdecl* tasks_step_fn)(uint32_t phase);
typedef int(__cdecl* tween_step_fn)(uint8_t* t);

static step_gate_fn       g_step_gate = nullptr;            // 0x51C100 turbo factor
static tasks_step_fn      g_tasks_step = nullptr;           // 0x764170 Menu_TasksStep
static tween_step_fn g_tween_step_orig = nullptr;      // trampoline of 0x772EB0
static volatile int32_t*  g_cursor_counter = nullptr;   // 0x01CCE9B8 cursor / blink counter
static volatile uint8_t*  g_parity_byte = nullptr;    // 0x016BC586 menu parity byte: item-use cursor clock (ITEM-CURSOR.md)
static int                g_opt_parity = 1;        // [rows] X2-MENU-002-PARITY
static volatile int32_t*  g_fade_ramp = nullptr;      // 0x016BDCE8 fade ramp value
static volatile int32_t*  g_menu_up = nullptr;        // 0x016BC55C menu in progress
static volatile int32_t*  g_menu_up2 = nullptr;       // 0x016BC560
static volatile int32_t*  g_menu_request = nullptr;       // 0x00DF6870 request, -1 = none
static volatile uint32_t* g_menu_mode = nullptr;      // 0x016BC5EC mode

static bool  g_claimed_now = false;     // this invocation: main menu claimed
static bool  g_render_only_pass = false, g_in_logic_draws = false, g_fade_saved_valid = false;
static int32_t g_fade_saved = 0;
static uint32_t g_pass_serial = 1;
static LONG  g_passes = 0, g_render_passes = 0, g_blended = 0, g_tween_table_full = 0;

struct TweenRecord { uint8_t* p; uint32_t serial; int16_t v0, v1, ph0, ph1; };
static TweenRecord g_tweens[256];
static int g_tween_count = 0;
extern Row row_MENU002;

// Field main menu = mode 0 (pad open, Sg_MenuRequestTest 0x604E67). Mode 0x4000000 is the BATTLE pause menu: requested
// by FUN_0061e440 (0x61E5DA) on Start while battle state [0xDF68A0] == 1. Claiming it ran the pause menu at 60 Hz with
// its logic halved and crashed (friend's log 16/10/2026 09:15, "crash when he pause the game in combat").
static bool main_menu_mode(uint32_t m) { return (m & 0xFFFF0000u) == 0; }
static bool g_battle_menu = false;   // a 0x4000000 request was seen; never claim until every menu is closed

// Parts, each with an ini key and a numpad hotkey (game window focused, logged). U 14/10/2026: "hotkey to turn on/off
// each of them mid game so i can choose what to keep".
//   Num 9  X2-MENU-002           the whole row (off = X2-MENU-001 stock pacing, 30 fps menu)
//   Num 1  X2-MENU-002-TWEEN     tween slides drawn half-way (Tween_GetValue / Tween_GetPhase)
//   Num 2  X2-MENU-002-DRAWREPLAY task draw-phase replay (panel shadows)
//   Num 3  X2-MENU-002-POINT     screen point [0x16BC550]/[0x16BC554] (eased by tweens 0x16BDA80/90 in FUN_00761f30,
//                                read directly by Menu_DrawModules 0x764E07 and FUN_00762a20)
//   Num 4  X2-MENU-002-BACKDROP  backdrop ramp [0x16BDC24..0x16BDC2E] (FUN_00761f80: position s16 x2, colour/alpha
//                                u8 x5, stepped +0x124/step; read directly by FUN_007622f0 / FUN_00760d60)
// POINT and BACKDROP: values read straight from memory. The logic pass writes the midpoint between the value before
// the step (at the gate) and after it (task step) for the draws, and puts the stepped value back after the pass
// (only if it still holds our midpoint).
static int g_opt_on = 1, g_opt_tween = 1, g_opt_point = 1, g_opt_backdrop = 1;
bool menu002_user_on() { return row_MENU002.on && g_opt_on; }
void menu002_set_user(bool on) {                  // X2-CFG-MENU: same switch as Num 9 (needs the row installed)
    if (on && !row_set(row_MENU002, true)) return;
    g_opt_on = on ? 1 : 0;
}
static int g_opt_drawreplay = 1;

struct BlendVar { volatile uint8_t* p; int size; bool sgn; int* toggle; int32_t pre, post, mid; bool wrote; };
static BlendVar g_blend_vars[9];
static int g_blend_var_count = 0;

static int32_t var_get(const BlendVar& v) {
    if (v.size == 4) return *(volatile int32_t*)v.p;
    if (v.size == 2) return v.sgn ? *(volatile int16_t*)v.p : *(volatile uint16_t*)v.p;
    return v.sgn ? *(volatile int8_t*)v.p : *(volatile uint8_t*)v.p;
}
static void var_set(const BlendVar& v, int32_t x) {
    if (v.size == 4) *(volatile int32_t*)v.p = x;
    else if (v.size == 2) *(volatile int16_t*)v.p = (int16_t)x;
    else *(volatile uint8_t*)v.p = (uint8_t)x;
}
static void add_blend_var(uint8_t* p, int size, bool sgn, int* toggle) {
    BlendVar& v = g_blend_vars[g_blend_var_count++];
    v.p = p; v.size = size; v.sgn = sgn; v.toggle = toggle; v.wrote = false;
}

struct Hotkey { int vk; int* flag; const char* name; bool prev; };
// Num 9 toggles the row in release builds only: dev builds use Num 9 for the hot reload (x2_reload.cpp, as in the FFX
// mod; user 24/10/2026); there the row is switched on the Config page.
static Hotkey g_hotkeys[] = {
#ifndef FPS60_DEV
    { VK_NUMPAD9, &g_opt_on,    "X2-MENU-002 (60 fps main menu)", false },
#endif
    { VK_NUMPAD1, &g_opt_tween,    "X2-MENU-002-TWEEN (slides)", false },
    { VK_NUMPAD2, &g_opt_drawreplay, "X2-MENU-002-DRAWREPLAY (shadows)", false },
    { VK_NUMPAD3, &g_opt_point,    "X2-MENU-002-POINT (screen point)", false },
    { VK_NUMPAD4, &g_opt_backdrop, "X2-MENU-002-BACKDROP (backdrop ramp)", false },
};

static void poll_hotkeys() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    bool focus = pid == GetCurrentProcessId();
    for (Hotkey& k : g_hotkeys) {
        bool down = (GetAsyncKeyState(k.vk) & 0x8000) != 0;
        if (down && !k.prev && focus) {
            *k.flag = !*k.flag;
            log_line("Num %d: %s %s", k.vk - VK_NUMPAD0, k.name, *k.flag ? "ON" : "OFF");
        }
        k.prev = down;
    }
}

// Called by X2-MENU-001 every invocation: true = this row runs the menu at 60 Hz (no stock hold).
bool menu002_claims() {
    if (!row_MENU002.on || !g_opt_on || g_disabled || !g_cfg.pacing || !g_menu_up) return false;
    int32_t req = *g_menu_request;
    if (*g_menu_up == 0 && *g_menu_up2 == 0 && req == -1) { g_battle_menu = false; return false; }   // no menu up or requested
    if (req != -1 && ((uint32_t)req & 0xFFFF0000u) == 0x04000000u) g_battle_menu = true;
    if (g_battle_menu) return false;                                // battle pause menu: X2-MENU-001 holds stock
    return req != -1 ? main_menu_mode((uint32_t)req) : main_menu_mode(*g_menu_mode);
}

static TweenRecord* find_tween_this_pass(const uint8_t* p) {
    for (int i = 0; i < g_tween_count; i++) if (g_tweens[i].p == p) return g_tweens[i].serial == g_pass_serial ? &g_tweens[i] : nullptr;
    return nullptr;
}

static int __cdecl hook_step_gate() {
    int n = g_step_gate();
    if (!g_claimed_now) return n;
    g_pass_serial++;
    g_tween_count = 0;
    InterlockedIncrement(&g_passes);
    // the pacing core's alternation: X2-UI-017 latches the pad on the same logic invocations, so no press is lost
    if (!render_only()) {
        for (int i = 0; i < g_blend_var_count; i++) { g_blend_vars[i].pre = var_get(g_blend_vars[i]); g_blend_vars[i].wrote = false; }
        return n;
    }
    g_render_only_pass = true;
    InterlockedIncrement(&g_render_passes);
    (*g_cursor_counter)--;                                   // undo this pass's INC (0x76473A ran before the gate)
    if (g_opt_parity) (*g_parity_byte)--;                    // and INC byte [0x16BC586] (0x764734): Items->Use cursors
    g_fade_saved = *g_fade_ramp;                           // restored after the pass (post)
    g_fade_saved_valid = true;
    return 0x7FFFFFFF;                                // CMP [0xDF6414],EAX; JL -> step block skipped
}

// X2-MENU-002-DRAWREPLAY: a task's step can arm a one-shot draw through its draw-phase byte +0x3E, e.g. the party
// panels' step sets 5 (0x773A2E) and the draw FUN_00773b60 paints the 14 panel shadows only at 5, then sets 7. With
// the step skipped, render-only passes lost the shadows (U 14/10/2026: "the shadow in the menu blink", 2 consecutive
// frames). The logic pass records +0x3E of every task after the step; a render-only pass puts it back before the
// draws, so both passes draw what the stock game draws every frame.
static uint8_t*  g_task_table = nullptr;              // 0x016BC5F0, stride 0x9C, 16 slots (Menu_TasksStep loop)
struct TaskDrawPhase { uint32_t step, draw; uint8_t ph; };
static TaskDrawPhase g_task_draw_phase[16];
static bool g_task_draw_phase_valid = false;
static LONG g_replays = 0, g_blend_var_mids = 0;
static const int TASK_SIZE = 0x9C, TASK_COUNT = 16;

static void __cdecl hook_tasks_step(uint32_t phase) {
    if (g_render_only_pass) {                              // the always-run task step: logic passes only
        if (!g_task_draw_phase_valid || !g_opt_drawreplay) return;
        for (int i = 0; i < TASK_COUNT; i++) {
            uint8_t* t = g_task_table + i * TASK_SIZE;
            if (*(uint32_t*)(t + 0xC) != g_task_draw_phase[i].step || *(uint32_t*)(t + 0x10) != g_task_draw_phase[i].draw) continue;
            if (t[0x3E] != g_task_draw_phase[i].ph) { t[0x3E] = g_task_draw_phase[i].ph; InterlockedIncrement(&g_replays); }
        }
        return;
    }
    g_tasks_step(phase);
    if (!g_claimed_now) return;
    g_in_logic_draws = true;                                   // from here on: the draw part of a logic pass
    for (int i = 0; i < TASK_COUNT; i++) {
        uint8_t* t = g_task_table + i * TASK_SIZE;
        g_task_draw_phase[i].step = *(uint32_t*)(t + 0xC);
        g_task_draw_phase[i].draw = *(uint32_t*)(t + 0x10);
        g_task_draw_phase[i].ph = t[0x3E];
    }
    g_task_draw_phase_valid = true;
    for (int i = 0; i < g_blend_var_count; i++) {                // POINT / BACKDROP: the draws see the midpoint
        BlendVar& v = g_blend_vars[i];
        if (!*v.toggle) continue;
        v.post = var_get(v);
        if (v.post == v.pre) continue;
        v.mid = (v.pre + v.post) / 2;
        var_set(v, v.mid);
        v.mid = var_get(v);                           // as stored (narrow types)
        v.wrote = true;
        InterlockedIncrement(&g_blend_var_mids);
    }
}

static int __cdecl hook_tween_step(uint8_t* t) {
    if (!g_claimed_now || g_render_only_pass) return g_tween_step_orig(t);
    int16_t v0 = *(int16_t*)(t + 0xE), ph0 = *(int16_t*)(t + 0xA);
    int r = g_tween_step_orig(t);
    TweenRecord* e = nullptr;
    for (int i = 0; i < g_tween_count; i++) if (g_tweens[i].p == t) { e = &g_tweens[i]; break; }
    if (!e) {
        if (g_tween_count >= (int)(sizeof(g_tweens) / sizeof(g_tweens[0]))) { InterlockedIncrement(&g_tween_table_full); return r; }
        e = &g_tweens[g_tween_count++];
        e->p = t;
        e->v0 = v0; e->ph0 = ph0;                     // first step this pass keeps the pre-step values
    }
    e->serial = g_pass_serial;
    e->v1 = *(int16_t*)(t + 0xE);
    e->ph1 = *(int16_t*)(t + 0xA);
    return r;
}

static int __cdecl hook_tween_get_value(uint8_t* t) {
    int v = *(int16_t*)(t + 0xE);
    if (!g_in_logic_draws || !g_opt_tween) return v;
    TweenRecord* e = find_tween_this_pass(t);
    if (!e || e->v1 != v || e->v0 == e->v1) return v;
    InterlockedIncrement(&g_blended);
    return ((int)e->v0 + (int)e->v1) / 2;
}

static int __cdecl hook_tween_get_phase(uint8_t* t) {
    int v = *(int16_t*)(t + 0xA);
    if (!g_in_logic_draws || !g_opt_tween) return v;
    TweenRecord* e = find_tween_this_pass(t);
    if (!e || e->ph1 != v || e->ph0 == e->ph1) return v;
    return ((int)e->ph0 + (int)e->ph1) / 2;
}

static void pre() {
    poll_hotkeys();
    bool a = menu002_claims();
    if (a != g_claimed_now) {
        log_line("X2-MENU-002: main menu %s (mode 0x%X, request %d)", a ? "at 60 Hz, logic every 2nd pass, slides blended" : "released",
                 *g_menu_mode, *g_menu_request);
        g_tween_count = 0;
        g_task_draw_phase_valid = false;
    }
    g_claimed_now = a;
    g_render_only_pass = false;
    g_in_logic_draws = false;
    g_fade_saved_valid = false;
}

static void post() {
    if (g_render_only_pass && g_fade_saved_valid) *g_fade_ramp = g_fade_saved;   // the ramp ran once more this pass: undo it
    for (int i = 0; i < g_blend_var_count; i++) {                // put the stepped values back
        BlendVar& v = g_blend_vars[i];
        if (v.wrote && var_get(v) == v.mid) var_set(v, v.post);
        v.wrote = false;
    }
    g_render_only_pass = false;
    g_in_logic_draws = false;
    static LONG last = 0;
    if (g_passes - last >= 1800) {
        last = g_passes;
        log_line("X2-MENU-002: passes=%ld render-only=%ld blended reads=%ld rec_full=%ld draw replays=%ld point/backdrop mids=%ld",
                 g_passes, g_render_passes, g_blended, g_tween_table_full, g_replays, g_blend_var_mids);
    }
}

static bool install() {
    // Sg_MainLoop menu exit (as X2-MENU-001): the in-progress flags
    uint8_t* ex = sig_find("X2-MENU-002 Sg_MainLoop menu exit",
        "E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 01 75 2D",
        0x605507);
    uint8_t* rq = sig_find("X2-MENU-002 Sg_MainLoop menu test call",
        "53 E8 ?? ?? ?? ?? 8B 1D ?? ?? ?? ?? 8B 3D ?? ?? ?? ?? 85 C0 74 48", 0x60549F);
    uint8_t* co = sig_find("X2-MENU-002 Menu_CoroutineIterate",
        "55 8B EC 53 56 8B 75 08 8B DE 81 E3 00 00 FF FF 83 3D ?? ?? ?? ?? 00 57 89 35", 0x7645E0);
    // INC [parity]; INC [counter]; CMP byte [..],0
    uint8_t* cn = sig_find("X2-MENU-002 cursor counter INC",
        "FE 05 ?? ?? ?? ?? FF 05 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00", 0x764734);
    // CALL turbo factor; CMP [0xDF6414],EAX; JL +0x61; CALL (pad/menu input)
    uint8_t* gt = sig_find("X2-MENU-002 step gate",
        "E8 ?? ?? ?? ?? 39 05 ?? ?? ?? ?? 7C 61 E8 ?? ?? ?? ?? E8", 0x764824);
    // CALL 0x780770; CALL Win_StateStep; PUSH 1; CALL Menu_TasksStep; ADD ESP,4; CMP EBX,0x10000
    uint8_t* tl = sig_find("X2-MENU-002 step tail",
        "E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 01 E8 ?? ?? ?? ?? 83 C4 04 81 FB 00 00 01 00", 0x764888);
    // MOV EAX,[fade]; ADD ESP,0x14; MOV [..],ESI; ADD EAX,[fade step]; MOV [fade],EAX
    uint8_t* fd = sig_find("X2-MENU-002 fade ramp",
        "A1 ?? ?? ?? ?? 83 C4 14 89 35 ?? ?? ?? ?? 03 05 ?? ?? ?? ?? A3", 0x7649BB);
    uint8_t* ts = sig_find("X2-MENU-002 Tween_Step",
        "55 8B EC 57 8B 7D 08 8A 07 84 C0 74 0C FE C8 88 07 B8 01 00 00 00 5F 5D C3 8A 4F 01 53 0F B7 5F 08", 0x772EB0);
    uint8_t* gp = sig_find("X2-MENU-002 Tween_GetPhase", "55 8B EC 8B 45 08 0F BF 40 0A 5D C3", 0x772FF0);
    if (!ex || !rq || !co || !cn || !gt || !tl || !fd || !ts || !gp) return false;
    uint8_t* gv = gp + 0x20;                          // Tween_GetValue 0x773010 (0x773000 = the +4 getter)
    if (!sig_match(gp + 0x10, "55 8B EC 8B 45 08 0F BF 40 04 5D C3") || !sig_match(gv, "55 8B EC 8B 45 08 0F BF 40 0E 5D C3")) {
        log_line("  X2-MENU-002: Tween_GetValue not at Tween_GetPhase+0x20 - refused");
        return false;
    }
    uint8_t* test = call_target(rq + 1);
    g_menu_up = (volatile int32_t*)abs_operand("menu in progress", ex + 7);
    g_menu_up2 = (volatile int32_t*)abs_operand("menu in progress 2", ex + 0x14);
    g_menu_request = test && sig_match(test, "8B 0D") ? (volatile int32_t*)abs_operand("menu request", test + 2) : nullptr;
    g_menu_mode = (volatile uint32_t*)abs_operand("menu mode", co + 0x1A);
    g_cursor_counter = (volatile int32_t*)abs_operand("cursor counter", cn + 8);
    g_parity_byte = (volatile uint8_t*)abs_operand("menu parity byte", cn + 2);
    g_fade_ramp = (volatile int32_t*)abs_operand("fade value", fd + 1);
    if (!g_menu_up || !g_menu_up2 || !g_menu_request || !g_menu_mode || !g_cursor_counter || !g_parity_byte || !g_fade_ramp ||
        *(const uint32_t*)(fd + 0x15) != (uint32_t)(uintptr_t)g_fade_ramp) {
        log_line("  X2-MENU-002: menu globals not as proven - refused");
        return false;
    }
    // all sites inside Menu_CoroutineIterate, in order
    if (!(co < cn && cn < gt && gt < tl && tl < fd && fd < co + 0x500)) {
        log_line("  X2-MENU-002: coroutine sites out of order - refused");
        return false;
    }
    uint8_t* gate = call_target(gt);
    uint8_t* tasks = call_target(tl + 0x0C);
    if (!gate || !tasks || gt + 0x0D + 0x61 != tl + 0x0A) {   // JL rel8 0x61 (gt+0xB) must land on PUSH 1 (tl+0xA)
        log_line("  X2-MENU-002: gate JL does not land on the task step - refused");
        return false;
    }
    // Menu_TasksStep: MOV ESI,&task[0].step (+0xC); ... ADD ESI,0x9C; CMP ESI,end; JL
    if (!sig_match(tasks, "55 8B EC 56 57 8B 7D 08 BE ?? ?? ?? ?? 8D 49 00 0F B6 46 33 3B C7") ||
        !sig_match(tasks + 0x63, "81 C6 9C 00 00 00 81 FE ?? ?? ?? ?? 7C 9F")) {
        log_line("  X2-MENU-002: Menu_TasksStep not as proven - refused");
        return false;
    }
    uint8_t* first = abs_operand("task table", tasks + 9);
    uint32_t end = *(const uint32_t*)(tasks + 0x6B);
    if (!first || end - (uint32_t)(uintptr_t)first != (uint32_t)(TASK_COUNT * TASK_SIZE)) {
        log_line("  X2-MENU-002: task table is not 16 x 0x9C - refused");
        return false;
    }
    g_task_table = first - 0xC;
    g_opt_drawreplay = config_row("X2-MENU-002-DRAWREPLAY", 1);
    g_opt_parity = config_row("X2-MENU-002-PARITY", 1);
    g_opt_tween = config_row("X2-MENU-002-TWEEN", 1);
    g_opt_point = config_row("X2-MENU-002-POINT", 1);
    g_opt_backdrop = config_row("X2-MENU-002-BACKDROP", 1);
    // POINT: FUN_00761f30 MOV EAX,[target x]; CMP EAX,[x]; JNE; MOV EAX,[target y]; CMP EAX,[y]; JE; PUSH tween
    uint8_t* pt = sig_find("X2-MENU-002-POINT step",
        "A1 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 75 0D A1 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 74 35 68", 0x761F30);
    uint8_t* px = pt ? abs_operand("point x", pt + 7) : nullptr;
    uint8_t* py = pt ? abs_operand("point y", pt + 0x14) : nullptr;
    if (px && py && *(const uint32_t*)(pt + 0x3E) == (uint32_t)(uintptr_t)px && *(const uint32_t*)(pt + 0x4B) == (uint32_t)(uintptr_t)py) {
        add_blend_var(px, 4, true, &g_opt_point);
        add_blend_var(py, 4, true, &g_opt_point);
    } else {
        log_line("  X2-MENU-002-POINT: not confirmed - off");
        g_opt_point = 0;
    }
    // BACKDROP: FUN_00761f80 CMP word [active],0; JE; PUSH EDI; MOV DI,[ramp]; CMP DI,-1 ... CMP word [x],AX at +0x4D
    uint8_t* bd = sig_find("X2-MENU-002-BACKDROP step",
        "66 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 57 66 8B 3D ?? ?? ?? ?? 66 83 FF FF", 0x761F80);
    uint8_t* ramp = bd ? abs_operand("backdrop ramp", bd + 0x12) : nullptr;
    uint8_t* bx = bd && sig_match(bd + 0x4D, "66 39 05") ? abs_operand("backdrop x", bd + 0x50) : nullptr;
    if (ramp && bx && bx == ramp + 0x12) {            // record layout (S): ramp +0x12 = 0x16BDC12, x = 0x16BDC24
        add_blend_var(bx, 2, true, &g_opt_backdrop);          // 0x16BDC24 x
        add_blend_var(bx + 2, 2, true, &g_opt_backdrop);      // 0x16BDC26 y
        static const int k_u8[] = { 4, 5, 6, 7, 0xA };   // 0x16BDC28..2B, 0x16BDC2E
        for (int o : k_u8) add_blend_var(bx + o, 1, false, &g_opt_backdrop);
    } else {
        log_line("  X2-MENU-002-BACKDROP: not confirmed - off");
        g_opt_backdrop = 0;
    }
    g_step_gate = (step_gate_fn)gate;
    g_tasks_step = (tasks_step_fn)tasks;
    if (!hook_entry("X2-MENU-002 Tween_Step", ts, (void*)&hook_tween_step, (void**)&g_tween_step_orig)) return false;
    void* dummy = nullptr;
    if (!hook_entry("X2-MENU-002 Tween_GetValue", gv, (void*)&hook_tween_get_value, &dummy) ||
        !hook_entry("X2-MENU-002 Tween_GetPhase", gp, (void*)&hook_tween_get_phase, &dummy)) return false;
    if (!patch_call_rel32(gt, gate, (void*)&hook_step_gate)) return false;
    if (!patch_call_rel32(tl + 0x0C, tasks, (void*)&hook_tasks_step)) {
        patch_call_rel32(gt, (void*)&hook_step_gate, gate);
        return false;
    }
    log_line("  X2-MENU-002: gate gva=0x%08X (0x00764824) -> 0x%08X, tasks gva=0x%08X (0x00764894) -> 0x%08X, counter 0x%08X, fade "
             "0x%08X, Tween_Step 0x%08X, GetValue 0x%08X, GetPhase 0x%08X",
             rt_to_gva(gt), rt_to_gva(gate), rt_to_gva(tl + 0x0C), rt_to_gva(tasks), rt_to_gva((const void*)g_cursor_counter),
             rt_to_gva((const void*)g_fade_ramp), rt_to_gva(ts), rt_to_gva(gv), rt_to_gva(gp));
    return true;
}

Row row_MENU002 = {
    "X2-MENU-002",
    "field main menu at 60 fps: menu logic every 2nd pass, tween slides drawn half-way on logic passes",
    1, install, pre, post, 0
};

} // namespace x2
