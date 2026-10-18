// SPDX-License-Identifier: Apache-2.0
// X2-MAG-025 (port of FFX MAG-025): battle effect units (command scripts, OPU) step on every walk of the unit
// bytecode walker FUN_006bbca0 (FFX OpuMain_UnitBytecodeWalk): the EXE walks pass 1 (FUN_006a5d30, through the
// effect dispatch 0x616660(1) in Sg_MainLoop's second loop) on every invocation, and 310 magic DLLs call the walker
// (service slot +0xB14) from their draw. WAIT countdowns, script handlers (battle signals, actor motion requests,
// motion-end polls) and unit motion run 2x at 60 fps (FFX: Tidus's Mental Break wind-up). Render-only invocations
// hold these walks, per walk source (EXE pass-1 call, DLL calls), in one of three modes:
//   1: jump from the stream-loop head to its exit test: no stream steps, the unit's follow call still runs.
//   2: keep every WAIT countdown; streams at WAIT 0 run their handlers.
//   3: as 2, but a WAIT-0 stream runs only if its last logic walk ran handlers and yielded at the PC it started
//      from (per-frame draw/poll loops: FFX's battle-entry shatter blinked without them).
// Position integration is held in every mode (X2-MAG-025-POS); listed opcodes never run (X2-MAG-025-DENY, modes 2/3).
// X2-MAG-025-LOOP1F (default 1, needs 0x1F in DENY): a held walk that reaches the 0x1F loop counter (Opu_Op1FLoopCounter
// 0x6A7610: --[unit+0xB4+(op>>12)], or +0xB4 with op&0x200; non-zero -> PC + [PC+2], else PC + 4) jumps back without
// counting while the count would stay non-zero, so a per-frame draw loop draws on render-only invocations too; the
// last count still yields to the next logic walk. U 18/10/2026: dressphere change body blink (magic_0937, re/ledger/
// XFADE-BLINK.md): denied, the stream yielded at 0x1F and mode 3 then skipped it every second frame; undenied, the
// count ran 2x (Warrior sword blink).
// X2-MAG-025-EXEPASS0 (default 1): the EXE pass-0 walk (Fx_OpuUpdate 0x6A5CD0, walker call 0x6A5D08, reached only
// through the effect dispatch 0x616660(0)) is held the same way (mode X2-MAG-025-MODE) on render-only invocations. It
// runs there only when X2-MAG-008 passes the dispatch (cross-fade DLLs, X2-MAG-008-PASS-LIST): U 18/10/2026 with
// PASS-LIST 885 937 906 the Warrior change stopped alternating dark / lit frames but played about 2x fast (0885 8.4 ->
// 5.0 s, 0937 4.0 -> 2.6 s, 0906 6.4 -> 3.3 s; D fps60x2.log 18:17).
// List profile (U 18/10/2026, Rikku -> Warrior fixed at 60 fps): while X2-MAG-008 passes a DLL named in
// X2-MAG-008-PASS-LIST (or chained from one), DLL-called walks use X2-MAG-025-LISTMODE (default 2: mode 3 skipped a
// draw loop on the first render-only walk after it started: one-frame dropout of the sword close-up's dark backdrop,
// shard stutter) and DLL pass-0 walks are held too (the passed dispatch runs them on render-only invocations).
// X2-MAG-025-FOLLOWGUARD: the attach follow handler reads [target+0xBC] unchecked (FFX Impulse crash); a NULL skips it.
// FFX-2 changes (PORT-OPU.md §5): scratch EBX / unit EDI / PC ESI (FFX ESI / EBX / EDI); the integration gate is
// cmp word [ebx+0x212],0 / jnz, so a held walk jumps to the gate's own target 0x6BC406 (AL from 0x6BC2EE kept); the
// sweep-2 latch lives in a side map that the test does not clear, so a skipped stream needs no latch put-back.
// Evidence: outer repo re/ledger/PORT-OPU.md (S); sigs tools/sigs_x2_opu.txt.
#include "../x2.h"
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>

