// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-OP114 (diagnostic, default 0, observe only): the dressphere-change outfit peel (magic_0906 for Warrior) is
// drawn by the OPU DLL-table op 0x114 (FUN_00875d00, cdecl(scr, pc), plain RET; DLL op table = magic service +0xC24,
// entry +0x50). Its outer model blinks at 60 fps only (U 18/10/2026, re/ledger/XFADE-BLINK.md). This row points the
// table entry at a stub that logs, for the first X2-DIAG-OP114-N calls (default 1200): invocation kind, scr, pc, script
// word, the slot scr[0xD8 + unit[0x1E]] (int), the world matrix translation scr+0x80 (param_1[0x20], built by
// FUN_006a4df0), unit+0x30..0x3C, the part-block flags scr+0x100 after the call and the returned PC.
// Run 2 (D 17:37 showed identical inputs on LOGIC and RENDER calls): the op draws each part through an HD VFX
// instance keyed (part*2 + texture mode, 0x389) (FUN_00875fc0: Hd_VfxLookup 0x4563D0, vertex memory from the
// multi-buffered pool DAT_00dd0204 by FUN_00451350 into geometry slot (pool index + 2) * 0x1C, submit FUN_00456c80,
// gated by g_dwSkipFlag0 0xDF6438). Also logged: pool index and write offset before/after, the skip flags, and per
// key 0..3 the instance, its vertex-slot pointer for the current pool index and vertex count.
// Run 3 (D 17:50: pool index 1 on LOGIC, 0 on RENDER, fresh vertices for both instances on every call, skip flags 0):
// A/B drop test, X2-DIAG-OP114-DROP (ini, default 0; Numpad 8 cycles, logged): 1 = skip both calls on RENDER
// invocations, 2 = skip the 1st call of each invocation (instance k0, "FrameBuffer" texture), 3 = skip the 2nd call
// (k1, dds texture), 4 = skip both always. A skipped call returns pc + 0x10 as the op does (word >> 12 == 1 only).
#include "../x2.h"
#include <stdio.h>

