// SPDX-License-Identifier: Apache-2.0
// X2-MAG-030 (FFX-2 only): the "T" magic overlays (71 magic_*.dll files, magic_0718 shape) run a record driver from
// a script task (service +0x60C -> task group 0x40, run by FUN_00680980 in Sg_MainLoop's second loop, so on every
// invocation). Each record starts (init callback, once) when its start frame <= the driver clock; the clock advances
// at the end of a call only when the exec getter +0x400 is non-zero. Stock exec is always 1, so a record starts on the
// tick after the clock reaches it, after that tick's overlay update re-polls the script flags (+0x420). At 60 Hz the
// render-only invocation right after a logic one starts it half a tick early, with flags polled before the script set
// them: magic_0718's init FUN_10001220 then finds no variant flag and writes through a null object (crash, D 16/10/2026
// 17:39:31, CoreDump.dmp: magic_0718_us.dll+0x1418 MOV [EAX+0x130],1, EAX=0, map 0x91).
// Fix: on render-only invocations the driver runs with its clock one tick back (then restored), so no record starts
// or ends there; records already running are called as before.
// Evidence: outer repo re/ledger/MAG-TASK.md (S); offline scan of magicFiles/FFX-2: 71 DLLs, 1 hit each, all checks.
// X2-MAG-030-DMG (default 1): that render-only re-run calls running records' callbacks with the same record frame as the
// logic call before it, so frame-keyed one-shot actions run twice; for damage that uses up the multi-hit budget and the
// later hits do nothing (U 17/10/2026: Tidus's Blitz Ace, magic_0749, half the hits had no number and no reaction; D:
// X2-DIAG-HIT, hit checks in L/R pairs; S: magic_0749 FUN_10002e30 `if (rec+4 == 0x3E) svc+0x3CC(...)`). While the
// re-run is active, ms_set_damage FUN_0061b150 (service +0x3CC) returns 0 without applying. Outer repo re/ledger/DMGNUM.md.
#include "../x2.h"
#include "../../../third_party/minhook/include/MinHook.h"
#include <string.h>

