// SPDX-License-Identifier: Apache-2.0
// X2-MAG-025: battle effects run their scripts once per game tick, not once per drawn frame.
//
// Background. A battle effect is a set of "units", each running a small bytecode program (the game calls it OPU) in
// up to 4 parallel "streams". The game's walker (FUN_006bbca0) steps every unit once: for each stream, if its WAIT
// counter is above 0 it counts down, otherwise the stream runs opcode handlers until one yields; afterwards the unit's
// position is integrated. A walk is "pass 0" (update) or "pass 1" (draw). It is called from the game (the effect
// dispatch: pass 0 and pass 1, every invocation) and by 310 spell DLLs from their draw function.
//
// The problem. At 60 fps the walks also run on render-only invocations, so WAIT counters, script handlers (battle
// signals, motion requests) and unit movement all ran at double speed.
//
// The fix. On render-only invocations a walk runs "held", in one of three modes:
//   1  skip every stream step (the unit's follow call still runs)
//   2  keep every WAIT counter; streams at WAIT 0 still run their handlers
//   3  as 2, but a WAIT-0 stream runs only if it "repeats": its last logic walk ran handlers and yielded at the same
//      opcode it started from. That is a per-frame draw or poll loop, which must run every frame or it blinks.
// In every mode the position integration is skipped (X2-MAG-025-POS), and opcodes listed in X2-MAG-025-DENY never run
// on a held walk (default 0x1F loop counter, 0x7F battle signal): the stream yields at them and the next logic walk
// runs them.
//
// Which walk gets which mode ([rows] keys):
//   game pass-1 walk                MODE (3)
//   game pass-0 walk                PASS0MODE (1), with EXEPASS0 on (it only runs on render-only frames when
//                                   X2-MAG-008 passes the effect dispatch)
//   spell DLL walks                 DLLMODE (3); pass 0 only with PASS0, or under the list profile
//   list profile                    while X2-MAG-008 passes a DLL from its PASS-LIST: LISTMODE (2) for DLL walks, and
//                                   DLL pass-0 walks are held too (DLLPASS0MODE, 0 = same as the DLL mode)
// LOOP1F (on): many draw loops are built on the 0x1F loop counter, which is in DENY. On a held pass-1 walk such a loop
//   jumps back without counting, so it still draws; its last count yields as usual. Each loop jumps back at most once
//   per held walk (a loop with no yield inside would never end) and never on pass-0 walks (a spawn loop would spawn
//   extra particles: that overflowed magic_0946's particle table).
// FOLLOWGUARD (on): the game's attach-follow handler reads [target+0xBC] without a check; when it is null the follow
//   is skipped instead of crashing.
// Test and diagnostic keys: ATTACHDENY, DLL0DENY (more held opcodes), OPDIAG (handler counts), OPTRACE, SLOWTEST /
//   SLOWSRC (play the chosen walks N times slower, to find which walk drives an effect).
//
// The hooks sit inside the walker at fixed offsets from its stream-loop head, all checked byte by byte at install.
// FFX-2 registers: scratch EBX, unit EDI, PC ESI. History, measurements and evidence: re/ledger/PORT-OPU.md (it also
// keeps this file's earlier header), XFADE-BLINK.md, HANG-0021.md, CRASH-0946.md; sigs tools/sigs_x2_opu.txt.
#include "../x2.h"
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>

