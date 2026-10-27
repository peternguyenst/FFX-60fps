// SPDX-License-Identifier: Apache-2.0
// Battle effect units (command scripts, OPU) step on every walk of OpuMain_UnitBytecodeWalk: the EXE walks pass 1
// (FUN_00800670, second loop) on every invocation, and magic DLLs call the walker (service slot +0xB30) from their
// draw. WAIT countdowns, script handlers (battle signals, actor motion requests, motion-end polls) and unit motion
// run 2x at 60 fps (Tidus's Mental Break: the signal that ends his wait loop came 3 ticks early). Render-only
// invocations hold these walks, per walk source (EXE pass-1 call, DLL calls), in one of three modes:
//   1: jump from the stream-loop head to its exit test: no stream steps, the unit's draw-type call still runs.
//   2: keep every WAIT countdown; streams at WAIT 0 run their handlers.
//   3: as 2, but a WAIT-0 stream runs only if its last logic walk ran handlers and yielded at the PC it started
//      from (per-frame draw/poll loops: the battle-entry shatter blinked without them).
// Position integration is held in every mode (MAG-025-POS); listed opcodes never run (MAG-025-DENY, modes 2/3).
// Details: docs/fps60/rows/MAG-025.md
#include "../fps60.h"
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>

typedef void(__cdecl* walk_t)(uint32_t unit_ctx, uint32_t pass);

// Walker sites (Ghidra VAs; all resolved from the stream-loop head signature and verified)
static uint8_t* g_entry = nullptr;               // OpuMain_UnitBytecodeWalk 0x80CC00
static uint8_t* g_head = nullptr;                // 0x80D4B0 stream-loop head
static uint8_t* g_sentry = nullptr;              // 0x80D528 stream handler entry (after the latch test)
static uint8_t* g_hcall = nullptr;               // 0x80D586 push edi / push esi / call eax (handler) / add esp,8
static uint8_t* g_dec = nullptr;                 // 0x80D5F4 WAIT > 0 path: dec al / mov [ecx+ebx+0x10],al
static void*    g_next = nullptr;                // 0x80D5FA next stream
static void*    g_skip = nullptr;                // 0x80D60F after the 4-stream loop (sweep-2 test, then draw)
static uint8_t* g_gate = nullptr;                // 0x80D69E movsx ecx,word [esi+0x212] (position integration gate)
static void*    g_hcall_after = nullptr;         // 0x80D58D yield test
static void*    g_gate_after = nullptr;          // 0x80D6A5
static walk_t   g_walk_tramp = nullptr;
static void*    g_head_tramp = nullptr;
static void*    g_sentry_tramp = nullptr;
static void*    g_hcall_tramp = nullptr;
static void*    g_dec_tramp = nullptr;
static void*    g_next_tramp = nullptr;
static void*    g_gate_tramp = nullptr;
static uint8_t* g_exe_ret = nullptr;             // return address of the EXE pass-1 walk call (0x8006C4)
static bool     g_patched = false;

// MAG-025-FOLLOWGUARD: follow call 0x80D994 (movzx eax,al / push edi / mov eax,[eax*4+0xC492C8] / push esi /
// call eax), after the integration block, runs on every walk. Follow type handler FUN_00815670 resolves the target
// unit (FUN_00801060, pure) and reads [target+0xBC] without a check: magic_0169 (Impulse) crashed there on a
// render-only DLL walk, target +0xBC NULL (13/10/2026). The guard skips that one call when the pointer is NULL.
typedef uint8_t*(__cdecl* find_unit_t)(uint8_t* ctx, uint8_t* obj, uint16_t id);
static uint8_t*    g_fcall = nullptr;            // 0x80D994
static void*       g_fcall_after = nullptr;      // 0x80D9A5 inc word [esi+0x218]
static void*       g_fcall_tramp = nullptr;
static uint8_t**   g_ftable = nullptr;           // 0xC492C8 follow handler table (12 entries)
static uint8_t*    g_fattach = nullptr;          // FUN_00815670
static find_unit_t g_find_unit = nullptr;        // FUN_00801060
static int         g_followguard = 1;            // MAG-025-FOLLOWGUARD
static volatile uint8_t g_fskip = 0;
static volatile LONG g_fskipped = 0;