namespace x2 {

extern Row row_MAG030;

// ---------------------------------------------------------------- signatures (magic_0718_us Ghidra VAs, base 0x10000000)
// Driver FUN_10003870: __fastcall (ecx passed on to +0x400), no stack args, plain RETs (+0x27, +0xFF, +0x10B).
static const char SIG_ENTRY[] =     // entry: exec getter +0x400 -> [exec], live count test, record loop setup
    "55 8B EC 51 A1 ?? ?? ?? ?? 8B 80 00 04 00 00 FF D0 83 3D ?? ?? ?? ?? 00 A3 ?? ?? ?? ?? 75 09 B8 01 00 00 00 "
    "8B E5 5D C3 53 56 33 DB 57 BE ?? ?? ?? ?? 39 1D ?? ?? ?? ?? 0F 8E";
static const char SIG_START[] =     // entry+0x44: record test, start != -1, CMP start,[clock] (operand +0x5E)
    "83 C7 0C 33 D2 89 55 FC 39 16 0F 84 ?? ?? ?? ?? 8B 47 F8 83 F8 FF 74 ?? 3B 05";
static const char SIG_TAIL[] =      // entry+0xE8: if ([exec]) INC [clock] (operand +0xF3); plain RET
    "83 3D ?? ?? ?? ?? 00 74 06 FF 05 ?? ?? ?? ?? 5F 5E 33 C0 5B 8B E5 5D C3";
enum { OFF_SVC = 5, OFF_EXEC = 0x19, OFF_START = 0x44, OFF_CLOCK = 0x5E, OFF_TAIL = 0xE8, OFF_TAIL_EXEC = 0xEA, OFF_TAIL_CLOCK = 0xF3 };
// X2-MAG-030-B (default 1): the second driver shape (magic_0760 FUN_10003680; 11 DLLs: magic_0760 and magic_0797 with its
// 9 language copies; offline scan of magicFiles/FFX-2: 1 hit each, all checks; A + B = all 82 exec-getter drivers). It
// loads the record table into EDI first and indexes it +4 / +8 / +0xC / +0x10 / +0x14 (A: ADD EDI,0xC, then -8 ..):
// same record logic, same clock / exec use. U 18/10/2026: the game closed while a commsphere recording was viewed, twice;
// D (X2-DIAG-CRASH): magic_0760+0x12D6 MOV [EAX+0x130],1 with EAX = 0 on a render-only invocation, from the driver's
// record init (+0x36F4): the same defect as the magic_0718 crash above. Outer repo re/ledger/MAG-TASK.md §B.
static const char SIG_ENTRY_B[] =   // as A, plus MOV EDI,[record table] before MOV ESI,records
    "55 8B EC 51 A1 ?? ?? ?? ?? 8B 80 00 04 00 00 FF D0 83 3D ?? ?? ?? ?? 00 A3 ?? ?? ?? ?? 75 09 B8 01 00 00 00 "
    "8B E5 5D C3 53 56 33 DB 57 8B 3D ?? ?? ?? ?? BE ?? ?? ?? ?? 39 1D ?? ?? ?? ?? 0F 8E";
static const char SIG_START_B[] =   // entry+0x44: record test (short JE), start != -1, CMP start,[clock] (operand +0x57)
    "33 D2 89 55 FC 39 16 74 ?? 8B 47 04 83 F8 FF 74 ?? 3B 05";
// X2-MAG-030-C (default 1): the third shape (magic_0740 FUN_10005570; 10 DLLs: 0614 0631 0740 0741 0759 0770 0799 0808
// 0872 0923; offline scan: 1 hit each, all checks, field offsets identical; a loose scan for the record start/end tests
// finds no driver outside A/B/C). The timeline is an object in ECX (thiscall): clock [obj+0], record count [obj+4], live
// count [obj+8], records obj+0xC (stride 0x74), record table [obj+0x74C], exec [obj+0x750]; same record logic, and the
// tail INC [obj] runs only when exec != 0. U 18/10/2026: the game closed while another commsphere recording was viewed;
// D (X2-DIAG-CRASH): magic_0740+0x12E0 write [0 + 0x130] on a render-only invocation from the driver's record init
// (+0x55E4). The clock is found through ECX at each call.
static const char SIG_ENTRY_C[] =   // PUSH EBP; MOV EBP,ESP; MOV EAX,[svc]; SUB ESP,8; MOV EAX,[EAX+0x400]; PUSH EBX; MOV EBX,ECX;
    // CALL EAX; CMP [EBX+8],0; MOV [EBX+exec],EAX; JNE; MOV EAX,1; POP EBX; leave; RET; CMP [EBX+4],0; PUSH ESI; PUSH EDI;
    // MOV EDI,[EBX+table]; LEA ESI,[EBX+0xC]
    "55 8B EC A1 ?? ?? ?? ?? 83 EC 08 8B 80 00 04 00 00 53 8B D9 FF D0 83 7B 08 00 89 83 ?? ?? ?? ?? 75 0A B8 01 00 00 00 "
    "5B 8B E5 5D C3 83 7B 04 00 56 57 8B BB ?? ?? ?? ?? 8D 73 0C";
static const char SIG_START_C[] =   // entry+0x48: record test, start != -1, CMP start,[EBX] (the clock), JG
    "33 C9 89 4D F8 39 0E 74 ?? 8B 47 04 83 F8 FF 74 ?? 3B 03 7F";
static const char SIG_TAIL_C[] =    // entry+0xD5: if ([EBX+exec]) INC [EBX]; plain RET
    "83 BB ?? ?? ?? ?? 00 74 02 FF 03 5F 5E 33 C0 5B 8B E5 5D C3";
// Per shape: operand offsets (svc table, exec, clock) at the entry and the start test / tail checks. clock < 0: the clock
// is [ECX] (shape C, exec is then an ECX field offset).
struct Shape { const char* entry; const char* start; const char* tail_sig; int svc, exec, start_off, clock, tail, tail_exec, tail_clock; const char* tag; };
static const Shape SHAPES[3] = {
    { SIG_ENTRY,   SIG_START,   SIG_TAIL,   OFF_SVC, OFF_EXEC, OFF_START, OFF_CLOCK, OFF_TAIL, OFF_TAIL_EXEC, OFF_TAIL_CLOCK, "A" },
    { SIG_ENTRY_B, SIG_START_B, SIG_TAIL,   OFF_SVC, OFF_EXEC, OFF_START, 0x57,      0xD9,     0xDB,          0xE4,           "B" },
    { SIG_ENTRY_C, SIG_START_C, SIG_TAIL_C, 4,       0x1C,     0x48,      -1,        0xD5,     0xD7,          -1,             "C" },
};
static int g_opt_shape[3] = { 1, 1, 1 };

// ---------------------------------------------------------------- local scanner (DLL executable sections)
struct BytePattern { uint8_t b[64]; uint8_t m[64]; int len; };

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool parse_pattern(const char* s, BytePattern* p) {
    p->len = 0;
    while (*s) {
        if (*s == ' ') { s++; continue; }
        if (p->len >= 64) return false;
        if (s[0] == '?' && s[1] == '?') { p->b[p->len] = 0; p->m[p->len] = 0; }
        else {
            int hi = hex_digit(s[0]), lo = hex_digit(s[1]);
            if (hi < 0 || lo < 0) return false;
            p->b[p->len] = (uint8_t)(hi * 16 + lo); p->m[p->len] = 1;
        }
        p->len++;
        s += 2;
    }
    return p->len > 0;
}

static bool pattern_matches(const uint8_t* at, const BytePattern& p) {
    for (int i = 0; i < p.len; i++) if (p.m[i] && at[i] != p.b[i]) return false;
    return true;
}

// Hits of `pattern` in the module's executable sections (stops counting at 2); -1 = bad headers/pattern.
static int scan_module(uint8_t* mod, uint32_t size, const char* pattern, uint8_t** first) {
    BytePattern p;
    *first = nullptr;
    if (!parse_pattern(pattern, &p)) return -1;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || (uint32_t)dos->e_lfanew > size - sizeof(IMAGE_NT_HEADERS32)) return -1;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int hits = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint32_t va = sec[i].VirtualAddress, vs = sec[i].Misc.VirtualSize;
        if (va >= size || vs > size - va) return -1;
        uint8_t* lo = mod + va;
        uint8_t* hi = lo + vs;
        for (uint8_t* a = lo; a + p.len <= hi; a++) {
            if (p.m[0] && a[0] != p.b[0]) continue;
            if (pattern_matches(a, p)) { if (!hits) *first = a; if (++hits > 1) return hits; }
        }
    }
    return hits;
}