namespace x2 {

typedef void(__cdecl* walker_fn)(uint32_t unit_ctx, uint32_t pass);

// --- Places inside the game's walker that we hook ----------------------------------------------------------------------
// All found from the stream-loop head signature and checked byte by byte in install(). Ghidra addresses in brackets.
static uint8_t* g_walker = nullptr;                     // the walker itself (FUN_006bbca0)
static uint8_t* g_site_loop_head = nullptr;             // top of the loop over the 4 streams (0x6BC170)
static uint8_t* g_site_stream_entry = nullptr;          // a stream is about to run (0x6BC1D2)
static uint8_t* g_site_handler_call = nullptr;          // the call of one opcode handler (0x6BC239)
static uint8_t* g_site_wait_dec = nullptr;              // the WAIT countdown of a waiting stream (0x6BC2C6)
static void*    g_site_next_stream = nullptr;           // on to the next stream (0x6BC2CC)
static void*    g_site_after_streams = nullptr;         // after the stream loop (0x6BC2E1)
static uint8_t* g_site_integration_gate = nullptr;      // the test before the position integration (0x6BC2FC)
static void*    g_site_after_handler = nullptr;         // the yield test after a handler (0x6BC240)
static void*    g_site_after_integration = nullptr;     // past the integration: where the gate jumps when paused (0x6BC406)
// The original code at each hooked place (MinHook trampolines)
static walker_fn   g_walker_orig = nullptr;
static void*    g_loop_head_orig = nullptr;
static void*    g_stream_entry_orig = nullptr;
static void*    g_handler_call_orig = nullptr;
static void*    g_wait_dec_orig = nullptr;
static void*    g_next_stream_orig = nullptr;
static void*    g_integration_gate_orig = nullptr;
// Return addresses that tell the game's own walks apart from DLL ones
static uint8_t* g_exe_pass1_ret = nullptr;              // after the game's pass-1 walk call (0x6A5D85)
static uint8_t* g_exe_pass0_ret = nullptr;              // after the game's pass-0 walk call (0x6A5D0D)
static bool     g_hooks_installed = false;
extern Row row_MAG025;

// --- FOLLOWGUARD -------------------------------------------------------------------------------------------------------
// After the streams, a unit may "follow" another one: the walker calls a handler from a table of 13 (0xD50120) at
// 0x6BC446. The attach handler (FUN_006c2cd0) looks the target up and reads [target+0xBC] without a null check.
typedef uint8_t*(__cdecl* find_unit_fn)(uint8_t* ctx, uint8_t* obj, uint16_t id);
static uint8_t*    g_site_follow_call = nullptr;        // the follow call (0x6BC446)
static void*       g_site_after_follow = nullptr;       // just after it (0x6BC457)
static void*       g_follow_call_orig = nullptr;
static uint8_t**   g_follow_table = nullptr;            // the follow handler table (0xD50120)
static uint8_t*    g_follow_attach = nullptr;           // the attach handler (FUN_006c2cd0)
static find_unit_fn g_find_unit = nullptr;              // the game's unit lookup (FUN_006ac1f0)
static int         g_opt_followguard = 1;               // X2-MAG-025-FOLLOWGUARD
static volatile uint8_t g_follow_skip = 0;              // skip the follow call now running
static volatile LONG g_follow_skipped = 0;

// --- ini switches ([rows] X2-MAG-025-*) --------------------------------------------------------------------------------
static int g_opt_mode_exe = 3;                          // MODE: the game's pass-1 walk (0 = never held)
static int g_opt_mode_dll = 3;                          // DLLMODE: walks called by spell DLLs (0 = never held)
static int g_opt_mode_list = 2;                         // LISTMODE: DLL walks under the list profile
static int g_opt_mode_exe_pass0 = 1;                    // PASS0MODE: the game's pass-0 walk (0 = as MODE)
static int g_opt_mode_dll_pass0 = 0;                    // DLLPASS0MODE: DLL pass-0 walks (0 = as the DLL mode)
static int g_opt_hold_dll_pass0 = 0;                    // PASS0: hold DLL pass-0 walks outside the list profile too
static uint8_t g_opt_hold_position = 1;                 // POS: skip the position integration on held walks
static bool g_held_opcode[512];                         // DENY=hex,hex: never run on held walks (opcode word & 0x1FF)
static int  g_opt_opdiag = 0;                           // OPDIAG: log held-walk handler counts every 30 invocations
static int  g_opt_loop1f = 1;                           // LOOP1F
static int  g_opt_attach_deny = 0;                      // ATTACHDENY
static volatile LONG g_attach_held = 0;
static volatile LONG g_optrace_left = 0;                // OPTRACE=n: log the first n held-walk handlers after a 0x1F
static bool g_trace_on = false;
static int  g_opt_slowtest = 0;                         // SLOWTEST=N: under the list profile, chosen walks step 1 tick in N
static int  g_opt_slowtest_src = 1;                     // SLOWSRC: bit 0 game pass 0, bit 1 DLL pass 0, bit 2 DLL pass 1
static uint32_t g_slowtest_walks[3];

// --- State of the walk running now -------------------------------------------------------------------------------------
static volatile uint8_t g_held_mode = 0;                // mode of the held walk running now (0 = a normal walk)
static uint8_t g_walk_src = 0;                          // 0 = the game's walk, 1 = a DLL's (for the log)
static volatile LONG g_held_walks[2] = {0, 0};          // held walks so far: game, DLL
static volatile LONG g_wait0_ran = 0, g_wait0_held = 0;   // mode 3: WAIT-0 streams that ran / were skipped
static volatile LONG g_op_ran[3][512], g_op_held[3][512];   // OPDIAG counts per walk kind
static uint8_t g_walk_kind = 0;                         // 0 game walk, 1 DLL pass 1, 2 DLL pass 0
static bool g_held_opcode_dll0[512];                    // DLL0DENY=hex,hex: also never run on DLL pass-0 walks

// Mode 3 needs to know, per unit and stream, whether its last logic walk "repeated" (ran handlers from WAIT 0 and
// yielded at the opcode it started from). A small hash table keyed by the unit record, one bit per stream.
struct RepeatEntry { uint8_t* rec; uint8_t mask; };
static RepeatEntry g_repeat_table[4096];
static RepeatEntry* repeat_entry(uint8_t* rec, bool add) {
    uint32_t h = ((uint32_t)rec >> 8) & 4095;
    for (int i = 0; i < 4096; i++, h = (h + 1) & 4095) {
        if (g_repeat_table[h].rec == rec) return &g_repeat_table[h];
        if (!g_repeat_table[h].rec) {
            if (!add) return nullptr;
            g_repeat_table[h].rec = rec;
            g_repeat_table[h].mask = 0;
            return &g_repeat_table[h];
        }
    }
    return nullptr;
}
// The stream a logic walk is running now: unit, stream number, PC and WAIT when it started
static struct { uint8_t* rec; int s; uint32_t pc; uint8_t wait; bool valid; } g_stream_start;
static volatile uint8_t g_skip_stream = 0;              // the hook at stream entry skips this stream
static volatile uint8_t g_hold_this_op = 0;             // the handler about to run must not (the stream yields)
static volatile uint8_t g_loop_jump_now = 0;            // LOOP1F: jump to g_loop_target instead of the handler
static uint32_t g_loop_target = 0;
static volatile LONG g_loop_runs = 0;
// LOOP1F guard: the uncounted jump-back of the held walk running now. A loop whose body never yields reaches the same
// 0x1F again in the same walk; jumping back uncounted would never end (this hung Power Break, HANG-0021.md).
static uint8_t* g_loop1f_unit = nullptr;
static uint8_t* g_loop1f_at = nullptr;
static int      g_loop1f_jumps = 0;
static volatile LONG g_loop_repeats = 0;

// --- What the hooks do ---------------------------------------------------------------------------------------------------
// scr = the walker's scratch block (+0x216 stream number, +0x21A current opcode word, +0x220 unit, +0x210 yield flag),
// rec = the unit's stream record (PC of stream s at +4*s, its WAIT counter at +0x10+s).

// A stream is about to run. Logic walk: remember where it starts (for the repeat test). Held walk in mode 3: a WAIT-0
// stream that did not repeat on its last logic walk is skipped.
static void __cdecl on_stream_entry(uint8_t* scr, uint8_t* rec) {
    int st = *(int16_t*)(scr + 0x216) & 3;
    uint8_t wait = rec[0x10 + st];
    g_skip_stream = 0;
    g_stream_start.valid = false;
    if (!g_held_mode) {
        g_stream_start.rec = rec; g_stream_start.s = st; g_stream_start.pc = *(uint32_t*)(rec + st * 4); g_stream_start.wait = wait; g_stream_start.valid = true;
        return;
    }
    if (wait || g_held_mode != 3) return;             // counting streams: the WAIT hook keeps them
    RepeatEntry* r = repeat_entry(rec, false);
    if (r && (r->mask >> st & 1)) { InterlockedIncrement(&g_wait0_ran); return; }
    InterlockedIncrement(&g_wait0_held);
    g_skip_stream = 1;                           // the walker's "already ran" mark lives elsewhere: nothing to undo
}

// A stream has finished (logic walks): record whether it repeated, for mode 3.
static void __cdecl on_stream_next(uint8_t* scr, uint8_t* rec) {
    if (!g_stream_start.valid) return;
    g_stream_start.valid = false;
    int st = *(int16_t*)(scr + 0x216) & 3;
    if (g_stream_start.rec != rec || g_stream_start.s != st) return;
    bool repeat = g_stream_start.wait == 0 && *(uint32_t*)(rec + st * 4) == g_stream_start.pc;
    RepeatEntry* r = repeat_entry(rec, repeat);
    if (!r) return;
    if (repeat) r->mask |= (uint8_t)(1 << st);
    else r->mask &= (uint8_t)~(1 << st);
}

// An opcode handler is about to run on a held walk (modes 2/3): decide whether it may (DENY, DLL0DENY, ATTACHDENY),
// and for a 0x1F loop counter whether LOOP1F jumps back without counting.
static void __cdecl on_handler(uint8_t* scr, uint8_t* pc) {
    uint16_t word = *(uint16_t*)(scr + 0x21A);
    int op = word & 0x1FF;
    if (g_optrace_left > 0 && (g_trace_on || op == 0x1F)) {
        g_trace_on = true;
        uint8_t* u = *(uint8_t**)(scr + 0x220);
        if (InterlockedDecrement(&g_optrace_left) >= 0)
            log_line("X2-MAG-025 optrace %s scr %p stream %d pc %p word %04X op %03X unit %p +18 %02X +B4 %02X +B5 %02X",
                     g_walk_src ? "DLL" : "EXE", scr, *(int16_t*)(scr + 0x216) & 3, pc, word, op, u,
                     u ? u[0x18] : 0, u ? u[0xB4] : 0, u ? u[0xB5] : 0);
    }
    g_hold_this_op = (g_held_opcode[op] || (g_walk_kind == 2 && g_held_opcode_dll0[op])) ? 1 : 0;
    if (!g_hold_this_op && g_opt_attach_deny && op == 0xDC && ((word >> 12) == 0 || (word >> 12) == 6)) {
        g_hold_this_op = 1;                          // particle program attach: logic walks only
        LONG n = InterlockedIncrement(&g_attach_held);
        if (n <= 3 || n % 1000 == 0)
            log_line("X2-MAG-025-ATTACHDENY: 0xDC attach held on a %s walk (unit %p, #%ld) - runs on the next logic walk",
                     g_walk_src ? "DLL" : "EXE", *(uint8_t**)(scr + 0x220), n);
    }
    g_loop_jump_now = 0;
    if (op == 0x1F && g_hold_this_op && g_opt_loop1f && g_walk_kind != 2) {   // not on DLL pass-0 (update) walks: CRASH-0946
        // 0x1F decrements the unit's loop counter (+0xB4, or +0xB4 + word>>12) and jumps back by [PC+2] while it is
        // not zero. Jump back without counting, unless this is the last count or the loop came round again in this walk.
        uint8_t* unit = *(uint8_t**)(scr + 0x220);
        uint8_t* ctr = unit ? unit + 0xB4 + ((word & 0x200) ? 0 : (uint8_t)(word >> 12)) : nullptr;
        if (ctr && (uint8_t)(*ctr - 1) != 0) {
            if ((unit == g_loop1f_unit && pc == g_loop1f_at) || g_loop1f_jumps >= 64) {
                // non-yielding body: stay denied (the stream yields here; the next logic walk counts the loop)
                LONG n = InterlockedIncrement(&g_loop_repeats);
                if (n <= 5 || n % 600 == 0)
                    log_line("X2-MAG-025-LOOP1F: 0x1F at %p reached again in one held %s walk (unit %p, #%ld) - yields "
                             "instead", pc, g_walk_src ? "DLL" : "EXE", unit, n);
            } else {
                g_loop1f_unit = unit;
                g_loop1f_at = pc;
                g_loop1f_jumps++;
                g_loop_target = (uint32_t)(uintptr_t)(pc + *(int16_t*)(pc + 2));
                g_loop_jump_now = 1;
                g_hold_this_op = 0;
                InterlockedIncrement(&g_loop_runs);
            }
        }
    }
    if (g_opt_opdiag) InterlockedIncrement(g_hold_this_op ? &g_op_held[g_walk_kind][op] : &g_op_ran[g_walk_kind][op]);
}

// The hooks themselves are naked: they sit in the middle of the walker, save the registers around our C function,
// then continue in the original code (..._orig) or jump past it.

// Mode 1: skip the whole stream loop.
__declspec(naked) static void hook_loop_head() {
    __asm {
        cmp byte ptr [g_held_mode], 1
        je held
        jmp dword ptr [g_loop_head_orig]
    held:
        jmp dword ptr [g_site_after_streams]
    }
}

// Modes 2/3: a waiting stream keeps its WAIT count (no countdown on render-only frames).
__declspec(naked) static void hook_wait_dec() {
    __asm {
        cmp byte ptr [g_held_mode], 2
        jb pass
        jmp dword ptr [g_site_next_stream]
    pass:
        jmp dword ptr [g_wait_dec_orig]
    }
}

// Every walk: on_stream_entry, then run the stream or skip to the next one.
__declspec(naked) static void hook_stream_entry() {
    __asm {
        pushad
        push edi                                 // unit record
        push ebx                                 // scratch
        call on_stream_entry
        add esp, 8
        popad
        cmp byte ptr [g_skip_stream], 0
        jne skip
        jmp dword ptr [g_stream_entry_orig]
    skip:
        jmp dword ptr [g_site_next_stream]
    }
}

// Every walk: on_stream_next, then on to the next stream as usual.
__declspec(naked) static void hook_next_stream() {
    __asm {
        pushad
        push edi
        push ebx
        call on_stream_next
        add esp, 8
        popad
        jmp dword ptr [g_next_stream_orig]
    }
}

// Modes 2/3: on_handler, then one of three ways on. Held opcode: it does not run and the stream yields at it (PC kept,
// WAIT 0), so the next logic walk runs it. LOOP1F jump: continue at the loop start, counter untouched. Else: run it.
__declspec(naked) static void hook_handler_call() {
    __asm {
        cmp byte ptr [g_held_mode], 2
        jb pass
        pushad
        push esi                                 // PC of the opcode
        push ebx
        call on_handler
        add esp, 8
        popad
        cmp byte ptr [g_loop_jump_now], 0
        jne loop1f
        cmp byte ptr [g_hold_this_op], 0
        je pass
        mov eax, esi
        mov byte ptr [ebx + 0x210], 1
        jmp dword ptr [g_site_after_handler]
    loop1f:
        mov eax, dword ptr [g_loop_target]           // 0x1F jump-back target, count untouched, no yield
        jmp dword ptr [g_site_after_handler]
    pass:
        jmp dword ptr [g_handler_call_orig]
    }
}

// POS: on a held walk, skip the position integration by jumping where the gate jumps when the walker is paused. We
// do not set the walker's pause flag (+0x212) itself: spawn handlers read it too and would drop their spawn.
__declspec(naked) static void hook_integration_gate() {
    __asm {
        cmp byte ptr [g_held_mode], 0
        je pass
        cmp byte ptr [g_opt_hold_position], 0
        je pass
        jmp dword ptr [g_site_after_integration]
    pass:
        jmp dword ptr [g_integration_gate_orig]
    }
}

// FOLLOWGUARD: before an attach follow, look the target up the way the handler will; if its +0xBC is null, skip it.
static void __cdecl on_follow(uint32_t type, uint8_t* scr) {
    g_follow_skip = 0;
    if (type >= 13 || g_follow_table[type] != g_follow_attach) return;
    uint8_t* obj = *(uint8_t**)(scr + 0x220);
    uint16_t id = *(uint16_t*)(obj + 0xA0);
    uint8_t* base = *(uint8_t**)(scr + 0x320);
    uint8_t* t = g_find_unit(scr + 0x300, obj, id);
    if (*(void**)(t + 0xBC)) return;
    g_follow_skip = 1;
    LONG n = InterlockedIncrement(&g_follow_skipped);
    if (n <= 20 || n % 600 == 0)
        log_line("X2-MAG-025-FOLLOWGUARD: follow skipped (#%ld): target +0xBC NULL; kind %d hold %d src %s, unit #%d "
                 "id 0x%04X, target id 0x%04X -> #%d%s, group %d/%d",
                 n, pacing_kind(), g_held_mode, g_walk_src ? "DLL" : "EXE", (int)((obj - base) >> 8), *(uint16_t*)(obj + 0x1A),
                 id, (int)((t - base) >> 8), t == base ? " (not found)" : "", obj[0x1E], t[0x1E]);
}

__declspec(naked) static void hook_follow_call() {
    __asm {
        pushad
        push ebx                                 // scratch
        movzx eax, al
        push eax                                 // follow type
        call on_follow
        add esp, 8
        popad
        cmp byte ptr [g_follow_skip], 0
        jne skip
        jmp dword ptr [g_follow_call_orig]
    skip:
        jmp dword ptr [g_site_after_follow]
    }
}

// --- The walk itself -------------------------------------------------------------------------------------------------------
// Runs the game's walker with the hooks in "held" mode. Walks can nest (a handler can start another walk), so the
// state of the outer walk is saved and put back.
static void held_walk(int mode, uint8_t src, uint32_t unit_ctx, uint32_t pass) {
    InterlockedIncrement(&g_held_walks[src]);
    uint8_t saved = g_held_mode, saved_src = g_walk_src, saved_dsrc = g_walk_kind;
    uint8_t* saved_unit = g_loop1f_unit;
    uint8_t* saved_at = g_loop1f_at;
    int saved_n = g_loop1f_jumps;
    g_held_mode = (uint8_t)mode;
    g_walk_src = src;
    g_walk_kind = src == 0 ? 0 : (uint8_t)pass == 0 ? 2 : 1;
    g_loop1f_unit = nullptr;
    g_loop1f_at = nullptr;
    g_loop1f_jumps = 0;
    g_walker_orig(unit_ctx, pass);
    g_held_mode = saved;
    g_walk_src = saved_src;
    g_walk_kind = saved_dsrc;
    g_loop1f_unit = saved_unit;
    g_loop1f_at = saved_at;
    g_loop1f_jumps = saved_n;
}

static void walker_body(uint32_t unit_ctx, uint32_t pass, uint8_t* ret);

// Walk running now, for diagnostics (X2-DIAG-PPPSLOT): 1 + pass, 0 = none; nested walks restore it.
static volatile int g_walk_pass = 0;
int mag025_walk_info() { return g_held_mode | (g_walk_pass << 8); }

// Replaces the walker's entry. Who called it (the return address: inside FFX-2.exe or not, and which call) decides
// whether this walk is held and in which mode; see the table in the header.
static void __cdecl hook_walker(uint32_t unit_ctx, uint32_t pass) {
    int saved = g_walk_pass;
    g_walk_pass = (uint8_t)pass + 1;
    walker_body(unit_ctx, pass, (uint8_t*)_ReturnAddress());
    g_walk_pass = saved;
}

static void walker_body(uint32_t unit_ctx, uint32_t pass, uint8_t* ret) {
    if (g_opt_slowtest > 1 && row_MAG025.on && !g_disabled && !render_only() && mag008_list_active()) {
        // SLOWTEST, logic frames: k = 0 game pass 0, 1 DLL pass 0, 2 DLL pass 1; chosen walks step 1 tick in N
        int k = !in_image(ret) ? ((uint8_t)pass == 0 ? 1 : 2) : (g_exe_pass0_ret && ret == g_exe_pass0_ret) ? 0 : -1;
        if (k >= 0 && (g_slowtest_walks[k]++ % (uint32_t)g_opt_slowtest) != 0 && (g_opt_slowtest_src >> k & 1)) {
            held_walk(1, k ? 1 : 0, unit_ctx, pass);
            return;
        }
    }
    if (row_MAG025.on && !g_disabled && render_only()) {
        uint8_t p = (uint8_t)pass;
        if (!in_image(ret)) {                                   // called by a spell DLL
            bool prof = g_opt_mode_list && mag008_list_active();
            int m = prof ? g_opt_mode_list : g_opt_mode_dll;
            if (m && p == 0 && g_opt_mode_dll_pass0) m = g_opt_mode_dll_pass0;
            if (m && (p == 1 || (p == 0 && (g_opt_hold_dll_pass0 || prof)))) { held_walk(m, 1, unit_ctx, pass); return; }
        } else if (g_opt_mode_exe && ret == g_exe_pass1_ret) {
            held_walk(g_opt_mode_exe, 0, unit_ctx, pass);
            return;
        } else if (g_opt_mode_exe && g_exe_pass0_ret && ret == g_exe_pass0_ret) {
            held_walk(g_opt_mode_exe_pass0 ? g_opt_mode_exe_pass0 : g_opt_mode_exe, 0, unit_ctx, pass);
            return;
        }
    }
    g_walker_orig(unit_ctx, pass);
}

static const char* mode_name(int m) {
    return m == 0 ? "untouched" : m == 1 ? "skip every stream step" : m == 2 ? "hold WAIT countdowns"
                                                                             : "hold WAIT, run repeating WAIT-0 streams";
}

// --- Install: find and check every site; the hooks go in at the first pre() ------------------------------------------------
static bool install() {
    g_walker = sig_find("X2-MAG-025 walker FUN_006bbca0",
        "55 8B EC 81 EC 18 08 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 53 56 0F B7 75 08 57 89 B5 F4 F7 FF FF 83 FE 01 75 06 "
        "89 35 ?? ?? ?? ?? E8", 0x6BBCA0);
    if (!g_walker) return false;
    // cmp byte [ebx+0x21F],0 / jz / cmp byte [ebx+0x21D],0 / jz +0x168 (integration block) / movsx eax,[ebx+0x216] /
    // mov esi,[edi+eax*4]
    g_site_loop_head = sig_find("X2-MAG-025 stream-loop head",
        "80 BB 1F 02 00 00 00 74 0D 80 BB 1D 02 00 00 00 0F 84 68 01 00 00 0F BF 83 16 02 00 00 8B 34 87", 0x6BC170);
    if (!g_site_loop_head) return false;
    struct { uint8_t* at; const char* sig; const char* what; } checks[] = {
        { g_site_loop_head + 0x62,  "0F BF 8B 16 02 00 00 8A 44 39 10 84 C0 0F 85", "stream entry" },
        { g_site_loop_head + 0xC9,  "56 53 FF D0 83 C4 08 80 BB 10 02 00 00 00 8B F0", "handler call" },
        { g_site_loop_head + 0x156, "FE C8 88 44 39 10 66 FF 83 16 02 00 00", "WAIT decrement" },
        // inc word [ebx+0x216] / cmp word [ebx+0x216],4 / jl head / cmp byte [ebx+0x21D],0 / jnz
        { g_site_loop_head + 0x15C, "66 FF 83 16 02 00 00 66 83 BB 16 02 00 00 04 0F 8C 8F FE FF FF 80 BB 1D 02 00 00 00 0F 85",
          "stream-loop tail" },
        // mov al,[edi+0xB2] / test al,2 / jnz / cmp word [ebx+0x212],0 / jnz
        { g_site_loop_head + 0x17E, "8A 87 B2 00 00 00 A8 02 0F 85 ?? ?? ?? ?? 66 83 BB 12 02 00 00 00 0F 85", "integration gate" },
    };
    for (auto& c : checks)
        if (!sig_match(c.at, c.sig)) {
            log_line("  X2-MAG-025: %s at gva 0x%08X does not match - row off", c.what, rt_to_gva(c.at));
            return false;
        }
    g_site_stream_entry = g_site_loop_head + 0x62;
    g_site_handler_call = g_site_loop_head + 0xC9;
    g_site_after_handler = g_site_handler_call + 7;
    g_site_wait_dec = g_site_loop_head + 0x156;
    g_site_next_stream = g_site_loop_head + 0x15C;
    g_site_after_streams = g_site_loop_head + 0x171;
    g_site_integration_gate = g_site_loop_head + 0x18C;
    uint8_t* held = g_site_integration_gate + 8 + 6 + *(int32_t*)(g_site_integration_gate + 10);   // the gate's jnz rel32 target
    if (!sig_match(held, "A8 04 75")) {                          // test al,4 / jnz (follow skip)
        log_line("  X2-MAG-025: gate target gva 0x%08X is not 'test al,4' - row off", rt_to_gva(held));
        return false;
    }
    g_site_after_integration = held;

    g_opt_followguard = config_row("X2-MAG-025-FOLLOWGUARD", 1);
    if (g_opt_followguard) {
        // test al,al / jz / movzx eax,al / push esi / mov eax,[eax*4+table] / push ebx / call eax / add esp,8 /
        // inc word [ebx+0x218]
        uint8_t* site = sig_find("X2-MAG-025-FOLLOWGUARD follow call",
            "84 C0 74 11 0F B6 C0 56 8B 04 85 ?? ?? ?? ?? 53 FF D0 83 C4 08 66 FF 83 18 02 00 00", 0x6BC442);
        // FUN_006c2cd0: obj = [scr+0x220]; t = FUN_006ac1f0(scr+0x300, obj, obj+0xA0); esi = [t+0xBC]; movsx eax,[esi]
        uint8_t* fn = sig_find("X2-MAG-025-FOLLOWGUARD attach handler",
            "55 8B EC 53 8B 5D 08 56 57 8B BB 20 02 00 00 0F B7 87 A0 00 00 00 50 8D 83 00 03 00 00 57 50 E8 ?? ?? ?? ?? "
            "83 C4 0C 8B B0 BC 00 00 00 0F BF 06", 0x6C2CD0);
        uint8_t* find = fn ? call_target(fn + 0x1F) : nullptr;
        uint8_t* table = site ? abs_operand("X2-MAG-025-FOLLOWGUARD follow table", site + 0xB) : nullptr;
        if (!site || !find || !table || !sig_match(find, "55 8B EC 56 8B 75 0C 57 66 8B 7D 10 66 85 FF 79 4F")) {
            log_line("  X2-MAG-025-FOLLOWGUARD: sites not found - guard off");
            g_opt_followguard = 0;
        } else {
            g_site_follow_call = site + 4;
            g_site_after_follow = site + 0x15;
            g_follow_table = (uint8_t**)table;
            g_follow_attach = fn;
            g_find_unit = (find_unit_fn)find;
            log_line("  X2-MAG-025-FOLLOWGUARD: follow call gva=0x%08X, table gva=0x%08X, attach handler gva=0x%08X, "
                     "unit lookup gva=0x%08X", rt_to_gva(g_site_follow_call), rt_to_gva(table), rt_to_gva(fn), rt_to_gva(find));
        }
    }
    g_opt_mode_exe = config_row("X2-MAG-025-MODE", 3);
    g_opt_mode_dll = config_row("X2-MAG-025-DLLMODE", 3);
    if (g_opt_mode_exe < 0 || g_opt_mode_exe > 3) g_opt_mode_exe = 3;
    if (g_opt_mode_dll < 0 || g_opt_mode_dll > 3) g_opt_mode_dll = 3;
    g_opt_mode_exe_pass0 = config_row("X2-MAG-025-PASS0MODE", 1);
    if (g_opt_mode_exe_pass0 < 0 || g_opt_mode_exe_pass0 > 3) g_opt_mode_exe_pass0 = 1;
    g_opt_mode_dll_pass0 = config_row("X2-MAG-025-DLLPASS0MODE", 0);
    if (g_opt_mode_dll_pass0 < 0 || g_opt_mode_dll_pass0 > 3) g_opt_mode_dll_pass0 = 0;
    g_opt_mode_list = config_row("X2-MAG-025-LISTMODE", 2);
    if (g_opt_mode_list < 0 || g_opt_mode_list > 3) g_opt_mode_list = 2;
    if (config_row("X2-MAG-025-EXE", 1) == 0) g_opt_mode_exe = 0;
    if (g_opt_mode_exe) {
        // FUN_006a5d30: push / call FUN_0064f580 / push 1 / push 0 / call walker / mov esi,[..] / push 0 / mov [..],0
        uint8_t* site = sig_find("X2-MAG-025 EXE pass-1 walk call",
            "68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 01 6A 00 E8 ?? ?? ?? ?? 8B 35 ?? ?? ?? ?? 6A 00 C7 05", 0x6A5D72);
        if (!site || call_target(site + 0xE) != g_walker) {
            log_line("  X2-MAG-025: EXE pass-1 walk call not found - EXE walks untouched");
            g_opt_mode_exe = 0;
        } else {
            g_exe_pass1_ret = site + 0x13;
        }
    }
    if (g_opt_mode_exe && config_row("X2-MAG-025-EXEPASS0", 1)) {
        // Fx_OpuUpdate: mov [0x1b41b9c],eax / push 0 / push 0 / call walker / add esp,8 / cmp [esi+0x58],0 / jnz +8
        uint8_t* s0 = sig_find("X2-MAG-025 EXE pass-0 walk call",
            "A3 ?? ?? ?? ?? 6A 00 6A 00 E8 ?? ?? ?? ?? 83 C4 08 83 7E 58 00 75 08", 0x6A5CFF);
        if (s0 && call_target(s0 + 9) == g_walker) g_exe_pass0_ret = s0 + 0xE;
        else log_line("  X2-MAG-025-EXEPASS0: EXE pass-0 walk call not found - untouched");
    }
    g_opt_hold_dll_pass0 = config_row("X2-MAG-025-PASS0", 0);
    g_opt_hold_position = config_row("X2-MAG-025-POS", 1) ? 1 : 0;
    g_opt_opdiag = config_row("X2-MAG-025-OPDIAG", 0);
    g_opt_loop1f = config_row("X2-MAG-025-LOOP1F", 1);
    g_opt_attach_deny = config_row("X2-MAG-025-ATTACHDENY", 0);
    g_optrace_left = config_row("X2-MAG-025-OPTRACE", 0);
    g_opt_slowtest = config_row("X2-MAG-025-SLOWTEST", 0);
    g_opt_slowtest_src = config_row("X2-MAG-025-SLOWSRC", 1);
    if (g_opt_slowtest > 1)
        log_line("  X2-MAG-025-SLOWTEST: test, under the list profile walks%s%s%s step on 1 logic frame in %d",
                 g_opt_slowtest_src & 1 ? " EXE-pass0" : "", g_opt_slowtest_src & 2 ? " DLL-pass0" : "", g_opt_slowtest_src & 4 ? " DLL-pass1" : "",
                 g_opt_slowtest);
    char deny[256];
    config_str("rows", "X2-MAG-025-DENY", deny, sizeof(deny), "1F,7F");   // loop counter, battle signal
    char line[256]; int k = 0; line[0] = 0;
    for (char* t = deny; *t;) {
        char* e = nullptr;
        unsigned long op = strtoul(t, &e, 16);
        if (e == t) { t++; continue; }
        if (op < 512) { g_held_opcode[op] = true; k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " 0x%02lX", op); }
        t = e;
    }
    if (line[0]) log_line("  X2-MAG-025: opcodes held on render-only invocations (X2-MAG-025-DENY, modes 2/3):%s", line);
    config_str("rows", "X2-MAG-025-DLL0DENY", deny, sizeof(deny), "");
    k = 0; line[0] = 0;
    for (char* t = deny; *t;) {
        char* e = nullptr;
        unsigned long op = strtoul(t, &e, 16);
        if (e == t) { t++; continue; }
        if (op < 512) { g_held_opcode_dll0[op] = true; k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " 0x%02lX", op); }
        t = e;
    }
    if (line[0]) log_line("  X2-MAG-025-DLL0DENY: also held on render-only DLL pass-0 walks (modes 2/3):%s", line);
    log_line("  X2-MAG-025-ATTACHDENY: %s", g_opt_attach_deny ? "0xDC particle attach (sub-ops 0, 6) held on held walks (modes 2/3)" : "off");
    log_line("  X2-MAG-025-LOOP1F: %s", g_opt_loop1f && g_held_opcode[0x1F] ? "0x1F loops jump back uncounted on held walks (last count yields)" : "off");
    if (g_exe_pass0_ret) log_line("  X2-MAG-025-EXEPASS0: EXE pass-0 walk (return gva 0x%08X, expected 0x006A5D0D) held too",
                             rt_to_gva(g_exe_pass0_ret));
    log_line("  X2-MAG-025: EXE pass-1 walk (return gva 0x%08X, expected 0x006A5D85): mode %d, %s",
             g_exe_pass1_ret ? rt_to_gva(g_exe_pass1_ret) : 0, g_opt_mode_exe, mode_name(g_opt_mode_exe));
    log_line("  X2-MAG-025: DLL-called walks (pass 1%s): mode %d, %s; list profile (X2-MAG-008-PASS-LIST): mode %d, pass 0 held; "
             "held pass-0 walks: EXE mode %d, DLL mode %d", g_opt_hold_dll_pass0 ? " and 0" : "", g_opt_mode_dll, mode_name(g_opt_mode_dll), g_opt_mode_list,
             g_opt_mode_exe_pass0, g_opt_mode_dll_pass0);
    log_line("  X2-MAG-025: walker gva=0x%08X, stream head gva=0x%08X, held gate target gva=0x%08X (expected 0x006BC406); "
             "position hold %s", rt_to_gva(g_walker), rt_to_gva(g_site_loop_head), rt_to_gva(g_site_after_integration), g_opt_hold_position ? "ON" : "OFF");
    return true;
}

