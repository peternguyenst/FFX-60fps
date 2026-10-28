// SPDX-License-Identifier: Apache-2.0
// W2S-001: 2D HUD animations (battle HUD groups, Auron's Bushido arrow prompt, Lulu's Fury circle, menus) run through
// Ui2dAnim_RunTrackSet 0xA7B460, whose tracks (Ui2dAnim_TrackStepDraw 0xA7B510) count down their keyframe and draw in
// the same call. It runs once per invocation, so at N passes the animation ran N/2 times as fast as at stock (U
// 28/10/2026: Auron's overdrive arrows blink very fast; D W2-STATIC item 2: 0x01849ACC changes on every BUILD under
// freeze). On render-only invocations the set's parent word and track records are put back after the call: the draw
// stands, the step does not.
// Details: docs/fps60/rows/W2S-001.md
#include "../fps60.h"
#include <string.h>

typedef uint32_t(__cdecl* runset_t)(int slot, int set_offset, int p3);

static uint8_t*  g_entry = nullptr;
static runset_t  g_tramp = nullptr;
static uint8_t** g_res = nullptr;                // DAT_022fb4dc: 2D animation resource per slot
static bool      g_patched = false;
static volatile LONG g_held = 0, g_big = 0;

enum { MAX_BLOCK = 4096 };

// uint(int slot, int set_offset, int p3), plain RET (0xA7B508/0xA7B50F).
static uint32_t __cdecl h_runset(int slot, int set_offset, int p3) {
    if (g_disabled || !row_W2S001.on || pacing_kind() != INV_BUILD || set_offset == 0 || slot < 0 || slot > 7)
        return g_tramp(slot, set_offset, p3);
    uint8_t* res = g_res[slot];
    if (!res) return g_tramp(slot, set_offset, p3);
    uint32_t* parent = (uint32_t*)(res + set_offset + 4);
    uint32_t pw = *parent;
    if ((int32_t)pw >= 0) return g_tramp(slot, set_offset, p3);   // not running: the original returns 0
    uint8_t* hdr = res + (pw & 0x3FFFFFFF);
    int32_t count = *(int32_t*)hdr;
    size_t len = 4 + (size_t)(count > 0 ? count : 0) * 0x14;
    if (len > MAX_BLOCK) {
        if (InterlockedIncrement(&g_big) == 1) log_line("W2S-001: track set with %d tracks not held (over %d bytes)", count, MAX_BLOCK);
        return g_tramp(slot, set_offset, p3);
    }
    uint8_t saved[MAX_BLOCK];
    memcpy(saved, hdr, len);
    uint32_t r = g_tramp(slot, set_offset, p3);
    memcpy(hdr, saved, len);
    *parent = pw;
    if (InterlockedIncrement(&g_held) == 1) log_line("W2S-001: first 2D track set held on a render-only invocation (slot %d)", slot);
    return r;
}

static bool install() {
    // Ui2dAnim_RunTrackSet: MOV EDX,[EBP+0xC]; ...; JZ; MOV EAX,[EBP+8]; MOV ECX,[EAX*4+res]; MOV [EBP+0xC],ECX; TEST ECX,ECX
    g_entry = sig_find_unique("W2S-001 Ui2dAnim_RunTrackSet",
        "55 8B EC 8B 55 0C 83 EC 0C 53 85 D2 0F 84 ?? ?? ?? ?? 8B 45 08 8B 0C 85 ?? ?? ?? ?? 89 4D 0C 85 C9 "
        "0F 84 ?? ?? ?? ?? 8D 1C 11", 0xA7B460);
    if (!g_entry) return false;
    // the track call at +0x77 must be Ui2dAnim_TrackStepDraw (prologue PUSH EBP; MOV EBP,ESP)
    if (!sig_match(g_entry + 0x77, "E8") || !sig_match(call_target(g_entry + 0x77), "55 8B EC")) {
        log_line("  W2S-001: track call at +0x77 not found - refused");
        return false;
    }
    g_res = (uint8_t**)read_abs(g_entry + 0x18);
    log_line("  W2S-001: Ui2dAnim_RunTrackSet at gva=0x%08X, resources gva=0x%08X (entry hooked on the game thread)",
             rt_to_gva_exe(g_entry), rt_to_gva_exe(g_res));
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    if (!hook_entry("W2S-001", g_entry, (void*)&h_runset, (void**)&g_tramp)) {
        log_line("W2S-001: entry hook failed - row off");
        row_W2S001.on = 0;
        return;
    }
    log_line("W2S-001: Ui2dAnim_RunTrackSet entry hooked on the game thread (tramp %p)", (void*)g_tramp);
}

Row row_W2S001 = {
    "W2S-001",
    "2D HUD animation tracks (overdrive arrows, Fury circle, battle HUD): keyframes step on logic ticks only",
    1, install, pre, nullptr, 0
};