static bool matches_at(const uint8_t* at, const char* pattern) {
    BytePattern p;
    return parse_pattern(pattern, &p) && pattern_matches(at, p);
}

// ---------------------------------------------------------------- per-DLL slots
typedef uint32_t(__fastcall* driver_fn)(void* ecx, void* edx);

enum { MAX_SLOTS = 8 };
struct DriverSlot {
    uint8_t*          mod;
    uint8_t*          entry;
    volatile int32_t* clock;                     // null: the clock is [ECX] (shape C)
    driver_fn          orig;
    char              name[48];
};
static DriverSlot          g_slots[MAX_SLOTS];
static volatile LONG g_driver_calls_held = 0, g_clock_fixups = 0;
static int           g_in_render_only_driver = 0;         // game thread: inside a render-only re-run of a T driver
typedef uint32_t(__cdecl* set_damage_fn)(uint32_t, uint32_t, uint32_t);
static uint8_t*      g_set_damage_entry = nullptr;      // FUN_0061b150 ms_set_damage
static set_damage_fn      g_set_damage_orig = nullptr;
static int           g_opt_dmg = 0;
static volatile LONG g_damage_skipped = 0;
static uint32_t __cdecl hook_set_damage(uint32_t att, uint32_t tgt, uint32_t flag) {
    if (g_in_render_only_driver) { InterlockedIncrement(&g_damage_skipped); return 0; }
    return g_set_damage_orig(att, tgt, flag);
}

