// SPDX-License-Identifier: Apache-2.0
// FFX-2 hooks: the runner's CALL Sg_MainLoop re-pointed to a wrapper (pre/post around every invocation) and
// the SKS_SetFrameDivider entry hook (MinHook). Own state; shares nothing with the FFX mod's hooks.cpp.
#include "x2.h"
#include "../../third_party/minhook/include/MinHook.h"

namespace x2 {

volatile LONG g_disabled = 0;
bool g_divider_hooked = false;

static void on_fault(const char* where, DWORD code) {
    if (InterlockedExchange(&g_disabled, 1) == 0)
        log_line("FAULT 0x%08lX in %s - x2 disabled, hooks are pass-through from now on", code, where);
}

extern "C" void __cdecl x2_pre() {
    if (g_disabled) return;
    __try {
        if (perf_on()) perf_inv_begin();
        hooks_install_divider_on_game_thread();
        pacing_pre();
        rollback_pre();
        rows_pre();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("pre", GetExceptionCode()); }
}

extern "C" void __cdecl x2_post() {
    if (g_disabled) return;
    __try {
        rows_post();
        rollback_post();
        pacing_post();
        if (perf_on()) perf_inv_end();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("post", GetExceptionCode()); }
#ifdef FPS60_DEV
    __try { reload_poll(); }                     // between invocations, on the game thread
    __except (EXCEPTION_EXECUTE_HANDLER) { on_fault("reload", GetExceptionCode()); }
#endif
}

static void* g_mainloop_target = nullptr;      // read by the naked wrapper

// Sg_MainLoop: cdecl, one float (delta), plain RET (re/ledger/LOOP.md). The wrapper re-pushes the caller's arg.
__declspec(naked) static void sg_mainloop_wrapper() {
    __asm {
        call x2_pre
        push dword ptr [esp + 4]
        call dword ptr [g_mainloop_target]
        add  esp, 4
        call x2_post
        ret
    }
}

// Every patch_u32 write is journaled (address, old and new value) so a dev hot reload can undo it.
struct Patch { uint8_t* at; uint32_t orig, now; };
static Patch g_patch[512];
static int   g_npatch = 0;

static bool write_u32(uint8_t* at, uint32_t v) {
    DWORD old;
    if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    if ((((uintptr_t)at) & 3) == 0) InterlockedExchange((volatile LONG*)at, (LONG)v);
    else *(uint32_t*)at = v;
    VirtualProtect(at, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 4);
    return true;
}

bool patch_u32(void* at_, uint32_t v) {
    uint8_t* at = (uint8_t*)at_;
    uint32_t orig = 0;
    if (!safe_read_u32(at, &orig)) return false;
    if (!write_u32(at, v)) return false;
    if (g_npatch < (int)(sizeof(g_patch) / sizeof(g_patch[0]))) g_patch[g_npatch++] = { at, orig, v };
    return true;
}

// Newest first, so chained patches unwind in order. A site whose bytes are no longer ours (module unloaded and
// reused, or patched again by someone else) is left alone.
void patch_revert_all() {
    int done = 0, skipped = 0;
    for (int i = g_npatch - 1; i >= 0; i--) {
        uint32_t cur = 0;
        if (!safe_read_u32(g_patch[i].at, &cur) || cur != g_patch[i].now || !write_u32(g_patch[i].at, g_patch[i].orig)) {
            skipped++;
            continue;
        }
        done++;
    }
    log_line("patches: %d reverted, %d skipped", done, skipped);
    g_npatch = 0;
}

bool hooks_install_mainloop() {
    uint8_t* site = g_mainloop_callsite;
    if (!site || !g_sg_mainloop) return false;
    int32_t want_rel = (int32_t)(g_sg_mainloop - (site + 5));
    if (site[0] != 0xE8 || *(int32_t*)(site + 1) != want_rel) {
        log_line("hook mainloop: call site %p changed under us - NOT installed", site);
        return false;
    }
    g_mainloop_target = g_sg_mainloop;
    if (!patch_u32(site + 1, (int32_t)((uint8_t*)&sg_mainloop_wrapper - (site + 5)))) {
        log_line("hook mainloop: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    log_line("hook mainloop: ON - CALL at gva=0x%08X re-pointed to wrapper %p; Sg_MainLoop gva=0x%08X",
             rt_to_gva(site), (void*)&sg_mainloop_wrapper, rt_to_gva(g_sg_mainloop));
    return true;
}

// Re-points one verified `CALL old_target` to new_target (one aligned rel32 write). The callee keeps the convention.
bool patch_call_rel32(uint8_t* site, void* old_target, void* new_target) {
    if (!site || site[0] != 0xE8 || call_target(site) != (uint8_t*)old_target) return false;
    return patch_u32(site + 1, (int32_t)((uint8_t*)new_target - (site + 5)));
}

static bool mh_ready() {
    static int state = 0;                        // 0 = not tried, 1 = ready, 2 = failed
    if (state == 0) {
        MH_STATUS s = MH_Initialize();
        state = (s == MH_OK || s == MH_ERROR_ALREADY_INITIALIZED) ? 1 : 2;
        if (state == 2) log_line("hook: MH_Initialize failed (%s)", MH_StatusToString(s));
    }
    return state == 1;
}

bool hook_entry(const char* name, void* target, void* detour, void** orig) {
    if (!mh_ready()) return false;
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) {
        log_line("hook %s: MinHook at gva=0x%08X failed (%s)", name, rt_to_gva(target), MH_StatusToString(s));
        MH_RemoveHook(target);
        return false;
    }
    return true;
}

void hook_remove(void* target) {
    if (!mh_ready()) return;
    MH_DisableHook(target);
    MH_RemoveHook(target);
}

void hook_disable_all() {
    if (!mh_ready()) return;
    MH_STATUS s = MH_DisableHook(MH_ALL_HOOKS);
    log_line("hooks: all entry hooks disabled (%s)", MH_StatusToString(s));
}

static set_divider_t g_divider_tramp = nullptr;

static void __cdecl h_set_divider(uint32_t req) {
    bool swallow = false;
    if (!g_disabled) {
        __try { swallow = pacing_divider_request(req); }
        __except (EXCEPTION_EXECUTE_HANDLER) { swallow = false; on_fault("divider hook", GetExceptionCode()); }
    }
    if (!swallow) g_divider_tramp(req);
}

void hooks_install_divider_on_game_thread() {
    static bool attempted = false;
    if (attempted) return;
    attempted = true;
    if (!g_set_divider || !g_divider) {
        log_line("hook divider: SKS_SetFrameDivider not resolved - pacing stays stock");
        return;
    }
    void* entry = (void*)g_set_divider;
    if (!hook_entry("divider", entry, (void*)&h_set_divider, (void**)&g_divider_tramp)) {
        log_line("hook divider: NOT installed, pacing stays stock");
        return;
    }
    g_set_divider = g_divider_tramp;             // our own calls bypass the hook
    g_divider_hooked = true;
    log_line("hook divider: ON - SKS_SetFrameDivider gva=0x%08X hooked, divider now %u", rt_to_gva(entry), *g_divider);
}

} // namespace x2