// [rows]
static int g_mode_exe = 3;                       // MAG-025-MODE: EXE pass-1 walk (0 = untouched)
static int g_mode_dll = 3;                       // MAG-025-DLLMODE: DLL-called walks (0 = untouched)
static int g_pass0 = 0;                          // MAG-025-PASS0: hold DLL pass-0 walks too (MAG-021 replays)
static bool g_pass0_now = false;                 // MAG-032: hold DLL pass-0 walks inside its replay only
static uint8_t g_hold_pos = 1;                   // MAG-025-POS
static bool g_deny[512];                         // MAG-025-DENY=hex,hex (opcode word & 0x1FF; bit 8 = DLL table)
static int  g_opdiag = 0;                        // MAG-025-OPDIAG: held-walk handler counts every 30 invocations

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
    if (scr[0x21D]) rec[0x1F] |= (uint8_t)(1 << st);   // sweep 2 cleared the latch bit before this point
    g_skip_stream = 1;
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

// MAG-025-ALLOW (above 60 fps only, MAG-025-ALLOW-ON): held walks run only the listed opcodes, every other one waits
// for the logic walk. At 4-8 passes a deny list leaked whatever it did not name (0xDC, 0x2B, ...: allocations, spawns,
// counters that ran once per pass). No OPU handler draws (the OPP walk does), so holding one only delays a state change.
static bool g_allow[512];
static int  g_allow_on = 0;

static void __cdecl on_handler(uint8_t* scr) {
    int op = *(uint16_t*)(scr + 0x21A) & 0x1FF;
    g_deny_now = (g_deny[op] || (g_allow_on && rate_mult() > 2 && !g_allow[op])) ? 1 : 0;
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
        push ebx
        push esi
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
        push ebx
        push esi
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
        push esi
        call on_handler
        add esp, 4
        popad
        cmp byte ptr [g_deny_now], 0
        je pass
        mov eax, edi
        mov byte ptr [esi + 0x210], 1
        jmp dword ptr [g_hcall_after]
    pass:
        jmp dword ptr [g_hcall_tramp]
    }
}

// Held walk: the integration gate reads "paused" (the walker's own +0x212 test), without setting +0x212 itself:
// spawn handlers 0x32/0x5D read it too and would drop their spawn while the PC advances.
__declspec(naked) static void h_gate() {
    __asm {
        cmp byte ptr [g_hold], 0
        je pass
        cmp byte ptr [g_hold_pos], 0
        je pass
        mov ecx, 1
        jmp dword ptr [g_gate_after]
    pass:
        jmp dword ptr [g_gate_tramp]
    }
}

static void __cdecl on_follow(uint32_t type, uint8_t* unit) {
    g_fskip = 0;
    if (g_ftable[type] != g_fattach) return;
    uint8_t* obj = *(uint8_t**)(unit + 0x220);
    uint16_t id = *(uint16_t*)(obj + 0xA0);
    uint8_t* base = *(uint8_t**)(unit + 0x320);
    uint8_t* t = g_find_unit(unit + 0x300, obj, id);
    if (*(void**)(t + 0xBC)) return;
    g_fskip = 1;
    LONG n = InterlockedIncrement(&g_fskipped);
    if (n <= 20 || n % 600 == 0)
        log_line("MAG-025-FOLLOWGUARD: follow skipped (#%ld): target +0xBC NULL; kind %d hold %d src %s, unit #%d id 0x%04X, "
                 "target id 0x%04X -> #%d%s, group %d/%d",
                 n, pacing_kind(), g_hold, g_src ? "DLL" : "EXE", (int)((obj - base) >> 8), *(uint16_t*)(obj + 0x1A),
                 id, (int)((t - base) >> 8), t == base ? " (not found)" : "", obj[0x1E], t[0x1E]);
}