namespace x2 {

typedef void(__cdecl* walk_t)(uint32_t unit_ctx, uint32_t pass);

// Walker sites (Ghidra VAs; all resolved from the stream-loop head signature and verified)
static uint8_t* g_entry = nullptr;               // FUN_006bbca0
static uint8_t* g_head = nullptr;                // 0x6BC170 stream-loop head
static uint8_t* g_sentry = nullptr;              // 0x6BC1D2 stream handler entry (after the latch tests)
static uint8_t* g_hcall = nullptr;               // 0x6BC239 push esi / push ebx / call eax (handler) / add esp,8
static uint8_t* g_dec = nullptr;                 // 0x6BC2C6 WAIT > 0 path: dec al / mov [ecx+edi+0x10],al
static void*    g_next = nullptr;                // 0x6BC2CC next stream
static void*    g_skip = nullptr;                // 0x6BC2E1 after the 4-stream loop (sweep-2 test, then integration)
static uint8_t* g_gate = nullptr;                // 0x6BC2FC cmp word [ebx+0x212],0 (position integration gate)
static void*    g_hcall_after = nullptr;         // 0x6BC240 yield test
static void*    g_gate_held = nullptr;           // 0x6BC406 after the integration (the gate's jnz target)
static walk_t   g_walk_tramp = nullptr;
static void*    g_head_tramp = nullptr;
static void*    g_sentry_tramp = nullptr;
static void*    g_hcall_tramp = nullptr;
static void*    g_dec_tramp = nullptr;
static void*    g_next_tramp = nullptr;
static void*    g_gate_tramp = nullptr;
static uint8_t* g_exe_ret = nullptr;             // return address of the EXE pass-1 walk call (0x6A5D85)
static uint8_t* g_exe0_ret = nullptr;            // return address of the EXE pass-0 walk call (0x6A5D0D)
static bool     g_patched = false;
extern Row row_MAG025;

// FOLLOWGUARD: follow call 0x6BC446 (movzx eax,al / push esi / mov eax,[eax*4+0xD50120] / push ebx / call eax)
typedef uint8_t*(__cdecl* find_unit_t)(uint8_t* ctx, uint8_t* obj, uint16_t id);
static uint8_t*    g_fcall = nullptr;            // 0x6BC446
static void*       g_fcall_after = nullptr;      // 0x6BC457 inc word [ebx+0x218]
static void*       g_fcall_tramp = nullptr;
static uint8_t**   g_ftable = nullptr;           // 0xD50120 follow handler table (13 entries)
static uint8_t*    g_fattach = nullptr;          // FUN_006c2cd0
static find_unit_t g_find_unit = nullptr;        // FUN_006ac1f0
static int         g_followguard = 1;            // X2-MAG-025-FOLLOWGUARD
static volatile uint8_t g_fskip = 0;
static volatile LONG g_fskipped = 0;

// [rows]
static int g_mode_exe = 3;                       // X2-MAG-025-MODE: EXE pass-1 walk (0 = untouched)
static int g_mode_dll = 3;                       // X2-MAG-025-DLLMODE: DLL-called walks (0 = untouched)
static int g_mode_list = 2;                      // X2-MAG-025-LISTMODE: DLL-called walks under the list profile
static int g_pass0 = 0;                          // X2-MAG-025-PASS0: hold DLL pass-0 walks too
static uint8_t g_hold_pos = 1;                   // X2-MAG-025-POS
static bool g_deny[512];                         // X2-MAG-025-DENY=hex,hex (opcode word & 0x1FF; bit 8 = DLL table)
static int  g_opdiag = 0;                        // X2-MAG-025-OPDIAG: held-walk handler counts every 30 invocations
static int  g_loop1f = 1;                        // X2-MAG-025-LOOP1F
static volatile LONG g_optrace = 0;              // X2-MAG-025-OPTRACE=n: log the first n held-walk handlers after a 0x1F loop
static bool g_trace_on = false;

static volatile uint8_t g_hold = 0;              // mode of the held walk running now (0 = none)
static uint8_t g_src = 0;                        // 0 = EXE walk, 1 = DLL walk (diagnostic)
static volatile LONG g_held[2] = {0, 0};         // held walks: EXE, DLL
static volatile LONG g_ro_run = 0, g_ro_blocked = 0;   // mode 3: WAIT-0 streams on held walks
static volatile LONG g_opcnt[2][512], g_opden[2][512];

// Mode 3: per stream, did its last logic walk run handlers (WAIT 0 at entry) and yield at the PC it started from?
// Keyed by unit record; 4 stream bits each.
struct RepeatSlot { uint8_t* rec; uint8_t mask; };
static RepeatSlot g_rep[4096];
static RepeatSlot* rep_slot(uint8_t* rec, bool add) {
    uint32_t h = ((uint32_t)rec >> 8) & 4095;
    for (int i = 0; i < 4096; i++, h = (h + 1) & 4095) {
        if (g_rep[h].rec == rec) return &g_rep[h];
        if (!g_rep[h].rec) {
            if (!add) return nullptr;
            g_rep[h].rec = rec;
            g_rep[h].mask = 0;
            return &g_rep[h];
        }
    }
    return nullptr;
}
static struct { uint8_t* rec; int s; uint32_t pc; uint8_t wait; bool valid; } g_cur;
static volatile uint8_t g_skip_stream = 0;
static volatile uint8_t g_deny_now = 0;
static volatile uint8_t g_loop_now = 0;
static uint32_t g_loop_pc = 0;
static volatile LONG g_loop_runs = 0;

static void __cdecl on_stream_entry(uint8_t* scr, uint8_t* rec) {
    int st = *(int16_t*)(scr + 0x216) & 3;
    uint8_t wait = rec[0x10 + st];
    g_skip_stream = 0;
    g_cur.valid = false;
    if (!g_hold) {
        g_cur.rec = rec; g_cur.s = st; g_cur.pc = *(uint32_t*)(rec + st * 4); g_cur.wait = wait; g_cur.valid = true;
        return;
    }
    if (wait || g_hold != 3) return;             // counting streams: the WAIT hook keeps them
    RepeatSlot* r = rep_slot(rec, false);
    if (r && (r->mask >> st & 1)) { InterlockedIncrement(&g_ro_run); return; }
    InterlockedIncrement(&g_ro_blocked);
    g_skip_stream = 1;                           // sweep-2 latch: side map, the test left it set (no put-back)
}

static void __cdecl on_stream_next(uint8_t* scr, uint8_t* rec) {
    if (!g_cur.valid) return;
    g_cur.valid = false;
    int st = *(int16_t*)(scr + 0x216) & 3;
    if (g_cur.rec != rec || g_cur.s != st) return;
    bool repeat = g_cur.wait == 0 && *(uint32_t*)(rec + st * 4) == g_cur.pc;
    RepeatSlot* r = rep_slot(rec, repeat);
    if (!r) return;
    if (repeat) r->mask |= (uint8_t)(1 << st);
    else r->mask &= (uint8_t)~(1 << st);
}

static void __cdecl on_handler(uint8_t* scr, uint8_t* pc) {
    uint16_t word = *(uint16_t*)(scr + 0x21A);
    int op = word & 0x1FF;
    if (g_optrace > 0 && (g_trace_on || op == 0x1F)) {
        g_trace_on = true;
        uint8_t* u = *(uint8_t**)(scr + 0x220);
        if (InterlockedDecrement(&g_optrace) >= 0)
            log_line("X2-MAG-025 optrace %s scr %p stream %d pc %p word %04X op %03X unit %p +18 %02X +B4 %02X +B5 %02X",
                     g_src ? "DLL" : "EXE", scr, *(int16_t*)(scr + 0x216) & 3, pc, word, op, u,
                     u ? u[0x18] : 0, u ? u[0xB4] : 0, u ? u[0xB5] : 0);
    }
    g_deny_now = g_deny[op] ? 1 : 0;
    g_loop_now = 0;
    if (op == 0x1F && g_deny_now && g_loop1f) {
        uint8_t* unit = *(uint8_t**)(scr + 0x220);
        uint8_t* ctr = unit ? unit + 0xB4 + ((word & 0x200) ? 0 : (uint8_t)(word >> 12)) : nullptr;
        if (ctr && (uint8_t)(*ctr - 1) != 0) {
            g_loop_pc = (uint32_t)(uintptr_t)(pc + *(int16_t*)(pc + 2));
            g_loop_now = 1;
            g_deny_now = 0;
            InterlockedIncrement(&g_loop_runs);
        }
    }
    if (g_opdiag) InterlockedIncrement(g_deny_now ? &g_opden[g_src][op] : &g_opcnt[g_src][op]);
}

// Mode 1: skip the whole stream loop.
__declspec(naked) static void h_head() {
    __asm {
        cmp byte ptr [g_hold], 1
        je held
        jmp dword ptr [g_head_tramp]
    held:
        jmp dword ptr [g_skip]
    }
}

// Modes 2/3: a counting stream keeps its count.
__declspec(naked) static void h_dec() {
    __asm {
        cmp byte ptr [g_hold], 2
        jb pass
        jmp dword ptr [g_next]
    pass:
        jmp dword ptr [g_dec_tramp]
    }
}

__declspec(naked) static void h_sentry() {
    __asm {
        pushad
        push edi                                 // unit record
        push ebx                                 // scratch
        call on_stream_entry
        add esp, 8
        popad
        cmp byte ptr [g_skip_stream], 0
        jne skip
        jmp dword ptr [g_sentry_tramp]
    skip:
        jmp dword ptr [g_next]
    }
}

__declspec(naked) static void h_next() {
    __asm {
        pushad
        push edi
        push ebx
        call on_stream_next
        add esp, 8
        popad
        jmp dword ptr [g_next_tramp]
    }
}

// Modes 2/3: count the handler; a denied one does not run and the stream yields at it (PC kept, WAIT 0), so the
// next logic walk runs it.
__declspec(naked) static void h_hcall() {
    __asm {
        cmp byte ptr [g_hold], 2
        jb pass
        pushad
        push esi                                 // PC of the opcode
        push ebx
        call on_handler
        add esp, 8
        popad
        cmp byte ptr [g_loop_now], 0
        jne loop1f
        cmp byte ptr [g_deny_now], 0
        je pass
        mov eax, esi
        mov byte ptr [ebx + 0x210], 1
        jmp dword ptr [g_hcall_after]
    loop1f:
        mov eax, dword ptr [g_loop_pc]           // 0x1F jump-back target, count untouched, no yield
        jmp dword ptr [g_hcall_after]
    pass:
        jmp dword ptr [g_hcall_tramp]
    }
}

// Held walk: take the gate's "paused" branch (as when the walker's own +0x212 is set) without writing +0x212: spawn
// handlers read it too and would drop their spawn while the PC advances. AL (unit+0xB2) is left as loaded.
__declspec(naked) static void h_gate() {
    __asm {
        cmp byte ptr [g_hold], 0
        je pass
        cmp byte ptr [g_hold_pos], 0
        je pass
        jmp dword ptr [g_gate_held]
    pass:
        jmp dword ptr [g_gate_tramp]
    }
}

static void __cdecl on_follow(uint32_t type, uint8_t* scr) {
    g_fskip = 0;
    if (type >= 13 || g_ftable[type] != g_fattach) return;
    uint8_t* obj = *(uint8_t**)(scr + 0x220);
    uint16_t id = *(uint16_t*)(obj + 0xA0);
    uint8_t* base = *(uint8_t**)(scr + 0x320);
    uint8_t* t = g_find_unit(scr + 0x300, obj, id);
    if (*(void**)(t + 0xBC)) return;
    g_fskip = 1;
    LONG n = InterlockedIncrement(&g_fskipped);
    if (n <= 20 || n % 600 == 0)
        log_line("X2-MAG-025-FOLLOWGUARD: follow skipped (#%ld): target +0xBC NULL; kind %d hold %d src %s, unit #%d "
                 "id 0x%04X, target id 0x%04X -> #%d%s, group %d/%d",
                 n, pacing_kind(), g_hold, g_src ? "DLL" : "EXE", (int)((obj - base) >> 8), *(uint16_t*)(obj + 0x1A),
                 id, (int)((t - base) >> 8), t == base ? " (not found)" : "", obj[0x1E], t[0x1E]);
}

__declspec(naked) static void h_fcall() {
    __asm {
        pushad
        push ebx                                 // scratch
        movzx eax, al
        push eax                                 // follow type
        call on_follow
        add esp, 8
        popad
        cmp byte ptr [g_fskip], 0
        jne skip
        jmp dword ptr [g_fcall_tramp]
    skip:
        jmp dword ptr [g_fcall_after]
    }
}

static void held_walk(int mode, uint8_t src, uint32_t unit_ctx, uint32_t pass) {
    InterlockedIncrement(&g_held[src]);
    uint8_t saved = g_hold, saved_src = g_src;
    g_hold = (uint8_t)mode;
    g_src = src;
    g_walk_tramp(unit_ctx, pass);
    g_hold = saved;
    g_src = saved_src;
}

static void __cdecl h_walk(uint32_t unit_ctx, uint32_t pass) {
    if (row_MAG025.on && !g_disabled && render_only()) {
        uint8_t* ret = (uint8_t*)_ReturnAddress();
        uint8_t p = (uint8_t)pass;
        if (!in_image(ret)) {
            bool prof = g_mode_list && mag008_list_active();
            int m = prof ? g_mode_list : g_mode_dll;
            if (m && (p == 1 || (p == 0 && (g_pass0 || prof)))) { held_walk(m, 1, unit_ctx, pass); return; }
        } else if (g_mode_exe && (ret == g_exe_ret || (g_exe0_ret && ret == g_exe0_ret))) {
            held_walk(g_mode_exe, 0, unit_ctx, pass);
            return;
        }
    }
    g_walk_tramp(unit_ctx, pass);
}

static const char* mode_name(int m) {
    return m == 0 ? "untouched" : m == 1 ? "skip every stream step" : m == 2 ? "hold WAIT countdowns"
                                                                             : "hold WAIT, run repeating WAIT-0 streams";
}

static bool install() {
    g_entry = sig_find("X2-MAG-025 walker FUN_006bbca0",
        "55 8B EC 81 EC 18 08 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 53 56 0F B7 75 08 57 89 B5 F4 F7 FF FF 83 FE 01 75 06 "
        "89 35 ?? ?? ?? ?? E8", 0x6BBCA0);
    if (!g_entry) return false;
    // cmp byte [ebx+0x21F],0 / jz / cmp byte [ebx+0x21D],0 / jz +0x168 (integration block) / movsx eax,[ebx+0x216] /
    // mov esi,[edi+eax*4]
    g_head = sig_find("X2-MAG-025 stream-loop head",
        "80 BB 1F 02 00 00 00 74 0D 80 BB 1D 02 00 00 00 0F 84 68 01 00 00 0F BF 83 16 02 00 00 8B 34 87", 0x6BC170);
    if (!g_head) return false;
    struct { uint8_t* at; const char* sig; const char* what; } checks[] = {
        { g_head + 0x62,  "0F BF 8B 16 02 00 00 8A 44 39 10 84 C0 0F 85", "stream entry" },
        { g_head + 0xC9,  "56 53 FF D0 83 C4 08 80 BB 10 02 00 00 00 8B F0", "handler call" },
        { g_head + 0x156, "FE C8 88 44 39 10 66 FF 83 16 02 00 00", "WAIT decrement" },
        // inc word [ebx+0x216] / cmp word [ebx+0x216],4 / jl head / cmp byte [ebx+0x21D],0 / jnz
        { g_head + 0x15C, "66 FF 83 16 02 00 00 66 83 BB 16 02 00 00 04 0F 8C 8F FE FF FF 80 BB 1D 02 00 00 00 0F 85",
          "stream-loop tail" },
        // mov al,[edi+0xB2] / test al,2 / jnz / cmp word [ebx+0x212],0 / jnz
        { g_head + 0x17E, "8A 87 B2 00 00 00 A8 02 0F 85 ?? ?? ?? ?? 66 83 BB 12 02 00 00 00 0F 85", "integration gate" },
    };
    for (auto& c : checks)
        if (!sig_match(c.at, c.sig)) {
            log_line("  X2-MAG-025: %s at gva 0x%08X does not match - row off", c.what, rt_to_gva(c.at));
            return false;
        }
    g_sentry = g_head + 0x62;
    g_hcall = g_head + 0xC9;
    g_hcall_after = g_hcall + 7;
    g_dec = g_head + 0x156;
    g_next = g_head + 0x15C;
    g_skip = g_head + 0x171;
    g_gate = g_head + 0x18C;
    uint8_t* held = g_gate + 8 + 6 + *(int32_t*)(g_gate + 10);   // the gate's jnz rel32 target
    if (!sig_match(held, "A8 04 75")) {                          // test al,4 / jnz (follow skip)
        log_line("  X2-MAG-025: gate target gva 0x%08X is not 'test al,4' - row off", rt_to_gva(held));
        return false;
    }
    g_gate_held = held;

    g_followguard = config_row("X2-MAG-025-FOLLOWGUARD", 1);
    if (g_followguard) {
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
            g_followguard = 0;
        } else {
            g_fcall = site + 4;
            g_fcall_after = site + 0x15;
            g_ftable = (uint8_t**)table;
            g_fattach = fn;
            g_find_unit = (find_unit_t)find;
            log_line("  X2-MAG-025-FOLLOWGUARD: follow call gva=0x%08X, table gva=0x%08X, attach handler gva=0x%08X, "
                     "unit lookup gva=0x%08X", rt_to_gva(g_fcall), rt_to_gva(table), rt_to_gva(fn), rt_to_gva(find));
        }
    }
    g_mode_exe = config_row("X2-MAG-025-MODE", 3);
    g_mode_dll = config_row("X2-MAG-025-DLLMODE", 3);
    if (g_mode_exe < 0 || g_mode_exe > 3) g_mode_exe = 3;
    if (g_mode_dll < 0 || g_mode_dll > 3) g_mode_dll = 3;
    g_mode_list = config_row("X2-MAG-025-LISTMODE", 2);
    if (g_mode_list < 0 || g_mode_list > 3) g_mode_list = 2;
    if (config_row("X2-MAG-025-EXE", 1) == 0) g_mode_exe = 0;
    if (g_mode_exe) {
        // FUN_006a5d30: push / call FUN_0064f580 / push 1 / push 0 / call walker / mov esi,[..] / push 0 / mov [..],0
        uint8_t* site = sig_find("X2-MAG-025 EXE pass-1 walk call",
            "68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 01 6A 00 E8 ?? ?? ?? ?? 8B 35 ?? ?? ?? ?? 6A 00 C7 05", 0x6A5D72);
        if (!site || call_target(site + 0xE) != g_entry) {
            log_line("  X2-MAG-025: EXE pass-1 walk call not found - EXE walks untouched");
            g_mode_exe = 0;
        } else {
            g_exe_ret = site + 0x13;
        }
    }
    if (g_mode_exe && config_row("X2-MAG-025-EXEPASS0", 1)) {
        // Fx_OpuUpdate: mov [0x1b41b9c],eax / push 0 / push 0 / call walker / add esp,8 / cmp [esi+0x58],0 / jnz +8
        uint8_t* s0 = sig_find("X2-MAG-025 EXE pass-0 walk call",
            "A3 ?? ?? ?? ?? 6A 00 6A 00 E8 ?? ?? ?? ?? 83 C4 08 83 7E 58 00 75 08", 0x6A5CFF);
        if (s0 && call_target(s0 + 9) == g_entry) g_exe0_ret = s0 + 0xE;
        else log_line("  X2-MAG-025-EXEPASS0: EXE pass-0 walk call not found - untouched");
    }
    g_pass0 = config_row("X2-MAG-025-PASS0", 0);
    g_hold_pos = config_row("X2-MAG-025-POS", 1) ? 1 : 0;
    g_opdiag = config_row("X2-MAG-025-OPDIAG", 0);
    g_loop1f = config_row("X2-MAG-025-LOOP1F", 1);
    g_optrace = config_row("X2-MAG-025-OPTRACE", 0);
    char deny[256];
    config_str("rows", "X2-MAG-025-DENY", deny, sizeof(deny), "1F,7F");   // loop counter, battle signal
    char line[256]; int k = 0; line[0] = 0;
    for (char* t = deny; *t;) {
        char* e = nullptr;
        unsigned long op = strtoul(t, &e, 16);
        if (e == t) { t++; continue; }
        if (op < 512) { g_deny[op] = true; k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " 0x%02lX", op); }
        t = e;
    }
    if (line[0]) log_line("  X2-MAG-025: opcodes held on render-only invocations (X2-MAG-025-DENY, modes 2/3):%s", line);
    log_line("  X2-MAG-025-LOOP1F: %s", g_loop1f && g_deny[0x1F] ? "0x1F loops jump back uncounted on held walks (last count yields)" : "off");
    if (g_exe0_ret) log_line("  X2-MAG-025-EXEPASS0: EXE pass-0 walk (return gva 0x%08X, expected 0x006A5D0D) held too",
                             rt_to_gva(g_exe0_ret));
    log_line("  X2-MAG-025: EXE pass-1 walk (return gva 0x%08X, expected 0x006A5D85): mode %d, %s",
             g_exe_ret ? rt_to_gva(g_exe_ret) : 0, g_mode_exe, mode_name(g_mode_exe));
    log_line("  X2-MAG-025: DLL-called walks (pass 1%s): mode %d, %s; list profile (X2-MAG-008-PASS-LIST): mode %d, pass 0 held",
             g_pass0 ? " and 0" : "", g_mode_dll, mode_name(g_mode_dll), g_mode_list);
    log_line("  X2-MAG-025: walker gva=0x%08X, stream head gva=0x%08X, held gate target gva=0x%08X (expected 0x006BC406); "
             "position hold %s", rt_to_gva(g_entry), rt_to_gva(g_head), rt_to_gva(g_gate_held), g_hold_pos ? "ON" : "OFF");
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    struct { const char* name; void* at; void* detour; void** tramp; } hooks[] = {
        { "X2-MAG-025 head",    g_head,   (void*)&h_head,   &g_head_tramp },
        { "X2-MAG-025 entry",   g_sentry, (void*)&h_sentry, &g_sentry_tramp },
        { "X2-MAG-025 handler", g_hcall,  (void*)&h_hcall,  &g_hcall_tramp },
        { "X2-MAG-025 dec",     g_dec,    (void*)&h_dec,    &g_dec_tramp },
        { "X2-MAG-025 next",    g_next,   (void*)&h_next,   &g_next_tramp },
        { "X2-MAG-025 gate",    g_gate,   (void*)&h_gate,   &g_gate_tramp },
        { "X2-MAG-025",         g_entry,  (void*)&h_walk,   (void**)&g_walk_tramp },
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
    if (g_followguard && hook_entry("X2-MAG-025-FOLLOWGUARD", g_fcall, (void*)&h_fcall, &g_fcall_tramp))
        log_line("X2-MAG-025-FOLLOWGUARD: follow call hooked");
}

