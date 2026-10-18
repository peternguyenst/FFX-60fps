// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-OP114 (diagnostic, default 0, observe only): the dressphere-change outfit peel (magic_0906 for Warrior) is
// drawn by the OPU DLL-table op 0x114 (FUN_00875d00, cdecl(scr, pc), plain RET; DLL op table = magic service +0xC24,
// entry +0x50). Its outer model blinks at 60 fps only (U 18/10/2026, re/ledger/XFADE-BLINK.md). This row points the
// table entry at a stub that logs, for the first X2-DIAG-OP114-N calls (default 400): invocation kind, scr, script
// word, the peel timer scr[0xD8 + unit[0x1E]] (float), the world matrix translation scr+0x80 (param_1[0x20], built by FUN_006a4df0), unit+0x30..0x3C, the
// part-block flags scr+0x100 after the call and the returned PC.
#include "../x2.h"

namespace x2 {

typedef uint8_t* (__cdecl* op_t)(uint8_t* scr, uint8_t* pc);
static op_t g_orig = nullptr;
static volatile LONG g_left = 0;
extern Row row_DIAGOP114;

static uint8_t* __cdecl h_op(uint8_t* scr, uint8_t* pc) {
    uint8_t* r = g_orig(scr, pc);
    if (row_DIAGOP114.on && g_left > 0 && InterlockedDecrement(&g_left) >= 0) {
        __try {
            uint8_t* unit = *(uint8_t**)(scr + 0x220);
            int k = unit ? unit[0x1E] : 0;
            float timer = *(float*)(scr + (0xD8 + k) * 4);
            const float* m = (const float*)(scr + 0x80);
            const float* u = unit ? (const float*)(unit + 0x30) : nullptr;
            static const char* const K[4] = { "LOGIC", "RENDER", "BACKLOG", "STOCK" };
            log_line("OP114 %s scr %p word %04X timer %.3f (slot %d) mtx (%.2f %.2f %.2f) unit30 (%.2f %.2f %.2f %.2f) "
                     "flags %08X ret %+d",
                     K[pacing_kind() & 3], scr, *(uint16_t*)(scr + 0x21A), timer, k, m[12], m[13], m[14],
                     u ? u[0] : 0.0f, u ? u[1] : 0.0f, u ? u[2] : 0.0f, u ? u[3] : 0.0f,
                     *(uint32_t*)(scr + 0x100), (int)(r - pc));
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

static bool install() {
    uint8_t* at = sig_find("X2-DIAG-OP114 MagicFile_Start table push",
        "0F 94 C0 50 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 01 00 00 00 FF 15", 0x8578D1);
    uint8_t* svc = at ? abs_operand("service table", at + 0x0A) : nullptr;
    if (!svc) return false;
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
    g_left = config_row("X2-DIAG-OP114-N", 400);
    if (!patch_u32(entry, (uint32_t)(uintptr_t)&h_op)) return false;
    log_line("  X2-DIAG-OP114: entry gva=0x%08X (0x00D6F808) -> stub (FUN gva=0x%08X, 0x00875D00), %ld calls logged",
             rt_to_gva(entry), rt_to_gva(fn), (LONG)g_left);
    return true;
}

Row row_DIAGOP114 = {
    "X2-DIAG-OP114",
    "diagnostic: log OPU op 0x114 (dressphere outfit peel draw) inputs per call",
    0, install, nullptr, nullptr, 0
};

} // namespace x2