static uint32_t run_driver(void* ecx, void* edx, int idx) {
    DriverSlot& s = g_slots[idx];
    if (!row_MAG030.on || g_disabled || !render_only()) return s.orig(ecx, edx);
    volatile int32_t* clk = s.clock ? s.clock : (volatile int32_t*)ecx;
    if (!clk) return s.orig(ecx, edx);
    InterlockedIncrement(&g_driver_calls_held);
    int32_t c = *clk;
    *clk = c - 1;                            // start <= clock / end <= clock: as of the last logic call
    g_in_render_only_driver++;
    uint32_t r = s.orig(ecx, edx);           // exec is 0 here: the driver does not advance the clock
    g_in_render_only_driver--;
    if (*clk != c - 1) InterlockedIncrement(&g_clock_fixups);
    *clk = c;
    return r;
}

template <int I> static uint32_t __fastcall hook_driver(void* ecx, void* edx) { return run_driver(ecx, edx, I); }
static void* const DRIVERS[MAX_SLOTS] = { (void*)&hook_driver<0>, (void*)&hook_driver<1>, (void*)&hook_driver<2>, (void*)&hook_driver<3>,
                                          (void*)&hook_driver<4>, (void*)&hook_driver<5>, (void*)&hook_driver<6>, (void*)&hook_driver<7> };

// ---------------------------------------------------------------- load / unload (loader lock)
// The hooked prologue (9 bytes) holds an absolute operand (A1 <service table ptr>): it is hooked only once it points
// inside the loaded module (relocated), so MinHook's copy is final.
static bool in_module(uint8_t* mod, uint32_t size, uint32_t v) { return v >= (uint32_t)mod && v - (uint32_t)mod < size; }

static void on_load(uint8_t* mod, uint32_t size, const char* name) {
    if (!row_MAG030.on || g_disabled) return;
    uint8_t* entry = nullptr;
    const Shape* sh = nullptr;
    for (int k = 0; k < 3 && !sh; k++) {
        if (!g_opt_shape[k]) continue;
        int h = scan_module(mod, size, SHAPES[k].entry, &entry);
        if (h == 0) continue;
        if (h != 1) { log_line("X2-MAG-030: %s driver entry (shape %s): %d hits - refused", name, SHAPES[k].tag, h); return; }
        sh = &SHAPES[k];
    }
    if (!sh) return;                                         // not a T overlay (silent)
    uint32_t off = (uint32_t)(entry - mod);
    auto u = [&](int o) { return *(const uint32_t*)(entry + o); };
    const char* why = nullptr;
    if (entry <= mod + 1 || (entry[-1] != 0xCC && entry[-1] != 0xC3)) why = "entry boundary";
    else if (!matches_at(entry + sh->start_off, sh->start)) why = "start test";
    else if (!matches_at(entry + sh->tail, sh->tail_sig)) why = "tail (clock INC, plain RET)";
    else if ((sh->clock >= 0 && u(sh->clock) != u(sh->tail_clock)) || u(sh->exec) != u(sh->tail_exec)) why = "clock/exec operands";
    else if (!in_module(mod, size, u(sh->svc)) || (sh->clock >= 0 && !in_module(mod, size, u(sh->clock)))) why = "operands not relocated yet";
    if (why) { log_line("X2-MAG-030: %s driver at +0x%X: %s mismatch - refused", name, off, why); return; }

    int idx = -1;
    for (int i = 0; i < MAX_SLOTS; i++) if (!g_slots[i].mod) { idx = i; break; }
    if (idx < 0) { log_line("X2-MAG-030: %s - no free slot, refused", name); return; }
    DriverSlot& s = g_slots[idx];
    s.entry = entry;
    s.clock = sh->clock >= 0 ? (volatile int32_t*)u(sh->clock) : nullptr;
    s.orig = nullptr;
    memcpy(s.name, name, sizeof(s.name));
    s.name[sizeof(s.name) - 1] = 0;
    if (!hook_entry("X2-MAG-030 driver", entry, DRIVERS[idx], (void**)&s.orig)) {
        log_line("X2-MAG-030: %s driver hook failed - refused", name);
        return;
    }
    s.mod = mod;
    if (s.clock)
        log_line("X2-MAG-030: %s hooked: T record driver (shape %s) +0x%X (gva 0x%08X), clock +0x%X, slot %d", name, sh->tag, off,
                 0x10000000u + off, (uint32_t)((uint8_t*)s.clock - mod), idx);
    else
        log_line("X2-MAG-030: %s hooked: T record driver (shape %s) +0x%X (gva 0x%08X), clock [ECX] (exec [ECX+0x%X]), slot %d",
                 name, sh->tag, off, 0x10000000u + off, u(sh->exec), idx);
}