// --- Every invocation ------------------------------------------------------------------------------------------------------
// First pre(), on the game thread: install the seven hooks (all or none) and the follow guard.
static void pre() {
    if (g_hooks_installed) return;
    g_hooks_installed = true;
    struct { const char* name; void* at; void* detour; void** tramp; } hooks[] = {
        { "X2-MAG-025 head",    g_site_loop_head,   (void*)&hook_loop_head,   &g_loop_head_orig },
        { "X2-MAG-025 entry",   g_site_stream_entry, (void*)&hook_stream_entry, &g_stream_entry_orig },
        { "X2-MAG-025 handler", g_site_handler_call,  (void*)&hook_handler_call,  &g_handler_call_orig },
        { "X2-MAG-025 dec",     g_site_wait_dec,    (void*)&hook_wait_dec,    &g_wait_dec_orig },
        { "X2-MAG-025 next",    g_site_next_stream,   (void*)&hook_next_stream,   &g_next_stream_orig },
        { "X2-MAG-025 gate",    g_site_integration_gate,   (void*)&hook_integration_gate,   &g_integration_gate_orig },
        { "X2-MAG-025",         g_walker,  (void*)&hook_walker,   (void**)&g_walker_orig },
    };
    int n = (int)(sizeof(hooks) / sizeof(hooks[0]));
    for (int i = 0; i < n; i++) {
        if (hook_entry(hooks[i].name, hooks[i].at, hooks[i].detour, hooks[i].tramp)) continue;
        while (--i >= 0) hook_remove(hooks[i].at);
        log_line("X2-MAG-025: hooks failed - row off");
        row_MAG025.on = 0;
        return;
    }
    log_line("X2-MAG-025: walker entry and 6 inner sites hooked on the game thread");
    if (g_opt_followguard && hook_entry("X2-MAG-025-FOLLOWGUARD", g_site_follow_call, (void*)&hook_follow_call, &g_follow_call_orig))
        log_line("X2-MAG-025-FOLLOWGUARD: follow call hooked");
}