namespace x2 {

typedef uint8_t* (__cdecl* op_t)(uint8_t* scr, uint8_t* pc);
typedef uint8_t* (__cdecl* lookup_t)(uint32_t lo, int32_t hi);
static op_t g_orig = nullptr;
static lookup_t g_lookup = nullptr;      // Hd_VfxLookup 0x4563D0
static uint8_t** g_pool = nullptr;       // &DAT_00dd0204
static uint32_t* g_skip = nullptr;       // &g_dwSkipFlag0 0xDF6438 (+4 0xDF643C, +8 0xDF6440)
static volatile LONG g_left = 0;
static int g_drop = 0;                   // game thread
static int g_call = 0;                   // call index in this invocation (game thread)
static uint8_t* g_lastpc = nullptr;
extern Row row_DIAGOP114;

static void pre() {
    static bool was = false;
    bool down = (GetAsyncKeyState(VK_NUMPAD8) & 0x8000) != 0;
    if (down && !was) {
        g_drop = (g_drop + 1) % 5;
        static const char* const D[5] = { "none", "both calls on RENDER", "1st call (k0 FrameBuffer)",
                                          "2nd call (k1 dds)", "both calls always" };
        log_line("Num 8: X2-DIAG-OP114-DROP %d (%s)", g_drop, D[g_drop]);
    }
    was = down;
}

// pool index and current buffer write offset (FUN_0049fb40 / FUN_0049fa10(0))
static void pool_state(int* idx, uint32_t* wr) {
    *idx = -1; *wr = 0;
    uint8_t* mgr = *g_pool;
    if (!mgr) return;
    int* st = *(int**)(mgr + 8);
    if (!st) return;
    *idx = st[0];
    if (st[1] == 0) return;
    uint8_t* buf = *(uint8_t**)(st[3] + st[0] * 4);
    *wr = *(uint32_t*)(buf + 8) - *(uint32_t*)(buf + 4);
}

static uint8_t* __cdecl h_op(uint8_t* scr, uint8_t* pc) {
    int i0 = -1; uint32_t w0 = 0;
    bool log = row_DIAGOP114.on && g_left > 0;
    if (log) __try { pool_state(&i0, &w0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_call = (pc > g_lastpc) ? g_call + 1 : 0;
    g_lastpc = pc;
    bool render = pacing_kind() == INV_BUILD;
    bool drop = (*(uint16_t*)(scr + 0x21A) >> 12) == 1 &&
                ((g_drop == 1 && render) || (g_drop == 2 && g_call == 0) || (g_drop == 3 && g_call == 1) || g_drop == 4);
    uint8_t* r = drop ? pc + 0x10 : g_orig(scr, pc);
    if (log && InterlockedDecrement(&g_left) >= 0) {
        __try {
            uint8_t* unit = *(uint8_t**)(scr + 0x220);
            int k = unit ? unit[0x1E] : 0;
            int slot = *(int*)(scr + (0xD8 + k) * 4);
            const float* m = (const float*)(scr + 0x80);
            const float* u = unit ? (const float*)(unit + 0x30) : nullptr;
            int i1; uint32_t w1;
            pool_state(&i1, &w1);
            char inst[256]; int n = 0;
            for (uint32_t key = 0; key < 4; key++) {
                uint8_t* in = g_lookup(key, 0x389);
                if (!in) { n += sprintf_s(inst + n, sizeof(inst) - n, " k%u -", key); continue; }
                uint8_t* geom = *(uint8_t**)(in + 0x88);
                uint32_t vtx = (geom && i1 >= 0) ? *(uint32_t*)(geom + (i1 + 2) * 0x1C) : 0;
                n += sprintf_s(inst + n, sizeof(inst) - n, " k%u %p vtx %08X n %d", key, in, vtx,
                               geom ? *(int*)(geom + 4) : -1);
            }
            static const char* const K[4] = { "LOGIC", "RENDER", "BACKLOG", "STOCK" };
            log_line("OP114 %s scr %p pc %p word %04X slot %d (k %d) mtx (%.2f %.2f %.2f) unit30 (%.2f %.2f %.2f %.2f) "
                     "flags %08X ret %+d call %d%s | skip %u %u %u pool %d +%X -> %d +%X |%s",
                     K[pacing_kind() & 3], scr, pc, *(uint16_t*)(scr + 0x21A), slot, k, m[12], m[13], m[14],
                     u ? u[0] : 0.0f, u ? u[1] : 0.0f, u ? u[2] : 0.0f, u ? u[3] : 0.0f,
                     *(uint32_t*)(scr + 0x100), (int)(r - pc), g_call, drop ? " DROPPED" : "", g_skip[0], g_skip[1], g_skip[2], i0, w0, i1, w1, inst);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

static bool install() {
    uint8_t* at = sig_find("X2-DIAG-OP114 MagicFile_Start table push",
        "0F 94 C0 50 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 01 00 00 00 FF 15", 0x8578D1);
    uint8_t* svc = at ? abs_operand("service table", at + 0x0A) : nullptr;
    if (!svc) return false;
    // FUN_004563d0: PUSH EBP; MOV EBP,ESP; MOV ECX,[EBP+8]; MOV EDX,[EBP+0xC]; MOV EAX,ECX; OR EAX,EDX; JZ +0x10; ...;
    // CALL 0x4b31d0 (rel32 pinned: FUN_00456380 has the same shape with CALL 0x4b30f0)
    uint8_t* lk = sig_find("X2-DIAG-OP114 Hd_VfxLookup",
        "55 8B EC 8B 4D 08 8B 55 0C 8B C1 0B C2 74 10 52 51 E8 ?? ?? ?? ?? 8B C8 E8 E3 CD 05 00 5D C3 33 C0 5D C3",
        0x4563D0);
    // FUN_00451310: MOV ECX,[0x00dd0204]; XOR EAX,EAX; CMP [ECX+8],EAX; SETNZ AL; RET
    uint8_t* pl = sig_find("X2-DIAG-OP114 pool test", "8B 0D ?? ?? ?? ?? 33 C0 39 41 08 0F 95 C0 C3", 0x451310);
    // FUN_00456980: CMP [0x00df6438],0; JNZ +0x0B; MOV ECX,[0x00dcda04]; JMP 0x465030 (rel32 pinned: FUN_00453aa0
    // has the same shape with JMP 0x482140)
    uint8_t* sk = sig_find("X2-DIAG-OP114 skip flag test", "83 3D ?? ?? ?? ?? 00 75 0B 8B 0D ?? ?? ?? ?? E9 9C E6 00 00",
        0x456980);
    if (!lk || !pl || !sk) return false;
    g_pool = (uint8_t**)abs_operand("pool manager ptr", pl + 2);
    g_skip = (uint32_t*)abs_operand("skip flag 0", sk + 2);
    if (!g_pool || !g_skip) return false;
    g_lookup = (lookup_t)lk;
    uint8_t* tab = (uint8_t*)(uintptr_t)*(uint32_t*)(svc + 0xC24);
    if (!in_image(tab)) { log_line("  X2-DIAG-OP114: service +0xC24 = %p not in the image - off", tab); return false; }
    uint32_t* entry = (uint32_t*)(tab + 0x14 * 4);
    uint8_t* fn = (uint8_t*)(uintptr_t)*entry;
    // PUSH EBP; MOV EBP,ESP; SUB ESP,0x114; cookie; MOV ECX,[EBP+0xC]; PUSH EBX; PUSH EDI; MOV EDI,[EBP+8]
    if (!in_image(fn) || !sig_match(fn, "55 8B EC 81 EC 14 01 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 8B 4D 0C 53 57 8B 7D 08")) {
        log_line("  X2-DIAG-OP114: op table entry 0x114 = %p is not FUN_00875d00 - off", fn);
        return false;
    }
    g_orig = (op_t)fn;
    g_left = config_row("X2-DIAG-OP114-N", 1200);
    g_drop = config_row("X2-DIAG-OP114-DROP", 0);
    if (g_drop < 0 || g_drop > 4) g_drop = 0;
    if (!patch_u32(entry, (uint32_t)(uintptr_t)&h_op)) return false;
    log_line("  X2-DIAG-OP114: entry gva=0x%08X (0x00D6F808) -> stub (FUN gva=0x%08X, 0x00875D00), %ld calls logged, drop mode %d; "
             "pool ptr gva=0x%08X (0x00DD0204), skip gva=0x%08X (0x00DF6438)",
             rt_to_gva(entry), rt_to_gva(fn), (LONG)g_left, g_drop, rt_to_gva(g_pool), rt_to_gva(g_skip));
    return true;
}

Row row_DIAGOP114 = {
    "X2-DIAG-OP114",
    "diagnostic: log OPU op 0x114 (dressphere outfit peel draw) inputs, HD VFX instances and vertex pool per call",
    0, install, pre, nullptr, 0
};

} // namespace x2
