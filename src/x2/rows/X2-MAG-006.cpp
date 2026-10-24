// SPDX-License-Identifier: Apache-2.0
// X2-MAG-006: particle effects advance once per game tick, not once per drawn frame.
//
// Background. Sparks, glows, smoke and the like are "particle programs". Each one is stepped by Ppp_ProgramStep
// (FUN_007fed00, cdecl(program, flags)) every time it is drawn: the step spawns, ages and moves the particles and draws
// them. At 60 fps that is twice per tick, so effects would run at double speed. The engine has a global particle
// pause flag (gva 0x01CF579C): while it is set, a step draws the particles but does not advance their clock, age or ops.
//
// The fix. On render-only invocations, steps called by spell DLLs run with the pause flag set: the particles are drawn
// again in place. Steps called from FFX-2.exe itself keep the game's own pause state (pausing them crashed in FFX),
// except call sites another row registers with mag006_pause_exe_caller (X2-MAG-026).
//   FRESH (on): a program that has never advanced steps unpaused, or its first draw shows unset state.
//   FFX-2's step first calls FUN_006ba090, which can clear the pause flag while a spell is loaded; we skip that call
//   inside our paused steps.
//
// Particle restarts and frees (separate rows, sharing this hook):
//   X2-FX-010-LOOPDEFER (on): at the end of an unpaused step, a looping particle that reached its end is restarted
//     in its birth state. The stock game never draws that state (the next tick's ops run on it first), but our
//     paused render-only step did: a blink. The restart is queued and applied at the start of the program's next
//     unpaused step, before its ops. Covers the field particle step (FUN_007fef20) too.
//   X2-FX-011-FREEDEFER (off, did not fix the glow it was written for): a particle that lives one tick is freed at
//     the end of the step that drew it, so the render-only step had nothing to draw; its free is delayed the same way.
// Test and diagnostic switches: F11 (render-only DLL steps unpaused: effects run 2x; tells whether a blink comes from
// the paused path), UNPAUSE-LIST (per spell DLL), UNPAUSE-AB (one DLL unpaused in alternate seconds), FRESH2 (off),
// X2-DIAG-PPPOPS hooks (mag006_cur_step / mag006_set_step_note).
// History, measurements and evidence: re/ledger/PORT-B.md §2 (it also keeps this file's earlier comments), LENS-GLOW.md.
#include "../x2.h"
#include <intrin.h>
#include <stdlib.h>
#include <string.h>

