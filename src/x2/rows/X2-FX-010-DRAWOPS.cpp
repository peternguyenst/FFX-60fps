// SPDX-License-Identifier: Apache-2.0
// X2-FX-010-DRAWOPS (port of FFX FX-010-DRAWOPS): the particle draw ops pppDrawShapeCamera FUN_008320a0 and
// pppDrawShapeCameraDisPos FUN_00833040 put their draw call behind `if (pause == 0)`, together with the sub-particle
// spawn and integrate. On render-only invocations the pause flag 0x1CF579C is set (X2-MAG-006, and the Ego framework
// of magic_0078 itself), so these effects are drawn on logic invocations only: they blink (hit flash).
// Fix: the JNZ of the gate is NOPed and the spawn / integrate CALLs go to stubs that skip while paused, so the draw
// runs on every invocation and the sub-particles still step once per tick.
// Evidence: outer repo re/ledger/DRAWCAM.md (S); sigs tools/sigs_x2_drawcam.txt.
#include "../x2.h"

namespace x2 {

typedef void(__cdecl* spawn_t)(int, int, int);  // FUN_00831d30 / FUN_00832c30: cdecl, plain RET, caller ADD ESP,0xC
typedef void(__cdecl* integ_t)(int, int);       // FUN_00831e70 / FUN_00832e30: cdecl, plain RET, caller ADD ESP,0x18
static const char* const k_name[2] = { "DrawShapeCamera", "DrawShapeCameraDisPos" };
static uint8_t*          g_gate[2] = {};         // CMP dword [pause],0
static spawn_t           g_spawn[2] = {};
static integ_t           g_integ[2] = {};
static volatile int32_t* g_pause = nullptr;
static bool              g_patched = false;
static volatile LONG     g_paused_draws[2] = {}, g_draws[2] = {};
extern Row row_FX010DRAWOPS;

// pppKeShpTail3 0x83C0B0 (FFX's "pyrefly" op, U 16/10/2026: mission-start hex grid blinks; PPP-GATES.md, S): its entry
// gate `CMP [pause],0 ... JNE end` (m+0xD) skips the whole draw; its one lasting write (+0x290 step) has its own inner
// check at end-0x1F. Fix: NOP the entry JNE; the [0xDF6438] branch at m+0x38, which jumped past the inner check to the
// step, now lands on the inner check (at pause 0 the same bytes run as stock). Sub-switch X2-FX-010-DRAWOPS-TAIL3.
static uint8_t* g_t3 = nullptr;                  // m = CMP [pause],0 at 0x83C118
static uint8_t* g_t3_inner = nullptr;            // 0x83D16E

template <int I> static void __cdecl h_spawn(int a, int b, int c) {
    if (*g_pause) return;
    g_spawn[I](a, b, c);
}

// runs right after the draw: counts every draw, and the ones the stock gate would have hidden
template <int I> static void __cdecl h_integ(int a, int b) {
    InterlockedIncrement(&g_draws[I]);
    if (*g_pause) { InterlockedIncrement(&g_paused_draws[I]); return; }
    g_integ[I](a, b);
}

static const spawn_t k_spawn_stub[2] = { &h_spawn<0>, &h_spawn<1> };
static const integ_t k_integ_stub[2] = { &h_integ<0>, &h_integ<1> };

// Patterns stay <= 96 bytes (sig parser cap). Both start 0x29 before the CMP. From the CMP: +7 JNZ +0x2D; +0x19 CALL spawn; +0x27 CALL draw (kept); +0x2E CALL integrate; +0x36 ADD ESP,0x18
static bool install() {
    uint8_t* a = sig_find("X2-FX-010-DRAWOPS DrawShapeCamera gate",
        "7E 20 8D 49 00 "
        "F6 01 01 75 0D 40 81 C1 A0 00 00 00 3B C2 7C F0 EB 0B 3B C2 7D 07 81 46 0C 00 F0 FF FF C7 47 54 01 00 00 00 "
        "83 3D ?? ?? ?? ?? 00 75 2D A1 ?? ?? ?? ?? 80 78 1D 00 75 0D FF 75 10 53 56 E8 ?? ?? ?? ?? 83 C4 0C "
        "57 FF 75 10 53 56 E8 ?? ?? ?? ?? 57 53 E8 ?? ?? ?? ?? 83 C4 18", 0x83229B);
    uint8_t* b = sig_find("X2-FX-010-DRAWOPS DrawShapeCameraDisPos gate",
        "EB 03 ?? ?? ?? "
        "F6 01 01 75 0D 40 81 C1 A0 00 00 00 3B C2 7C F0 EB 0B 3B C2 7D 07 81 46 0C 00 F0 FF FF C7 47 54 01 00 00 00 "
        "83 3D ?? ?? ?? ?? 00 75 2D A1 ?? ?? ?? ?? 80 78 1D 00 75 0D FF 75 10 53 56 E8 ?? ?? ?? ?? 83 C4 0C "
        "57 FF 75 10 53 56 E8 ?? ?? ?? ?? 57 53 E8 ?? ?? ?? ?? 83 C4 18", 0x83319B);
    if (!a || !b) return false;
    g_gate[0] = a + 0x29;
    g_gate[1] = b + 0x29;
    for (int i = 0; i < 2; i++) {
        uint8_t* m = g_gate[i];
        volatile int32_t* flag = (volatile int32_t*)abs_operand("particle pause flag", m + 2);
        if (!flag || (g_pause && flag != g_pause)) {
            log_line("  X2-FX-010-DRAWOPS: %s gate reads another flag - refused", k_name[i]);
            return false;
        }
        g_pause = flag;
        g_spawn[i] = (spawn_t)call_target(m + 0x19);
        g_integ[i] = (integ_t)call_target(m + 0x2E);
        uint8_t* draw = call_target(m + 0x27);
        if (!g_spawn[i] || !g_integ[i] || !in_image((void*)g_spawn[i]) || !in_image((void*)g_integ[i]) || !in_image(draw)) {
            log_line("  X2-FX-010-DRAWOPS: %s CALL targets not in FFX-2.exe - refused", k_name[i]);
            return false;
        }
        log_line("  X2-FX-010-DRAWOPS: %s gate gva=0x%08X, spawn 0x%08X, draw 0x%08X, integrate 0x%08X", k_name[i],
                 rt_to_gva(m), rt_to_gva((void*)g_spawn[i]), rt_to_gva(draw), rt_to_gva((void*)g_integ[i]));
    }
    log_line("  X2-FX-010-DRAWOPS: pause flag gva=0x%08X (expected 0x01CF579C), expected gates 0x008322C4 / 0x008331C4",
             rt_to_gva((const void*)g_pause));
    if (config_row("X2-FX-010-DRAWOPS-TAIL3", 1)) {
        uint8_t* m = sig_find("X2-FX-010-DRAWOPS KeShpTail3 entry gate",
            "83 3D ?? ?? ?? ?? 00 89 85 64 FE FF FF 0F 85 ?? ?? ?? ?? 81 7F 04 FF FF 00 00 0F 84 ?? ?? ?? ?? "
            "83 3D ?? ?? ?? ?? 00 8B 85 AC FE FF FF 8B 40 0C 8B 08 89 8D C8 FE FF FF 0F 85 ?? ?? ?? ??", 0x83C118);
        uint8_t* end = m ? m + 0x13 + *(int32_t*)(m + 0xF) : nullptr;          // JNE end at m+0xD
        uint8_t* inner = end ? end - 0x1F : nullptr;
        if (!m || (volatile int32_t*)abs_operand("particle pause flag", m + 2) != g_pause ||
            !sig_match(inner, "83 3D ?? ?? ?? ?? 00 75 16 8B 8D C8 FE FF FF 8B BD A4 FE FF FF 8B 47 08 01 84 31 90 02 00 00") ||
            (volatile int32_t*)abs_operand("particle pause flag", inner + 2) != g_pause ||
            m + 0x3E + *(int32_t*)(m + 0x3A) != inner + 0x15) {
            log_line("  X2-FX-010-DRAWOPS-TAIL3: KeShpTail3 gates not as expected - off (it still blinks while paused)");
        } else {
            g_t3 = m;
            g_t3_inner = inner;
            log_line("  X2-FX-010-DRAWOPS-TAIL3: KeShpTail3 entry gate gva=0x%08X (0x0083C118), inner gate gva=0x%08X (0x0083D16E)",
                     rt_to_gva(m), rt_to_gva(inner));
        }
    }
    return true;
}

// stock and patched dwords for X2-DEV-BISECT-AUTO's runtime switch (the spawn / integrate stubs stay: with the gate
// back, a paused draw never reaches them and unpaused they pass through)
static uint32_t g_gate_orig[2], g_gate_new[2];
static bool g_gate_ok[2] = {};
static uint32_t g_t3_orig[3], g_t3_new[3];      // m+0x3A, m+0xD, m+0x11
static bool g_t3_ok = false, g_live = true;

static void pre() {
    if (g_patched) return;
    g_patched = true;
    // on the game thread, between invocations: CALLs first, the JNZ last (the draw never runs with an unguarded step)
    for (int i = 0; i < 2; i++) {
        uint8_t* m = g_gate[i];
        if (!patch_call_rel32(m + 0x19, (void*)g_spawn[i], (void*)k_spawn_stub[i]) ||
            !patch_call_rel32(m + 0x2E, (void*)g_integ[i], (void*)k_integ_stub[i])) {
            log_line("X2-FX-010-DRAWOPS: %s CALL patch failed - stays stock", k_name[i]);
            continue;
        }
        uint32_t w = *(uint32_t*)(m + 7);         // 75 2D A1 xx
        if ((w & 0xFFFF) != 0x2D75 || !patch_u32(m + 7, (w & 0xFFFF0000) | 0x9090)) {
            log_line("X2-FX-010-DRAWOPS: %s gate JNZ patch failed (stubs pass through, gate stock)", k_name[i]);
            continue;
        }
        g_gate_orig[i] = w; g_gate_new[i] = (w & 0xFFFF0000) | 0x9090; g_gate_ok[i] = true;
        log_line("X2-FX-010-DRAWOPS: %s gate NOPed: draws while paused, spawn/integrate once per tick", k_name[i]);
    }
    if (g_t3) {
        // branch first (it then lands on the inner check, same as stock at pause 0), the entry JNE last
        uint8_t* m = g_t3;
        uint32_t w = *(uint32_t*)(m + 0x11);       // rel32 high half, then 81 7F
        g_t3_orig[0] = *(uint32_t*)(m + 0x3A); g_t3_orig[1] = *(uint32_t*)(m + 0xD); g_t3_orig[2] = w;
        g_t3_new[0] = (uint32_t)(g_t3_inner - (m + 0x3E)); g_t3_new[1] = 0x90909090u; g_t3_new[2] = (w & 0xFFFF0000u) | 0x9090u;
        if (!patch_u32(m + 0x3A, g_t3_new[0]) || !patch_u32(m + 0xD, g_t3_new[1]) || !patch_u32(m + 0x11, g_t3_new[2])) {
            log_line("X2-FX-010-DRAWOPS-TAIL3: patch failed");
            return;
        }
        g_t3_ok = true;
        log_line("X2-FX-010-DRAWOPS-TAIL3: KeShpTail3 entry gate NOPed (draws while paused), +0x290 step behind the inner check");
    }
}

// X2-DEV-BISECT-AUTO (game thread, between invocations): stock gates back (off) or NOPed again (on). TAIL3 keeps the
// install order: on = branch then entry, off = entry then branch.
void fx010_set_live(bool on) {
    if (!g_patched || on == g_live) return;
    g_live = on;
    for (int i = 0; i < 2; i++)
        if (g_gate_ok[i]) patch_u32(g_gate[i] + 7, on ? g_gate_new[i] : g_gate_orig[i]);
    if (g_t3_ok) {
        uint8_t* m = g_t3;
        if (on) { patch_u32(m + 0x3A, g_t3_new[0]); patch_u32(m + 0xD, g_t3_new[1]); patch_u32(m + 0x11, g_t3_new[2]); }
        else { patch_u32(m + 0x11, g_t3_orig[2]); patch_u32(m + 0xD, g_t3_orig[1]); patch_u32(m + 0x3A, g_t3_orig[0]); }
    }
}

static void post() {
    static LONG reported[2] = {};
    for (int i = 0; i < 2; i++) {
        if (!reported[i] && g_paused_draws[i] > 0) {
            reported[i] = 1;
            log_line("X2-FX-010-DRAWOPS: first paused %s draw kept (draws so far %ld)", k_name[i], g_draws[i]);
        }
    }
    static uint32_t last = 0;
    static LONG last_draws = 0;
    uint32_t now = GetTickCount();
    if (now - last >= 30000) {
        last = now;
        LONG d = g_draws[0] + g_draws[1];
        if (d != last_draws) {
            last_draws = d;
            log_line("X2-FX-010-DRAWOPS: draws %s %ld (paused %ld), %s %ld (paused %ld)", k_name[0], g_draws[0],
                     g_paused_draws[0], k_name[1], g_draws[1], g_paused_draws[1]);
        }
    }
}

Row row_FX010DRAWOPS = {
    "X2-FX-010-DRAWOPS",
    "particle draw ops DrawShapeCamera / DisPos draw while paused (hit flash blink); sub-particles step once per tick",
    1, install, pre, post, 0
};

} // namespace x2