static void log_ops(int src) {
    char line[1024]; int k = 0; line[0] = 0;
    for (int op = 0; op < 512; op++) {
        LONG n = InterlockedExchange(&g_opcnt[src][op], 0), d = InterlockedExchange(&g_opden[src][op], 0);
        if (n) k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %02X:%ld", op, n);
        if (d) k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %02X:held%ld", op, d);
    }
    if (line[0]) log_line("X2-MAG-025 opdiag %s: held-walk handlers (op:count):%s", src ? "DLL" : "EXE", line);
}

static void post() {
    static uint32_t tick = 0;
    if (g_opdiag && tick % 30 == 29) { log_ops(0); log_ops(1); }
    static LONG reported[2] = {0, 0};
    for (int s = 0; s < 2; s++)
        if (!reported[s] && g_held[s] > 0) {
            reported[s] = 1;
            log_line("X2-MAG-025: first held %s walk on a render-only invocation", s ? "DLL" : "EXE");
        }
    static LONG last = 0;
    LONG total = g_held[0] + g_held[1];
    if (++tick % 1800 == 0 && total != last) {
        last = total;
        log_line("X2-MAG-025: held walks so far: EXE %ld, DLL %ld; WAIT-0 streams on mode-3 walks: %ld ran, %ld held; "
                 "0x1F loops run uncounted %ld", g_held[0], g_held[1], g_ro_run, g_ro_blocked, g_loop_runs);
    }
}

Row row_MAG025 = {
    "X2-MAG-025",
    "battle effect-unit walks (EXE pass 1, magic-DLL draws): render-only invocations draw without stepping",
    1, install, pre, post, 0
};

} // namespace x2