static void on_unload(uint8_t* mod) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        DriverSlot& s = g_slots[i];
        if (s.mod != mod) continue;
        MH_STATUS a = MH_RemoveHook(s.entry);
        log_line("X2-MAG-030: %s unloaded, slot %d freed (unhook %s)", s.name, i, MH_StatusToString(a));
        s.mod = nullptr;
    }
}

static bool install() {
    if (!magic_mods_subscribe("X2-MAG-030", on_load, on_unload)) return false;
    g_opt_dmg = config_row("X2-MAG-030-DMG", 1);
    g_opt_shape[1] = config_row("X2-MAG-030-B", 1);
    g_opt_shape[2] = config_row("X2-MAG-030-C", 1);
    if (g_opt_dmg) {
        // PUSH EBP; MOV EBP,ESP; PUSH EBX; MOV EBX,[EBP+0xC]; PUSH EDI; PUSH [EBP+0x10]; MOV EDI,[EBP+8]; PUSH EBX; PUSH EDI; PUSH fmt; CALL
        g_set_damage_entry = sig_find("X2-MAG-030-DMG ms_set_damage FUN_0061b150",
            "55 8B EC 53 8B 5D 0C 57 FF 75 10 8B 7D 08 53 57 68 ?? ?? ?? ?? E8", 0x61B150);
        if (!g_set_damage_entry) { log_line("  X2-MAG-030-DMG: ms_set_damage not found - off"); g_opt_dmg = 0; }
    }
    log_line("  X2-MAG-030: magic overlay load hook registered (T record driver: no record start/end on render-only; shape B %s, C %s)", g_opt_shape[1] ? "on" : "off",
             g_opt_shape[2] ? "on" : "off");
    return true;
}

static void pre() {
    static bool done = false;
    if (done || !g_opt_dmg) return;
    done = true;                                 // game thread, between invocations
    if (!hook_entry("X2-MAG-030-DMG", g_set_damage_entry, (void*)&hook_set_damage, (void**)&g_set_damage_orig)) {
        log_line("X2-MAG-030-DMG: hook failed - off");
        g_opt_dmg = 0;
        return;
    }
    log_line("X2-MAG-030-DMG: ms_set_damage gva=0x%08X (0x0061B150) skipped inside render-only T driver re-runs",
             rt_to_gva(g_set_damage_entry));
}

static void post() {
    static LONG dmg_reported = 0;
    if (g_damage_skipped > dmg_reported && dmg_reported < 50) {
        dmg_reported = g_damage_skipped;
        log_line("X2-MAG-030-DMG: duplicate damage calls skipped so far %ld", g_damage_skipped);
    }
    static LONG reported = 0, fix_reported = 0;
    if (!reported && g_driver_calls_held) { reported = 1; log_line("X2-MAG-030: first T driver call held a tick back on a render-only invocation"); }
    if (!fix_reported && g_clock_fixups) { fix_reported = 1; log_line("X2-MAG-030: a render-only driver call changed its clock (restored)"); }
}

Row row_MAG030 = {
    "X2-MAG-030",
    "T magic overlays (magic_0718 shape): records do not start or end on render-only invocations (crash at init)",
    1, install, pre, post, 0
};

} // namespace x2