// OPDIAG: one line of opcode:count (and opcode:held count) for one walk kind, counters reset.
static void log_ops(int src) {
    char line[1024]; int k = 0; line[0] = 0;
    for (int op = 0; op < 512; op++) {
        LONG n = InterlockedExchange(&g_op_ran[src][op], 0), d = InterlockedExchange(&g_op_held[src][op], 0);
        if (n) k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %02X:%ld", op, n);
        if (d) k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %02X:held%ld", op, d);
    }
    if (line[0]) log_line("X2-MAG-025 opdiag %s: held-walk handlers (op:count):%s", src == 0 ? "EXE" : src == 1 ? "DLL pass1" : "DLL pass0", line);
}

// After every invocation: diagnostics and the periodic counters line (every 1800 invocations, ~30 s).
static void post() {
    static uint32_t tick = 0;
    if (g_opt_opdiag && tick % 30 == 29) { log_ops(0); log_ops(1); log_ops(2); }
    if (g_opt_slowtest > 1 && tick % 60 == 59) {
        static uint32_t seen[3] = {0, 0, 0};
        if (g_slowtest_walks[0] != seen[0] || g_slowtest_walks[1] != seen[1] || g_slowtest_walks[2] != seen[2]) {
            log_line("X2-MAG-025-SLOWTEST: logic-frame walks seen so far (1 in %d stepped): EXE pass0 %lu, DLL pass0 %lu, "
                     "DLL pass1 %lu", g_opt_slowtest, (unsigned long)g_slowtest_walks[0], (unsigned long)g_slowtest_walks[1], (unsigned long)g_slowtest_walks[2]);
            for (int i = 0; i < 3; i++) seen[i] = g_slowtest_walks[i];
        }
    }
    static LONG reported[2] = {0, 0};
    for (int s = 0; s < 2; s++)
        if (!reported[s] && g_held_walks[s] > 0) {
            reported[s] = 1;
            log_line("X2-MAG-025: first held %s walk on a render-only invocation", s ? "DLL" : "EXE");
        }
    static LONG last = 0;
    LONG total = g_held_walks[0] + g_held_walks[1];
    if (++tick % 1800 == 0 && total != last) {
        last = total;
        log_line("X2-MAG-025: held walks so far: EXE %ld, DLL %ld; WAIT-0 streams on mode-3 walks: %ld ran, %ld held; "
                 "0x1F loops run uncounted %ld", g_held_walks[0], g_held_walks[1], g_wait0_ran, g_wait0_held, g_loop_runs);
    }
}

Row row_MAG025 = {
    "X2-MAG-025",
    "battle effect-unit walks (EXE pass 1, magic-DLL draws): render-only invocations draw without stepping",
    1, install, pre, post, 0
};

} // namespace x2
