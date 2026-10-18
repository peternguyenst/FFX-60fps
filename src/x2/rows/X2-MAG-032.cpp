// SPDX-License-Identifier: Apache-2.0
// X2-MAG-032 (FFX-2 only): 37 magic DLLs (the dressphere-change "ky" DLLs 0896-0901 0908-0913 0917-0919 0926-0931,
// and 0771 0836-0840 0845 0847 0849-0851 0947 0959-0961 0977) read a PS2 frame buffer back as a texture: the draw
// calls service slot +0xCB4 (FUN_005e92b0, cdecl, plain RET, returns the TEX0 of buffer 0 / 1 / ...) with
// (frame counter & 1), the counter read through slot +0xC08. Sg_MainLoop flips the GS draw buffer by the raw counter
// parity each invocation (FUN_00606240(fc) at 0x605908). X2-MAG-009 points +0xC08 at a logic-rate counter, so at
// 60 fps the DLL's parity matches the buffer flip on every second frame only and samples the wrong buffer on the
// others (U 18/10/2026: dressphere change, glowing body and outfit swap every frame). Fix: slot +0xCB4 -> stub that
// passes the raw counter parity for arguments 0/1 (every DLL caller passes only the parity; the EXE calls
// FUN_005e92b0 directly, not through the table). Evidence: outer repo re/ledger/XFADE-BLINK.md (S).
#include "../x2.h"

namespace x2 {

typedef uint64_t(__cdecl* tex0_t)(int32_t buf);

static tex0_t g_orig = nullptr;
static volatile LONG g_fixed = 0;
extern Row row_MAG032;

static uint64_t __cdecl h_tex0(int32_t buf) {
    if ((buf == 0 || buf == 1) && row_MAG032.on && !g_disabled) {
        int32_t raw = (int32_t)(*g_frame_counter & 1);
        if (raw != buf && InterlockedIncrement(&g_fixed) == 1)
            log_line("X2-MAG-032: first frame-buffer read with the logic parity corrected (DLL asked %d, raw %d)", buf, raw);
        buf = raw;
    }
    return g_orig(buf);
}

static bool install() {
    if (!g_frame_counter) { log_line("  X2-MAG-032: frame counter not resolved - refused"); return false; }
    uint8_t* at = sig_find("X2-MAG-032 MagicFile_Start table push",
        "0F 94 C0 50 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 01 00 00 00 FF 15", 0x8578D1);
    if (!at) return false;
    uint8_t* table = abs_operand("service table", at + 0x0A);
    if (!table) return false;
    uint32_t* slot = (uint32_t*)(table + 0xCB4);
    uint8_t* fn = (uint8_t*)(uintptr_t)*slot;
    // PUSH EBP; MOV EBP,ESP; MOV EAX,[EBP+8]; SUB ESP,8; CMP EAX,5; JA +0x5B; JMP [EAX*4+table]
    if (!in_image(fn) || !sig_match(fn, "55 8B EC 8B 45 08 83 EC 08 83 F8 05 77 5B FF 24 85")) {
        log_line("  X2-MAG-032: table+0xCB4 = %p is not the frame-buffer TEX0 getter - refused", fn);
        return false;
    }
    g_orig = (tex0_t)fn;
    if (!patch_u32(slot, (uint32_t)(uintptr_t)&h_tex0)) return false;
    log_line("  X2-MAG-032: service table gva=0x%08X +0xCB4 (gva=0x%08X, expected 0x005E92B0) -> stub, parity from the raw "
             "frame counter", rt_to_gva(table), rt_to_gva(fn));
    return true;
}

Row row_MAG032 = {
    "X2-MAG-032",
    "magic DLLs' frame-buffer readback (service +0xCB4) uses the raw frame-counter parity (dressphere change blink)",
    1, install, nullptr, nullptr, 0
};

} // namespace x2
