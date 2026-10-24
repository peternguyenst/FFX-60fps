// SPDX-License-Identifier: Apache-2.0
// X2-MAG-006 (port of FFX MAG-006 v3+v4): particle programs (Ppp_ProgramStep FUN_007fed00) step on every draw, so
// at 60 fps they run 2x. On render-only invocations DLL callers (service table +0xB00) step with the engine's pause
// flag 0x01CF579C set: the particles draw but their clock, age and ops don't advance. EXE callers keep stock
// behaviour (pausing them crashed in FFX), except call sites other rows list (mag006_pause_exe_caller). A program that never advanced (+8 == 0 && +0xC == 0) steps unpaused
// (FFX v4 FRESH: a paused first step draws unset state).
// FFX-2 only: the step's first call FUN_006ba090 (E8 at entry+8) can clear the pause flag for the loaded magic
// overlay (DAT_00e20b24 && DAT_00e10398); it is skipped while this row pauses a step.
// Open (H, PORT-B §2): FFX-2's start-delay clock prog+0x78 (FUN_00804870) runs 2x for FRESH programs.
// Evidence: outer repo re/ledger/PORT-B.md §2 (S).
#include "../x2.h"
#include <intrin.h>
#include <stdlib.h>
#include <string.h>

namespace x2 {

typedef int(__cdecl* program_step_fn)(void* prog, uint32_t flags);   // bool cdecl(prog, flags), plain RET
typedef void(__cdecl* pause_override_fn)(void* prog);

static uint8_t*   g_program_step = nullptr;
static program_step_fn g_program_step_orig = nullptr;
static pause_override_fn g_pause_override = nullptr;
static volatile int32_t* g_pause_flag = nullptr;
static bool       g_patches_done = false;
static int        g_opt_fresh = 1;                // [rows] X2-MAG-006-FRESH
static int        g_opt_fresh2 = 1;                  // [rows] X2-MAG-006-FRESH2
static volatile LONG g_fresh2_calls = 0;
static int        g_in_paused_step = 0;                 // game thread: this row is inside a paused step
static volatile LONG g_paused_calls = 0, g_exe_calls = 0, g_fresh_calls = 0, g_override_skipped = 0;
extern Row row_MAG006;
static void*  g_step_prog = nullptr;
static int    g_step_caller = 0;                     // 0 none, 1 DLL caller, 2 EXE caller, 3 field step FUN_007fef20

// EXE call sites paused like DLL callers (return addresses; X2-MAG-026 adds the family-X record step).
static uint8_t* g_paused_exe_callers[4];
static int      g_paused_exe_caller_count = 0;
static volatile LONG g_exe_paused_calls = 0;
uint8_t* mag006_entry() { return row_MAG006.on ? g_program_step : nullptr; }
bool mag006_pause_exe_caller(uint8_t* ret) {
    if (g_paused_exe_caller_count >= 4) return false;
    g_paused_exe_callers[g_paused_exe_caller_count++] = ret;
    return true;
}
static bool is_paused_exe_caller(const void* ret) {
    for (int i = 0; i < g_paused_exe_caller_count; i++) if (g_paused_exe_callers[i] == ret) return true;
    return false;
}

static void __cdecl hook_pause_override(void* prog) {
    if (g_in_paused_step) { InterlockedIncrement(&g_override_skipped); return; }
    g_pause_override(prog);
}

// Test switch (F11, default off): DLL-caller steps on render-only invocations run with the pause flag CLEAR, overriding
// the pause a DLL sets itself (magic_0718's draw entry writes +0xBF0 = (exec==0)). Effects then step 2x; a blink that
// stops tells that it lives on the paused path (U 16/10/2026: mission-start hex grid).
static volatile LONG g_f11_unpause_test = 0, g_unpaused_calls = 0;

// X2-FX-010-LOOPDEFER (port of FFX FX-010-LOOPDEFER, default 1; U 16/10/2026: mission-start hex grid blinks, stops with
// the F11 unpause test). At the end of an unpaused step the group loop FUN_00805b80 (and its twin FUN_00806130) restarts
// looping particles that reached their end: age = loop start, FUN_007fff80 resets every op of the particle to its first
// key and runs the ops' init callbacks (birth state). Stock draws nothing until the next tick's ops have run on it; the
// paused render-only step in between drew the restarted particles in their raw birth state. Fix: under 60 Hz pacing every
// restart is queued and applied at the start of that program's next unpaused step, before its ops (stock order); see
// run_step. Dropped if the program ends or the particle left its list. Covers EXE-called programs too (they run
// paused whenever a DLL left the pause flag set). Sites 0x8060A3 / 0x8063EF (CALL FUN_007fff80, cdecl(particle), plain
// RET 0x7FFFD9, caller ADD ESP,4).
typedef void(__cdecl* restart_fn)(void*);
static restart_fn g_particle_restart = nullptr;
static uint8_t*  g_restart_callsite[2] = {};
static int       g_opt_loopdefer = 0;
static void*     g_tracked_prog = nullptr;         // DLL-called program in a logic step now (game thread)
struct DeferredRestart { void* prog; void* part; };
enum { MAX_DEFERRED_RESTARTS = 2048 };
static DeferredRestart  g_deferred_restarts[MAX_DEFERRED_RESTARTS];
static int       g_deferred_restart_count = 0;
static volatile LONG g_restarts_deferred = 0, g_restarts_applied = 0, g_restarts_dropped = 0, g_restarts_queue_full = 0;

static void __cdecl hook_particle_restart(void* part) {
    if (g_tracked_prog && g_deferred_restart_count < MAX_DEFERRED_RESTARTS) {
        g_deferred_restarts[g_deferred_restart_count++] = { g_tracked_prog, part };
        InterlockedIncrement(&g_restarts_deferred);
        return;
    }
    if (g_tracked_prog) InterlockedIncrement(&g_restarts_queue_full);
    g_particle_restart(part);
}

// the particle is still on the program's particle list (+0x30, next at +0)
static bool part_in_prog(uint8_t* prog, void* part) {
    __try {
        uint8_t* p = *(uint8_t**)(prog + 0x30);
        for (int k = 0; p && k < 8192; k++, p = *(uint8_t**)p)
            if (p == part) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static void apply_deferred_restarts(void* prog, bool drop) {
    int w = 0;
    for (int i = 0; i < g_deferred_restart_count; i++) {
        DeferredRestart d = g_deferred_restarts[i];
        if (d.prog != prog) { g_deferred_restarts[w++] = d; continue; }
        if (!drop && part_in_prog((uint8_t*)prog, d.part)) { g_particle_restart(d.part); InterlockedIncrement(&g_restarts_applied); }
        else InterlockedIncrement(&g_restarts_dropped);
    }
    g_deferred_restart_count = w;
}

// One step with LOOPDEFER: restarts made by this step are queued; queued restarts are applied at the start of the
// program's next UNPAUSED step (its ops then run on them, the stock order). A paused step keeps them queued: EXE-called
// programs (field particles, e.g. the pyrefly river) also run paused on render-only frames whenever a magic DLL left the
// pause flag set (magic_0718's draw entry writes it per draw; U 16/10/2026: river blinks ~1/s on the mission screen).
// X2-FX-011-FREEDEFER (default 0: did not help the glow, U 17/10; U 17/10/2026: Full Throttle light-ball glow blinks, F11 fixes it; re/ledger/LENS-GLOW.md).
// The end-of-step tail of the group loops (unpaused steps only, gate 0x80605D) frees a particle whose age +0xC reached
// its life (0x806118 / twin 0x806556: `push 0; push esi; mov [edx],edi; call FUN_00804b80`). Particles that live one
// tick are emitted, drawn and freed inside one unpaused step, so the paused render-only step that follows has nothing
// to draw (pppKeShpTail3X / pppKeLns* 0 paused calls). Under 60 Hz pacing the free is deferred: the particle stays on the
// program's list with age -= 0x1000 (the age it was drawn at), the paused step draws it unchanged (paused steps spawn,
// age and free nothing), and it is unlinked and freed at the start of the program's next unpaused step, before that
// step's spawn and ops (the stock order). If that empties a one-shot program (+0x1D), the step is not run and reports the
// end (frees_end_program). (First build skipped one-shot last particles: the glow is one; U 17/10 13:19 still blinked.)
typedef void(__cdecl* particle_free_fn)(void* part, int flag);
static particle_free_fn   g_particle_free = nullptr;               // FUN_00804b80
static int       g_opt_freedefer = 0;
static uint8_t*  g_free_callsite[2] = {};
static void*     g_free_keep_target[2] = {};             // 0x8060AB / 0x8063F7
static void*     g_free_after_target[2] = {};            // 0x8060B0 / 0x8063FC (after the original free)
struct DeferredFree { void* prog; void* part; };
enum { MAX_DEFERRED_FREES = 2048 };
static DeferredFree     g_deferred_frees[MAX_DEFERRED_FREES];
static int       g_deferred_free_count = 0;
static volatile LONG g_frees_deferred = 0, g_frees_done = 0, g_frees_dropped = 0;
static volatile LONG g_free_defer_result = 0;

static int __cdecl free_defer(uint8_t* part, uint8_t* prog) {
    if (!g_opt_freedefer || !row_MAG006.on || g_disabled || pacing_kind() == INV_STOCK || g_deferred_free_count >= MAX_DEFERRED_FREES) return 0;
    if (g_tracked_prog != prog) return 0;          // only inside a tracked step (it flushes the queue at the next one)
    g_deferred_frees[g_deferred_free_count++] = { prog, part };
    InterlockedIncrement(&g_frees_deferred);
    return 1;
}

static int apply_deferred_frees(void* prog, bool drop) {
    int w = 0, freed = 0;
    for (int i = 0; i < g_deferred_free_count; i++) {
        DeferredFree q = g_deferred_frees[i];
        if (q.prog != prog) { g_deferred_frees[w++] = q; continue; }
        bool done = false;
        if (!drop) {
            __try {
                uint8_t** prev = (uint8_t**)((uint8_t*)prog + 0x30);
                for (int k = 0; *prev && k < 8192; k++) {
                    if (*prev == (uint8_t*)q.part) {
                        *prev = *(uint8_t**)q.part;          // unlink (the original `mov [edx],edi`)
                        g_particle_free(q.part, 0);
                        done = true;
                        break;
                    }
                    prev = (uint8_t**)*prev;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        InterlockedIncrement(done ? &g_frees_done : &g_frees_dropped);
        if (done) freed++;
    }
    g_deferred_free_count = w;
    return freed;
}

// A one-shot program (+0x1D) ends when its list is empty after a step (Ppp_ProgramStep tail `return prog+0x30 == 0`).
// When the deferred free of its last particle empties the list, the step that would have reported the end has already
// returned "running"; report the end now, without running another step (no spawn in a step stock never ran).
static volatile LONG g_oneshot_ends = 0;
static bool frees_end_program(void* prog) {
    if (!apply_deferred_frees(prog, false)) return false;
    __try {
        uint8_t* pr = (uint8_t*)prog;
        if (pr[0x1D] && *(void**)(pr + 0x30) == nullptr) { InterlockedIncrement(&g_oneshot_ends); return true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

#define DEFINE_FREE_HOOK(N)                                                                          __declspec(naked) static void hook_free##N() {                                                       __asm { pushad }                                                                              __asm { push dword ptr [ebp + 8] }                                                            __asm { push esi }                                                                            __asm { call free_defer }                                                                     __asm { add esp, 8 }                                                                          __asm { mov g_free_defer_result, eax }                                                                   __asm { popad }                                                                               __asm { cmp g_free_defer_result, 0 }                                                                     __asm { je replay }                                                                           __asm { sub dword ptr [esi + 0xC], 0x1000 }                                                   __asm { jmp dword ptr [g_free_keep_target + N * 4] }                                                 __asm { replay: }                                                                             __asm { mov dword ptr [edx], edi }                                                            __asm { push 0 }                                                                              __asm { push esi }                                                                            __asm { call dword ptr [g_particle_free] }                                                            __asm { mov edx, dword ptr [ebp - 0x20] }                                                     __asm { add esp, 8 }                                                                          __asm { jmp dword ptr [g_free_after_target + N * 4] }                                            }
DEFINE_FREE_HOOK(0)
DEFINE_FREE_HOOK(1)

static int run_step(void* prog, uint32_t flags, bool will_pause, program_step_fn step = nullptr) {
    if (!step) step = g_program_step_orig;
    if (g_deferred_free_count && (!will_pause || pacing_kind() == INV_STOCK)) {               // deferred frees first
        if (step == g_program_step_orig) {                                             // Ppp_ProgramStep: nonzero = ended
            if (frees_end_program(prog)) { if (g_deferred_restart_count) apply_deferred_restarts(prog, true); return 1; }
        }
        else apply_deferred_frees(prog, false);                                      // field step: return value not proven
    }
    if (!g_opt_loopdefer || pacing_kind() == INV_STOCK) {
        if (g_deferred_restart_count) apply_deferred_restarts(prog, false);           // stock step (e.g. a menu holds pacing): apply first
        int r0 = step(prog, flags);
        if (r0 && g_deferred_free_count) apply_deferred_frees(prog, true);
        return r0;
    }
    if (g_deferred_restart_count && !will_pause) apply_deferred_restarts(prog, false);
    void* outer = g_tracked_prog;
    g_tracked_prog = prog;
    int r = step(prog, flags);
    g_tracked_prog = outer;
    if (r && g_deferred_restart_count) apply_deferred_restarts(prog, true);           // program ended
    if (r && g_deferred_free_count) apply_deferred_frees(prog, true);                      // its kill frees the kept particles
    return r;
}

// Field particle programs (pyrefly river, map effects) step through FUN_007fef20 (FFX FUN_007122f0: field loop
// FUN_00810190 / FUN_00810F90 -> FUN_007fef20(prog, 0x30) -> group loop FUN_00806130, restart site 0x8063EF), not
// Ppp_ProgramStep, on every invocation, with the pause flag as left by others (magic_0718 sets it per draw). Same
// deferral (U 16/10/2026: river still blinked with only Ppp_ProgramStep covered; D: "EXE callers passed 0").
static uint8_t*   g_field_step = nullptr;
static program_step_fn g_field_step_orig = nullptr;
static int __cdecl hook_field_step(void* prog, uint32_t flags) {
    if (!row_MAG006.on || g_disabled) return g_field_step_orig(prog, flags);
    void* outer = g_step_prog; int outer_cls = g_step_caller;
    g_step_prog = prog; g_step_caller = 3;
    int r = run_step(prog, flags, *g_pause_flag != 0, g_field_step_orig);
    g_step_prog = outer; g_step_caller = outer_cls;
    return r;
}

// Diagnostic (logged with FREEDEFER's line): steps from FUN_006a5f80 (return 0x6A5FD7, effect type 0xD4FA98 slot +8,
// Full Throttle's light-ball program) per invocation kind and pause value (U 17/10: glow ops never reached paused).
static volatile LONG g_diag_6a5f_steps[4][3];                // [kind][pause: 0 / 1 / other]
static uint32_t g_diag_6a5f_ret = 0x6A5FD7 - 0x400000;   // offset; compared against the live return address
// X2-MAG-006-UNPAUSE-LIST (default "" = off; U 17/10: "681" stopped the blink but the effect looked skipped): magic DLLs whose own particle steps run UNPAUSED on render-only invocations
// (the F11 test, per DLL). U 17/10/2026: Full Throttle (magic_0681) light-ball glow blinks with paused steps and not
// with F11; the exact paused-step gate is not mapped (re/ledger/LENS-GLOW.md). Cost: that DLL's particle programs step
// on every invocation (up to 2x their animation rate).
enum { MAX_UNPAUSE_LIST = 16, MAX_UNPAUSE_DLLS = 8 };
static int      g_unpause_list[MAX_UNPAUSE_LIST];
static int      g_unpause_list_count = 0;
static struct { uint8_t* base; uint32_t size; } g_unpause_dlls[MAX_UNPAUSE_DLLS];
static bool in_unpause_dll(const void* ret) {
    for (int i = 0; i < MAX_UNPAUSE_DLLS; i++)
        if (g_unpause_dlls[i].base && (const uint8_t*)ret >= g_unpause_dlls[i].base && (const uint8_t*)ret < g_unpause_dlls[i].base + g_unpause_dlls[i].size)
            return true;
    return false;
}
// X2-MAG-006-UNPAUSE-AB (default 0 = off; user 23/10/2026: Ultima cannot be cast on demand, one cast must answer):
// magic DLL number whose own render-only particle steps run unpaused in alternate seconds (odd GetTickCount seconds
// on), each switch logged, so X2-DIAG-LUMA compares both halves of one cast (ULTIMA-0106.md).
static int      g_ab_dll_number = 0;
static uint8_t* g_ab_dll_base = nullptr;
static uint32_t g_ab_dll_size = 0;
static int      g_ab_second = -1;
static bool ab_unpaused(const void* ret) {
    if (!g_ab_dll_base || (const uint8_t*)ret < g_ab_dll_base || (const uint8_t*)ret >= g_ab_dll_base + g_ab_dll_size) return false;
    int seg = (int)((GetTickCount() / 1000) & 1);
    if (seg != g_ab_second) {
        g_ab_second = seg;
        log_line("X2-MAG-006-UNPAUSE-AB: magic_%04d render-only steps %s", g_ab_dll_number, seg ? "UNPAUSED" : "paused");
    }
    return seg != 0;
}
static void unpause_on_load(uint8_t* base, uint32_t size, const char* name) {
    const char* p = strstr(name, "magic_");
    int n = p ? atoi(p + 6) : -1;
    if (g_ab_dll_number && n == g_ab_dll_number) {
        g_ab_dll_base = base; g_ab_dll_size = size; g_ab_second = -1;
        log_line("X2-MAG-006-UNPAUSE-AB: %s loaded - unpaused in alternate seconds", name);
    }
    for (int i = 0; i < g_unpause_list_count; i++)
        if (g_unpause_list[i] == n)
            for (int k = 0; k < MAX_UNPAUSE_DLLS; k++)
                if (!g_unpause_dlls[k].base) {
                    g_unpause_dlls[k].size = size; g_unpause_dlls[k].base = base;
                    log_line("X2-MAG-006-UNPAUSE-LIST: %s particle steps unpaused on render-only invocations", name);
                    return;
                }
}
static void unpause_on_unload(uint8_t* base) {
    if (base == g_ab_dll_base) g_ab_dll_base = nullptr;
    for (int k = 0; k < MAX_UNPAUSE_DLLS; k++) if (g_unpause_dlls[k].base == base) g_unpause_dlls[k].base = nullptr;
}

// X2-DIAG-PPPOPS: the program of the step running now and its caller class (game thread), plus a per-step callback
// (program, caller class, particles on +0x30 before / after, return value), also at the step start with after == -1.
static step_note_fn g_step_note = nullptr;
void* mag006_cur_step(int* cls) { if (cls) *cls = g_step_caller; return g_step_prog; }
void  mag006_set_step_note(step_note_fn fn) { g_step_note = fn; }
static int count_parts(void* prog) {
    int n = 0;
    __try {
        for (uint8_t* p = *(uint8_t**)((uint8_t*)prog + 0x30); p && n < 8192; p = *(uint8_t**)p) n++;
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = -1; }
    return n;
}
static int program_step_body(void* prog, uint32_t flags, void* ret);
static int __cdecl hook_program_step(void* prog, uint32_t flags) {
    void* ret = _ReturnAddress();
    void* outer = g_step_prog; int outer_cls = g_step_caller;
    g_step_prog = prog; g_step_caller = in_image(ret) ? 2 : 1;
    int before = 0;
    if (g_step_note) { before = count_parts(prog); g_step_note(prog, g_step_caller, before, -1, 0); }   // -1: step starts
    int r = program_step_body(prog, flags, ret);
    if (g_step_note) g_step_note(prog, g_step_caller, before, count_parts(prog), r);
    g_step_prog = outer; g_step_caller = outer_cls;
    return r;
}
static int program_step_body(void* prog, uint32_t flags, void* ret) {
    if (!row_MAG006.on || g_disabled || g_in_paused_step) return g_program_step_orig(prog, flags);
    if ((uint32_t)((uint8_t*)ret - exe_base()) == g_diag_6a5f_ret) {
        int32_t pv = *g_pause_flag;
        InterlockedIncrement(&g_diag_6a5f_steps[pacing_kind() & 3][pv == 0 ? 0 : pv == 1 ? 1 : 2]);
    }
    bool exe = in_image(ret) && !is_paused_exe_caller(ret);    // is_paused_exe_caller EXE callers take the DLL path
    if (!render_only() || exe) {                 // logic steps, and EXE callers (stock pause state)
        if (exe && render_only()) InterlockedIncrement(&g_exe_calls);
        return run_step(prog, flags, *g_pause_flag != 0);
    }
    if (g_f11_unpause_test || in_unpause_dll(ret) || pppops_prog_marked(prog) || ab_unpaused(ret)) {
        InterlockedIncrement(&g_unpaused_calls);
        int32_t saved = *g_pause_flag;
        *g_pause_flag = 0;
        int r = run_step(prog, flags, false);
        *g_pause_flag = saved;
        return r;
    }
    // X2-MAG-006-FRESH2 (default 0: fired once in Full Throttle, did not help, U 17/10): a program re-initialised by its DLL (+0xB08 FUN_007ff070 resets +8 and +0x78, not
    // +0xC) is fresh too; its first step runs with the pause flag CLEAR (the DLL may have written -1 itself), so its
    // emitters spawn. magic_0681 (Full Throttle) re-inits its light-ball program after every finished live step; the
    // paused render-only first step left the glow groups empty = blink (U 17/10/2026; F11 fixed it; LENS-GLOW.md §2).
    if (g_opt_fresh2 && *(int32_t*)((uint8_t*)prog + 8) == 0 && *(int32_t*)((uint8_t*)prog + 0x78) == 0) {
        InterlockedIncrement(&g_fresh2_calls);
        int32_t saved = *g_pause_flag;
        *g_pause_flag = 0;
        int r = run_step(prog, flags, false);
        *g_pause_flag = saved;
        return r;
    }
    if (g_opt_fresh && *(int32_t*)((uint8_t*)prog + 8) == 0 && *(int32_t*)((uint8_t*)prog + 0xC) == 0) {
        InterlockedIncrement(&g_fresh_calls);
        return run_step(prog, flags, *g_pause_flag != 0);
    }
    InterlockedIncrement(&g_paused_calls);
    if (in_image(ret) && InterlockedIncrement(&g_exe_paused_calls) == 1)
        log_line("X2-MAG-006: first paused step from a listed EXE caller (return gva 0x%08X)", rt_to_gva(ret));
    int32_t saved = *g_pause_flag;                   // values written by event scripts survive (FFX MAG-004)
    *g_pause_flag = 1;
    g_in_paused_step = 1;
    int r = g_program_step_orig(prog, flags);
    g_in_paused_step = 0;
    *g_pause_flag = saved;
    return r;
}

static bool install() {
    // Ppp_ProgramStep: PUSH ESI; CALL override(prog); ADD ESP,4; CMP [..],0; JNZ; PUSH ESI; CALL start-delay
    g_program_step = sig_find("X2-MAG-006 Ppp_ProgramStep",
        "55 8B EC 56 8B 75 08 56 E8 ?? ?? ?? ?? 83 C4 04 83 3D ?? ?? ?? ?? 00 75 20 56 E8", 0x7FED00);
    if (!g_program_step) return false;
    // the step's own pause test: CMP dword [flag],0 at entry+0x1BF
    if (!sig_match(g_program_step + 0x1BF, "83 3D ?? ?? ?? ?? 00 5F 75 3F 80 7E 1C 00")) {
        log_line("  X2-MAG-006: pause CMP not at entry+0x1BF - refused");
        return false;
    }
    g_pause_flag = (volatile int32_t*)abs_operand("particle pause flag", g_program_step + 0x1BF + 2);
    if (!g_pause_flag) return false;
    // override FUN_006ba090: if (DAT_00e20b24) flag &= ~-(DAT_00e10398 != 0)
    uint8_t* ov = call_target(g_program_step + 8);
    if (!ov || !in_image(ov)) return false;
    g_pause_override = (pause_override_fn)ov;
    g_opt_fresh = config_row("X2-MAG-006-FRESH", 1);
    g_opt_fresh2 = config_row("X2-MAG-006-FRESH2", 0);
    {
        char buf[128];
        config_str("rows", "X2-MAG-006-UNPAUSE-LIST", buf, sizeof(buf), "");
        for (char* t = buf; *t && g_unpause_list_count < MAX_UNPAUSE_LIST;) {
            char* e = nullptr; long v = strtol(t, &e, 10);
            if (e == t) { t++; continue; }
            g_unpause_list[g_unpause_list_count++] = (int)v; t = e;
        }
        g_ab_dll_number = config_row("X2-MAG-006-UNPAUSE-AB", 0);
        if ((g_unpause_list_count || g_ab_dll_number) && !magic_mods_subscribe("X2-MAG-006-UNPAUSE-LIST", unpause_on_load, unpause_on_unload)) g_unpause_list_count = g_ab_dll_number = 0;
        if (g_ab_dll_number) log_line("  X2-MAG-006-UNPAUSE-AB: magic_%04d render-only steps unpaused in alternate seconds", g_ab_dll_number);
    }
    g_opt_loopdefer = config_row("X2-FX-010-LOOPDEFER", 1);
    if (g_opt_loopdefer) {
        // group loop tails: MOV EAX,[ESI+0xC]; CMP EAX,[ECX+0x1C]; JL; MOV EAX,[ECX+0x18]; PUSH ESI; MOV [ESI+0xC],EAX; CALL
        uint8_t* a = sig_find("X2-FX-010-LOOPDEFER restart (FUN_00805b80)",
            "8B 46 0C 3B 41 1C 7C 72 8B 41 18 56 89 46 0C E8 ?? ?? ?? ?? 83 C4 04", 0x806094);
        uint8_t* b = sig_find("X2-FX-010-LOOPDEFER restart (FUN_00806130)",
            "8B 46 0C 3B 41 1C 0F 8C ?? ?? ?? ?? 8B 41 18 56 89 46 0C E8 ?? ?? ?? ?? 83 C4 04", 0x8063DC);
        uint8_t* ta = a ? call_target(a + 0xF) : nullptr;
        uint8_t* tb = b ? call_target(b + 0x13) : nullptr;
        // FUN_007fff80: MOV EAX,[ESP+4]-style cdecl(particle) reset; both sites must call it
        if (!ta || ta != tb || !in_image(ta)) {
            log_line("  X2-FX-010-LOOPDEFER: restart CALLs not found or differ - sub-switch off");
            g_opt_loopdefer = 0;
        } else {
            // field step FUN_007fef20: PUSH EBP; MOV EBP,ESP; FLD1; PUSH EBX; FST [..]; PUSH ESI; FST [..]; PUSH imm; FST; FSTP
            g_field_step = sig_find("X2-FX-010-LOOPDEFER field step FUN_007fef20",
                "55 8B EC D9 E8 53 D9 15 ?? ?? ?? ?? 56 D9 15 ?? ?? ?? ?? 68 ?? ?? ?? ?? D9 15 ?? ?? ?? ?? D9 1D ?? ?? ?? ??",
                0x7FEF20);
            if (!g_field_step) log_line("  X2-FX-010-LOOPDEFER: field step not found - field programs not covered");
            g_particle_restart = (restart_fn)ta;
            g_restart_callsite[0] = a + 0xF;
            g_restart_callsite[1] = b + 0x13;
            log_line("  X2-FX-010-LOOPDEFER: restart gva=0x%08X (0x007FFF80), CALLs gva=0x%08X / 0x%08X (0x008060A3 / 0x008063EF)",
                     rt_to_gva(ta), rt_to_gva(g_restart_callsite[0]), rt_to_gva(g_restart_callsite[1]));
        }
    }
    g_opt_freedefer = g_opt_loopdefer ? config_row("X2-FX-011-FREEDEFER", 0) : 0;   // needs LOOPDEFER's tracked steps
    if (g_opt_freedefer) {
        // tail free blocks: MOV EAX,[ECX+0x14]; OR EBX,EAX; CMP [ESI+0xC],EAX; JB keep; PUSH 0; PUSH ESI; MOV [EDX],EDI;
        // CALL free; MOV EDX,[EBP-0x20]; ADD ESP,8; JMP after
        uint8_t* f0 = sig_find("X2-FX-011-FREEDEFER free block (FUN_00805b80)",
            "8B 41 14 0B D8 39 46 0C 72 93 6A 00 56 89 3A E8 ?? ?? ?? ?? 8B 55 E0 83 C4 08 EB 86", 0x80610E);
        uint8_t* f1 = sig_find("X2-FX-011-FREEDEFER free block (FUN_00806130)",
            "8B 41 14 0B D8 39 46 0C 0F 82 ?? ?? ?? ?? 6A 00 56 89 3A E8 ?? ?? ?? ?? 8B 55 E0 83 C4 08 E9 ?? ?? ?? ??", 0x806548);
        uint8_t* c0 = f0 ? call_target(f0 + 0xF) : nullptr;
        uint8_t* c1 = f1 ? call_target(f1 + 0x13) : nullptr;
        if (!c0 || c0 != c1 || !in_image(c0)) {
            log_line("  X2-FX-011-FREEDEFER: free blocks not found or differ - off");
            g_opt_freedefer = 0;
        } else {
            g_particle_free = (particle_free_fn)c0;
            g_free_callsite[0] = f0 + 0xA;                          // 0x806118
            g_free_callsite[1] = f1 + 0xE;                          // 0x806556
            g_free_keep_target[0] = f0 + 0xA + (int8_t)f0[9];          // JB rel8 target 0x8060AB
            g_free_keep_target[1] = f1 + 0xE + *(int32_t*)(f1 + 10);   // JB rel32 target 0x8063F7
            g_free_after_target[0] = f0 + 0x1C + (int8_t)f0[0x1B];     // JMP rel8 target 0x8060B0
            g_free_after_target[1] = f1 + 0x23 + *(int32_t*)(f1 + 0x1F); // JMP rel32 target 0x8063FC
            if (!sig_match(g_free_keep_target[0], "8B D6 89 55 E0 8B F7") || !sig_match(g_free_keep_target[1], "8B D6 89 55 E0 8B F7") ||
                (uint8_t*)g_free_after_target[0] != (uint8_t*)g_free_keep_target[0] + 5 || (uint8_t*)g_free_after_target[1] != (uint8_t*)g_free_keep_target[1] + 5) {
                log_line("  X2-FX-011-FREEDEFER: keep / after targets not as proven - off");
                g_opt_freedefer = 0;
            } else {
                log_line("  X2-FX-011-FREEDEFER: free sites gva=0x%08X / 0x%08X (0x00806118 / 0x00806556), free gva=0x%08X, keep "
                         "0x%08X / 0x%08X (0x008060AB / 0x008063F7)", rt_to_gva(g_free_callsite[0]), rt_to_gva(g_free_callsite[1]),
                         rt_to_gva(c0), rt_to_gva(g_free_keep_target[0]), rt_to_gva(g_free_keep_target[1]));
            }
        }
    }
    log_line("  X2-MAG-006: Ppp_ProgramStep gva=0x%08X (entry + override CALL patched on the game thread), pause flag gva=0x%08X "
             "(expected 0x01CF579C), override gva=0x%08X skipped while paused, FRESH %s",
             rt_to_gva(g_program_step), rt_to_gva((const void*)g_pause_flag), rt_to_gva(ov), g_opt_fresh ? "ON" : "OFF");
    return true;
}

static void pre() {
    if (g_patches_done) return;
    g_patches_done = true;
    // both patches on the game thread, between invocations: the step never runs while they are written
    if (!patch_call_rel32(g_program_step + 8, (void*)g_pause_override, (void*)&hook_pause_override)) {
        log_line("X2-MAG-006: override CALL patch failed - row off");
        row_MAG006.on = 0;
        return;
    }
    if (!hook_entry("X2-MAG-006", g_program_step, (void*)&hook_program_step, (void**)&g_program_step_orig)) {
        log_line("X2-MAG-006: entry hook failed - row off (the override stub is pass-through alone)");
        row_MAG006.on = 0;
        return;
    }
    if (g_opt_loopdefer) {
        if (!patch_call_rel32(g_restart_callsite[0], (void*)g_particle_restart, (void*)&hook_particle_restart) ||
            !patch_call_rel32(g_restart_callsite[1], (void*)g_particle_restart, (void*)&hook_particle_restart)) {
            patch_call_rel32(g_restart_callsite[0], (void*)&hook_particle_restart, (void*)g_particle_restart);
            log_line("X2-FX-010-LOOPDEFER: restart CALL patch failed - off");
            g_opt_loopdefer = 0;
        } else {
            if (g_field_step && !hook_entry("X2-FX-010-LOOPDEFER field step", g_field_step, (void*)&hook_field_step, (void**)&g_field_step_orig)) {
                log_line("X2-FX-010-LOOPDEFER: field step hook failed - field programs not covered");
                g_field_step = nullptr;
            }
            log_line("X2-FX-010-LOOPDEFER: particle loop restarts deferred to the program's next unpaused step (DLL and EXE "
                     "callers%s)", g_field_step ? ", field step FUN_007fef20" : "");
        }
    }
    if (g_opt_freedefer && g_opt_loopdefer) {
        void* t0 = nullptr; void* t1 = nullptr;
        if (!hook_entry("X2-FX-011-FREEDEFER site 0", g_free_callsite[0], (void*)&hook_free0, &t0)) g_opt_freedefer = 0;
        else if (!hook_entry("X2-FX-011-FREEDEFER site 1", g_free_callsite[1], (void*)&hook_free1, &t1)) {
            hook_remove(g_free_callsite[0]);
            g_opt_freedefer = 0;
        }
        log_line("X2-FX-011-FREEDEFER: %s", g_opt_freedefer ? "particle frees deferred to the program's next unpaused step" : "hook failed - off");
    } else g_opt_freedefer = 0;
}

static void post() {
    static bool down_prev = false;
    bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (down && !down_prev) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            LONG v = g_f11_unpause_test ? 0 : 1;
            InterlockedExchange(&g_f11_unpause_test, v);
            log_line("F11: X2-MAG-006-UNPAUSE-TEST %s (DLL particle steps on render-only frames %s; unpaused so far %ld)",
                     v ? "ON" : "OFF", v ? "UNPAUSED, effects 2x" : "paused as normal", g_unpaused_calls);
        }
    }
    down_prev = down;
    static LONG reported = 0;
    if (!reported && g_paused_calls > 0) {
        reported = 1;
        log_line("X2-MAG-006: first paused DLL particle step on a render-only invocation (EXE callers passed %ld, fresh %ld)",
                 g_exe_calls, g_fresh_calls);
    }
    static DWORD last_un = 0;
    static LONG last_un_n = 0;
    if (GetTickCount() - last_un >= 10000 && g_unpaused_calls != last_un_n) {
        last_un = GetTickCount();
        last_un_n = g_unpaused_calls;
        log_line("X2-MAG-006: render-only DLL steps run unpaused so far %ld (F11 / UNPAUSE-LIST / UNPAUSE-OPS)", g_unpaused_calls);
    }
    static DWORD last_def = 0;
    static LONG last_total = 0;
    DWORD now = GetTickCount();
    if (g_opt_loopdefer && now - last_def >= 10000 && g_restarts_deferred != last_total) {
        last_def = now;
        last_total = g_restarts_deferred;
        log_line("X2-FX-010-LOOPDEFER: deferred %ld applied %ld dropped %ld queue-full %ld (queued now %d)",
                 g_restarts_deferred, g_restarts_applied, g_restarts_dropped, g_restarts_queue_full, g_deferred_restart_count);
    }
    static DWORD last_fq = 0;
    static LONG last_fq_total = 0;
    if (g_opt_freedefer && now - last_fq >= 10000 && g_frees_deferred != last_fq_total) {
        last_fq = now;
        last_fq_total = g_frees_deferred;
        log_line("X2-FX-011-FREEDEFER: deferred %ld freed %ld dropped %ld one-shot ends %ld (queued now %d); FRESH2 unpaused first steps %ld",
                 g_frees_deferred, g_frees_done, g_frees_dropped, g_oneshot_ends, g_deferred_free_count, g_fresh2_calls);
        log_line("X2-MAG-006 diag 0x6A5FD7 steps: LOGIC p0 %ld p1 %ld pX %ld | RENDER p0 %ld p1 %ld pX %ld", g_diag_6a5f_steps[0][0], g_diag_6a5f_steps[0][1],
                 g_diag_6a5f_steps[0][2], g_diag_6a5f_steps[1][0], g_diag_6a5f_steps[1][1], g_diag_6a5f_steps[1][2]);
    }
    static LONG ov_seen = 0;
    if (g_override_skipped && !ov_seen) {
        ov_seen = 1;
        log_line("X2-MAG-006: pause override FUN_006ba090 skipped inside a paused step (first time)");
    }
}

Row row_MAG006 = {
    "X2-MAG-006",
    "DLL particle programs: pause flag on render-only invocations (draw still runs); EXE callers untouched",
    1, install, pre, post, 0
};

} // namespace x2