namespace x2 {

typedef int(__cdecl* program_step_fn)(void* prog, uint32_t flags);   // bool cdecl(prog, flags), plain RET
typedef void(__cdecl* pause_override_fn)(void* prog);

static uint8_t*   g_program_step = nullptr;           // Ppp_ProgramStep in FFX-2.exe
static program_step_fn g_program_step_orig = nullptr;  // its original code (MinHook trampoline)
static pause_override_fn g_pause_override = nullptr;  // FUN_006ba090, the step's first call (may clear the pause flag)
static volatile int32_t* g_pause_flag = nullptr;      // the engine's particle pause flag
static bool       g_patches_done = false;
static int        g_opt_fresh = 1;                    // [rows] X2-MAG-006-FRESH
static int        g_opt_fresh2 = 1;                   // [rows] X2-MAG-006-FRESH2 (install reads it, default 0)
static volatile LONG g_fresh2_calls = 0;
static int        g_in_paused_step = 0;               // game thread: inside one of our paused steps
static volatile LONG g_paused_calls = 0, g_exe_calls = 0, g_fresh_calls = 0, g_override_skipped = 0;
extern Row row_MAG006;
// The step running now (for diagnostics): its program and who called it
static void*  g_step_prog = nullptr;
static int    g_step_caller = 0;                     // 0 none, 1 DLL caller, 2 EXE caller, 3 field step FUN_007fef20

// Game call sites that are paused like DLL callers (their return addresses; X2-MAG-026 registers one).
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

// Replaces the step's call of FUN_006ba090: skipped inside our paused steps, so it cannot clear our pause.
static void __cdecl hook_pause_override(void* prog) {
    if (g_in_paused_step) { InterlockedIncrement(&g_override_skipped); return; }
    g_pause_override(prog);
}

// F11 test switch (off): render-only DLL steps run with the pause flag CLEAR, even where a spell DLL sets it itself.
// Effects then run 2x; if a blink stops, it comes from the paused path.
static volatile LONG g_f11_unpause_test = 0, g_unpaused_calls = 0;

// --- X2-FX-010-LOOPDEFER: restart looping particles at the stock moment ------------------------------------------------
// At the end of an unpaused step the particle group loops (FUN_00805b80 and its twin FUN_00806130) restart a looping
// particle that reached its end: FUN_007fff80 puts it back to its birth state. The stock game never draws that state
// (the next tick's ops change it first); our paused render-only step in between drew it. So under 60 Hz pacing each
// restart is queued and applied at the start of the program's next unpaused step, before its ops (the stock order;
// see run_step). Dropped if the program ends or the particle has left its list. Also covers game-called programs:
// they run paused too whenever a spell DLL has left the pause flag set. CALL sites 0x8060A3 / 0x8063EF.
typedef void(__cdecl* restart_fn)(void*);
static restart_fn g_particle_restart = nullptr;     // FUN_007fff80
static uint8_t*  g_restart_callsite[2] = {};
static int       g_opt_loopdefer = 0;
static void*     g_tracked_prog = nullptr;         // program whose step is collecting restarts now (game thread)
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

// Applies (or with drop, discards) the queued restarts of one program; the others stay queued.
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

// --- X2-FX-011-FREEDEFER (off by default): free one-tick particles at the stock moment -----------------------------
// The group loops free a particle whose age (+0xC) reached its life at the end of an unpaused step (sites 0x806118 /
// 0x806556: `push 0; push esi; mov [edx],edi; call FUN_00804b80`). A particle that lives one tick is born, drawn and
// freed inside one step, so the paused render-only step after it has nothing to draw. With this on, the free is
// delayed: the particle stays on the list with its age moved back one tick (the age it was drawn at), the paused step
// draws it again, and it is unlinked and freed at the start of the program's next unpaused step, before that step's
// spawns and ops. If that empties a one-shot program (+0x1D), the step reports the program's end instead of running.
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

// Called from the free hook: 1 = queue this free (the hook keeps the particle), 0 = free it now as the game would.
static int __cdecl free_defer(uint8_t* part, uint8_t* prog) {
    if (!g_opt_freedefer || !row_MAG006.on || g_disabled || pacing_kind() == INV_STOCK || g_deferred_free_count >= MAX_DEFERRED_FREES) return 0;
    if (g_tracked_prog != prog) return 0;          // only inside a tracked step (it flushes the queue at the next one)
    g_deferred_frees[g_deferred_free_count++] = { prog, part };
    InterlockedIncrement(&g_frees_deferred);
    return 1;
}

// Unlinks and frees (or with drop, forgets) the queued frees of one program; returns how many were freed.
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

// A one-shot program (+0x1D) ends when its particle list is empty after a step. If a delayed free empties the list,
// the step that should have reported the end already returned "running": report it now, without running another step
// (which would spawn particles in a step the stock game never ran).
static volatile LONG g_oneshot_ends = 0;
static bool frees_end_program(void* prog) {
    if (!apply_deferred_frees(prog, false)) return false;
    __try {
        uint8_t* pr = (uint8_t*)prog;
        if (pr[0x1D] && *(void**)(pr + 0x30) == nullptr) { InterlockedIncrement(&g_oneshot_ends); return true; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// The free hook, one copy per site (N = 0 / 1). It replaces `push 0; push esi; mov [edx],edi; call free` (ESI =
// particle, [EBP+8] = program). Queued: age back one tick, continue where the game goes when the particle lives on.
// Not queued: do exactly what the replaced instructions did, then continue after them.
#define DEFINE_FREE_HOOK(N)                                                     \
__declspec(naked) static void hook_free##N() {                                  \
    __asm { pushad }                                                            \
    __asm { push dword ptr [ebp + 8] }                                          \
    __asm { push esi }                                                          \
    __asm { call free_defer }                                                   \
    __asm { add esp, 8 }                                                        \
    __asm { mov g_free_defer_result, eax }                                      \
    __asm { popad }                                                             \
    __asm { cmp g_free_defer_result, 0 }                                        \
    __asm { je replay }                                                         \
    __asm { sub dword ptr [esi + 0xC], 0x1000 }                                 \
    __asm { jmp dword ptr [g_free_keep_target + N * 4] }                        \
    __asm { replay: }                                                           \
    __asm { mov dword ptr [edx], edi }                                          \
    __asm { push 0 }                                                            \
    __asm { push esi }                                                          \
    __asm { call dword ptr [g_particle_free] }                                  \
    __asm { mov edx, dword ptr [ebp - 0x20] }                                   \
    __asm { add esp, 8 }                                                        \
    __asm { jmp dword ptr [g_free_after_target + N * 4] }                       \
}
DEFINE_FREE_HOOK(0)
DEFINE_FREE_HOOK(1)

// --- One step, with LOOPDEFER / FREEDEFER around it ---------------------------------------------------------------------
// will_pause: the pause flag will be set during this step. Queued frees and restarts of this program are applied only
// before an unpaused step; restarts made by this step are queued (g_tracked_prog). Without 60 Hz pacing (a menu holds
// stock pacing) everything queued is applied first and the step runs as stock. Returns the step's result (nonzero =
// the program ended; queued work for it is then dropped).
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

// Field particle programs (the pyrefly river, map effects) are stepped by the field loop through FUN_007fef20, not
// Ppp_ProgramStep, on every invocation, with the pause flag as others left it. They get the same LOOPDEFER treatment.
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

// Diagnostic (logged with FREEDEFER's line): steps called from 0x6A5FD7 (Full Throttle's light-ball program), counted
// per invocation kind and pause flag value.
static volatile LONG g_diag_6a5f_steps[4][3];                // [kind][pause: 0 / 1 / other]
static uint32_t g_diag_6a5f_ret = 0x6A5FD7 - 0x400000;   // offset; compared against the live return address
// X2-MAG-006-UNPAUSE-LIST (default empty): spell DLL numbers whose own particle steps run UNPAUSED on render-only
// invocations, i.e. the F11 test for one DLL. Cost: that DLL's particles then run up to 2x their speed.
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
// X2-MAG-006-UNPAUSE-AB (default 0 = off): one spell DLL number whose render-only particle steps run unpaused in
// alternate seconds, each switch logged, so a single cast shows both behaviours (for effects that cannot be cast on
// demand).
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
// --- The step hook ---------------------------------------------------------------------------------------------------------
// Replaces Ppp_ProgramStep's entry. The return address tells who called: inside FFX-2.exe = the game, else a spell DLL.
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
// Logic step or game caller: as stock. Render-only DLL step: unpaused if a test switch says so or the program is fresh,
// else paused (the particles are drawn again without advancing).
static int program_step_body(void* prog, uint32_t flags, void* ret) {
    if (!row_MAG006.on || g_disabled || g_in_paused_step) return g_program_step_orig(prog, flags);
    if ((uint32_t)((uint8_t*)ret - exe_base()) == g_diag_6a5f_ret) {
        int32_t pv = *g_pause_flag;
        InterlockedIncrement(&g_diag_6a5f_steps[pacing_kind() & 3][pv == 0 ? 0 : pv == 1 ? 1 : 2]);
    }
    bool exe = in_image(ret) && !is_paused_exe_caller(ret);    // registered game call sites take the DLL path
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
    // FRESH2 (off): a program its DLL has re-initialised (+8 and +0x78 reset, +0xC not) counts as fresh too.
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
    int32_t saved = *g_pause_flag;                   // put back afterwards: event scripts write the flag too
    *g_pause_flag = 1;
    g_in_paused_step = 1;
    int r = g_program_step_orig(prog, flags);
    g_in_paused_step = 0;
    *g_pause_flag = saved;
    return r;
}

// --- Install: find and check every address; patches go in at the first pre() ----------------------------------------------
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

// --- Every invocation ------------------------------------------------------------------------------------------------------
// First pre(), on the game thread: the override CALL patch, the step hook, then the LOOPDEFER / FREEDEFER patches.
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

// After every invocation: the F11 test key (only while the game window is in front) and the periodic log lines.
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
