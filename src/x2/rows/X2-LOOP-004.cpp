// SPDX-License-Identifier: Apache-2.0
// X2-LOOP-004: camera tweens and camera shake run at their normal speed.
//
// The viewport finalize steps the camera tweens and the shake (with its own random number stream) every time a frame
// is drawn, so twice per tick at 60 fps. The 3 viewport camera blocks (gva 0xE12A40, 0x1C0 bytes each) and the random
// seed are saved at the start of each render-only invocation and put back at its end (rollback, x2_rows.cpp).
// Evidence: re/ledger/PORT-A.md §3.
#include "../x2.h"

namespace x2 {

static bool install() {
    // FUN_0064fc10: stride 0x1C0 (vp < 3) ...
    if (!sig_find("X2-LOOP-004 viewport stride",
            "83 FE 03 7C 14 B8 01 00 00 00 5E 8B 4D FC 33 CD E8 ?? ?? ?? ?? 8B E5 5D C3 53 8B DE 69 DB C0 01 00 00 "
            "89 5D E4 80 BB ?? ?? ?? ?? 00", 0x64FC27))
        return false;
    // ... and the block base: LEA EAX,[EBX+base] next to the battle-camera stride 0x1040
    uint8_t* bb = sig_find("X2-LOOP-004 viewport block base",
        "69 F6 40 10 00 00 DD 05 ?? ?? ?? ?? 8D 83 ?? ?? ?? ?? 83 C4 04 81 C6 ?? ?? ?? ?? 8D B8 C2 00 00 00 "
        "8D 88 DC 00 00 00", 0x64FE9E);
    if (!bb) return false;
    uint8_t* block = abs_operand("viewport blocks", bb + 0x0E);
    // the battle-camera array follows the 3 blocks (ADD ESI,base+0x540)
    if (!block || *(const uint32_t*)(bb + 0x17) != (uint32_t)(uintptr_t)(block + 3 * 0x1C0)) {
        log_line("  X2-LOOP-004: battle camera array is not right after the blocks - refused");
        return false;
    }
    // Btl_Rand(stream) FUN_0061e270: load (+0x11) and store (+0x30) use the same seed table
    uint8_t* rng = sig_find("X2-LOOP-004 Btl_Rand",
        "55 8B EC 8B 55 08 8B 04 95 ?? ?? ?? ?? 0F AF 04 95 ?? ?? ?? ?? 8D 0C 80 0F B7 04 55", 0x61E270);
    if (!rng) return false;
    if (rng[0x2D] != 0x89 || rng[0x2E] != 0x04 || rng[0x2F] != 0x95 ||
        *(const uint32_t*)(rng + 0x11) != *(const uint32_t*)(rng + 0x30)) {
        log_line("  X2-LOOP-004: seed load/store bases differ - refused");
        return false;
    }
    uint8_t* seeds = abs_operand("rng seeds", rng + 0x11);
    if (!seeds) return false;
    log_line("  X2-LOOP-004: blocks gva=0x%08X (expected 0x00E12A40), stream 9 gva=0x%08X (expected 0x00E10414)",
             rt_to_gva(block), rt_to_gva(seeds + 9 * 4));
    return rollback_add_static("X2-LOOP-004", block, 3 * 0x1C0) &&
           rollback_add_static("X2-LOOP-004", seeds + 9 * 4, 4);
}

Row row_LOOP004 = {
    "X2-LOOP-004",
    "viewport camera tween/shake blocks + RNG stream 9 rolled back on render-only invocations",
    1, install, nullptr, nullptr, 0
};

} // namespace x2