__declspec(naked) static void h_fcall() {
    __asm {
        pushad
        push esi
        movzx eax, al
        push eax
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
    if (row_MAG025.on && !g_disabled && pacing_kind() == INV_BUILD) {
        uint8_t* ret = (uint8_t*)_ReturnAddress();
        uint8_t* base = exe_base();
        uint8_t p = (uint8_t)pass;
        if (ret < base || ret >= base + exe_size()) {
            if (g_mode_dll && (p == 1 || (p == 0 && (g_pass0 || g_pass0_now)))) { held_walk(g_mode_dll, 1, unit_ctx, pass); return; }
        } else if (g_mode_exe && ret == g_exe_ret) {
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
    g_entry = sig_find_unique("MAG-025 OpuMain_UnitBytecodeWalk",
        "55 8B EC 83 EC 28 A1 ?? ?? ?? ?? 33 C5 89 45 FC 53 56 57 0F B7 7D 08 83 FF 01 75 06 89 3D ?? ?? ?? ?? 68 00 04 00 00 E8",
        0x80CC00);
    if (!g_entry) return false;
    // cmp byte [esi+0x21F],0 / jz / cmp byte [esi+0x21D],0 / jz +0x156 (to the integration block) / movsx
    g_head = sig_find_unique("MAG-025 stream-loop head",
        "80 BE 1F 02 00 00 00 74 0D 80 BE 1D 02 00 00 00 0F 84 56 01 00 00 0F BF", 0x80D4B0);
    if (!g_head) return false;
    struct { uint8_t* at; const char* sig; const char* what; } checks[] = {
        { g_head + 0x78,  "0F BF 8E 16 02 00 00 8A 44 19 10", "stream entry" },
        { g_head + 0xD6,  "57 56 FF D0 83 C4 08 80 BE 10 02 00 00 00 8B F8", "handler call" },
        { g_head + 0x144, "FE C8 88 44 19 10", "WAIT decrement" },
        // inc word [esi+0x216] / cmp word [esi+0x216],4 / jl head / cmp byte [esi+0x21D],0 / jnz
        { g_head + 0x14A, "66 FF 86 16 02 00 00 66 83 BE 16 02 00 00 04 0F 8C A1 FE FF FF 80 BE 1D 02 00 00 00 0F 85",
          "stream-loop tail" },
        { g_head + 0x1EE, "0F BF 8E 12 02 00 00 89 0D", "integration gate" },
    };
    for (auto& c : checks)
        if (!sig_match(c.at, c.sig)) {
            log_line("  MAG-025: %s at gva 0x%08X does not match - row off", c.what, rt_to_gva_exe(c.at));
            return false;
        }
    g_sentry = g_head + 0x78;
    g_hcall = g_head + 0xD6;
    g_hcall_after = g_hcall + 7;
    g_dec = g_head + 0x144;
    g_next = g_head + 0x14A;
    g_skip = g_head + 0x15F;
    g_gate = g_head + 0x1EE;
    g_gate_after = g_gate + 7;

    g_followguard = config_row("MAG-025-FOLLOWGUARD", 1);
    if (g_followguard) {
        // mov al,[ebx+0xBA] / test al,al / jz / cmp al,0xC / jnb / movzx eax,al / push edi / mov eax,[eax*4+table] /
        // push esi / call eax / add esp,8 / inc word [esi+0x218]
        uint8_t* site = sig_find_unique("MAG-025-FOLLOWGUARD follow call",
            "8A 83 BA 00 00 00 84 C0 74 15 3C 0C 73 11 0F B6 C0 57 8B 04 85 ?? ?? ?? ?? 56 FF D0 83 C4 08 66 FF 86 18 02 00 00",
            0x80D986);
        // FUN_00815670: obj = [unit+0x220]; t = FUN_00801060(unit+0x300, obj, obj+0xA0); esi = [t+0xBC]; movsx eax,[esi]
        uint8_t* fn = sig_find_unique("MAG-025-FOLLOWGUARD attach handler",
            "55 8B EC 53 8B 5D 08 56 57 8B BB 20 02 00 00 0F B7 87 A0 00 00 00 50 8D 83 00 03 00 00 57 50 E8 ?? ?? ?? ?? "
            "83 C4 0C 8B B0 BC 00 00 00 0F BF 06", 0x815670);
        uint8_t* find = fn ? call_target(fn + 0x1F) : nullptr;
        if (!site || !find || !sig_match(find, "55 8B EC 56 8B 75 0C 57 66 8B 7D 10 66 85 FF 79 4F")) {
            log_line("  MAG-025-FOLLOWGUARD: sites not found - guard off");
            g_followguard = 0;
        } else {
            g_fcall = site + 0xE;
            g_fcall_after = site + 0x1F;
            g_ftable = (uint8_t**)read_abs(site + 0x15);
            g_fattach = fn;
            g_find_unit = (find_unit_t)find;
            log_line("  MAG-025-FOLLOWGUARD: follow call gva=0x%08X, table gva=0x%08X, attach handler gva=0x%08X, "
                     "unit lookup gva=0x%08X", rt_to_gva_exe(g_fcall), rt_to_gva_exe((uint8_t*)g_ftable),
                     rt_to_gva_exe(fn), rt_to_gva_exe(find));
        }
    }
    g_mode_exe = config_row("MAG-025-MODE", 3);
    g_mode_dll = config_row("MAG-025-DLLMODE", 3);
    if (g_mode_exe < 0 || g_mode_exe > 3) g_mode_exe = 3;
    if (g_mode_dll < 0 || g_mode_dll > 3) g_mode_dll = 3;
    if (config_row("MAG-025-EXE", 1) == 0) g_mode_exe = 0;
    if (g_mode_exe) {
        // FUN_00800670: push / call FUN_007bba50 / push 1 / push 0 / call walker / push 0 / call OppMain_EffectWalk
        uint8_t* site = sig_find_unique("MAG-025 EXE pass-1 walk call",
            "68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 01 6A 00 E8 ?? ?? ?? ?? 6A 00 E8", 0x8006B1);
        if (!site || call_target(site + 0xE) != g_entry) {
            log_line("  MAG-025: EXE pass-1 walk call not found - EXE walks untouched");
            g_mode_exe = 0;
        } else {
            g_exe_ret = site + 0x13;
        }
    }
    g_pass0 = config_row("MAG-025-PASS0", 0);
    g_hold_pos = config_row("MAG-025-POS", 1) ? 1 : 0;
    g_opdiag = config_row("MAG-025-OPDIAG", 0);
    char deny[256];
    config_str("rows", "MAG-025-DENY", deny, sizeof(deny), "1F,7F,32,5D,8F");   // loop counter, battle signal, unit spawns, afterimage append
    char line[256]; int k = 0; line[0] = 0;
    for (char* t = deny; *t;) {
        char* e = nullptr;
        unsigned long op = strtoul(t, &e, 16);
        if (e == t) { t++; continue; }
        if (op < 512) { g_deny[op] = true; k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " 0x%02lX", op); }
        t = e;
    }
    if (line[0]) log_line("  MAG-025: opcodes held on render-only invocations (MAG-025-DENY, modes 2/3):%s", line);
    // Allow list above 60 fps: 02 jump, 09 yield (loop shape), 53 transform from target, 61 copy from a unit (both
    // recompute from other state: the same result on every call).
    g_allow_on = config_row("MAG-025-ALLOW-ON", 0);   // off: holding 0xFF etc. blinked effects (U 25/10)
    config_str("rows", "MAG-025-ALLOW", deny, sizeof(deny), "02,09,53,61");
    k = 0; line[0] = 0;
    for (char* t = deny; *t;) {
        char* e = nullptr;
        unsigned long op = strtoul(t, &e, 16);
        if (e == t) { t++; continue; }
        if (op < 512) { g_allow[op] = true; k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " 0x%02lX", op); }
        t = e;
    }
    log_line("  MAG-025: above 60 fps held walks run only (MAG-025-ALLOW%s):%s", g_allow_on ? "" : ", OFF by MAG-025-ALLOW-ON=0",
             line);
    log_line("  MAG-025: EXE pass-1 walk (return gva 0x%08X): mode %d, %s", g_exe_ret ? rt_to_gva_exe(g_exe_ret) : 0,
             g_mode_exe, mode_name(g_mode_exe));
    log_line("  MAG-025: DLL-called walks (pass 1%s): mode %d, %s", g_pass0 ? " and 0" : "", g_mode_dll,
             mode_name(g_mode_dll));
    log_line("  MAG-025: walker gva=0x%08X, stream head gva=0x%08X; position hold %s",
             rt_to_gva_exe(g_entry), rt_to_gva_exe(g_head), g_hold_pos ? "ON" : "OFF");
    return true;
}

static void pre() {
    if (g_patched) return;
    g_patched = true;
    struct { const char* name; void* at; void* detour; void** tramp; } hooks[] = {
        { "MAG-025 head",    g_head,   (void*)&h_head,   &g_head_tramp },
        { "MAG-025 entry",   g_sentry, (void*)&h_sentry, &g_sentry_tramp },
        { "MAG-025 handler", g_hcall,  (void*)&h_hcall,  &g_hcall_tramp },
        { "MAG-025 dec",     g_dec,    (void*)&h_dec,    &g_dec_tramp },
        { "MAG-025 next",    g_next,   (void*)&h_next,   &g_next_tramp },
        { "MAG-025 gate",    g_gate,   (void*)&h_gate,   &g_gate_tramp },
        { "MAG-025",         g_entry,  (void*)&h_walk,   (void**)&g_walk_tramp },
    };
    int n = (int)(sizeof(hooks) / sizeof(hooks[0]));
    for (int i = 0; i < n; i++) {
        if (hook_entry(hooks[i].name, hooks[i].at, hooks[i].detour, hooks[i].tramp)) continue;
        while (--i >= 0) hook_remove(hooks[i].name, hooks[i].at);
        row_MAG025.on = 0;
        return;
    }
    log_line("MAG-025: walker entry and 6 inner sites hooked on the game thread");
    if (g_followguard && hook_entry("MAG-025-FOLLOWGUARD", g_fcall, (void*)&h_fcall, &g_fcall_tramp))
        log_line("MAG-025-FOLLOWGUARD: follow call hooked");
}

bool mag025_dll_hold_ready() { return row_MAG025.on && g_patched && g_mode_dll != 0 && !g_disabled; }
void mag025_hold_pass0(bool on) { g_pass0_now = on; }

static void log_ops(int src) {
    char line[1024]; int k = 0; line[0] = 0;
    for (int op = 0; op < 512; op++) {
        LONG n = InterlockedExchange(&g_opcnt[src][op], 0), d = InterlockedExchange(&g_opden[src][op], 0);
        if (n) k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %02X:%ld", op, n);
        if (d) k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %02X:held%ld", op, d);
    }
    if (line[0]) log_line("MAG-025 opdiag %s: held-walk handlers (op:count):%s", src ? "DLL" : "EXE", line);
}

static void post() {
    static uint32_t tick = 0;
    if (g_opdiag && tick % 30 == 29) { log_ops(0); log_ops(1); }
    static LONG reported[2] = {0, 0};
    for (int s = 0; s < 2; s++)
        if (!reported[s] && g_held[s] > 0) {
            reported[s] = 1;
            log_line("MAG-025: first held %s walk on a render-only invocation", s ? "DLL" : "EXE");
        }
    static LONG last = 0;
    LONG total = g_held[0] + g_held[1];
    if (++tick % 1800 == 0 && total != last) {
        last = total;
        log_line("MAG-025: held walks so far: EXE %ld, DLL %ld; WAIT-0 streams on mode-3 walks: %ld ran, %ld held",
                 g_held[0], g_held[1], g_ro_run, g_ro_blocked);
    }
}

Row row_MAG025 = {
    "MAG-025",
    "battle effect-unit walks (EXE pass 1, magic-DLL draws): render-only invocations draw without stepping",
    1, install, pre, post, 0
};
