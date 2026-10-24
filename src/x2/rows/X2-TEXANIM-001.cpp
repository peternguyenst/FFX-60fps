// SPDX-License-Identifier: Apache-2.0
// X2-TEXANIM-001: animated character textures (scrolling and flipbook textures) run at their normal speed.
//
// The texture animation step is called from the skeleton job, which runs on every invocation, so it ran twice per
// tick. Its CALL (gva 0x6E1924) goes to h_step, which steps only on logic invocations. Evidence: re/ledger/PORT-B.md §4.
#include "../x2.h"

namespace x2 {

typedef void(__cdecl* texanim_t)(int, int, int); // cdecl, 3 args, plain RET 0x53B71E, caller cleans 0xC
static texanim_t g_step = nullptr;
static volatile LONG g_skipped = 0, g_passed = 0;
extern Row row_TEXANIM001;

// May run on the skeleton job worker threads; the pacing kind is set on the game thread before the dispatch.
static void __cdecl h_step(int a, int b, int c) {
    if (row_TEXANIM001.on && !g_disabled && render_only()) { InterlockedIncrement(&g_skipped); return; }
    InterlockedIncrement(&g_passed);
    g_step(a, b, c);
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_skipped > 0) {
        reported = 1;
        log_line("X2-TEXANIM-001: first texture-anim step skipped on a render-only invocation (passed %ld)", g_passed);
    }
}

static bool install() {
    // skeleton job: PUSH [ESI+0x6A0]; PUSH [ESI-0x1A0]; PUSH [ESI+0x56C]; CALL stepper; ADD ESP,0xC
    uint8_t* a = sig_find("X2-TEXANIM-001 texanim step (skeleton job)",
        "FF B6 A0 06 00 00 FF B6 60 FE FF FF FF B6 6C 05 00 00 E8 ?? ?? ?? ?? 83 C4 0C", 0x6E1912);
    if (!a) return false;
    uint8_t* site = a + 0x12;
    uint8_t* fn = call_target(site);
    // stepper: slot*0x30 into the texture-anim table, type byte dispatch
    if (!fn || !sig_match(fn, "55 8B EC 8B 4D 08 8D 04 49 C1 E0 04 0F BE 80")) {
        log_line("  X2-TEXANIM-001: CALL target %p is not the stepper - refused", fn);
        return false;
    }
    g_step = (texanim_t)fn;
    if (!patch_call_rel32(site, fn, (void*)&h_step)) {
        log_line("  X2-TEXANIM-001: patch failed - refused");
        return false;
    }
    log_line("  X2-TEXANIM-001: stepper gva=0x%08X (expected 0x0053B6F0), CALL gva=0x%08X -> stub",
             rt_to_gva(fn), rt_to_gva(site));
    return true;
}

Row row_TEXANIM001 = {
    "X2-TEXANIM-001",
    "character texture animation (FUN_0053b6f0: UV scroll/flipbook) only on logic invocations",
    1, install, nullptr, post, 0
};

} // namespace x2
