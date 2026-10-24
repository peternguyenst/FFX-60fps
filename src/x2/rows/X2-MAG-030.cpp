// SPDX-License-Identifier: Apache-2.0
// X2-MAG-030: "timeline" spell DLLs start and end their effect parts only on logic frames (fixes two crashes).
//
// Background. 92 spell DLLs (commsphere recordings, magic_0718's family and others) play a timeline: a list of records,
// each with a start and end frame. A driver function in the DLL runs on every invocation: it starts each record whose
// start frame <= the timeline clock (calling its init once), calls the running ones, and advances the clock at the
// end, but only when the exec flag is set (logic frames).
//
// The problem. In the stock game the clock is checked once per tick, after the tick's script has set the flags a
// record's init reads. At 60 fps the render-only invocation right after a logic one starts the record half a tick
// early, before those flags are set: the init found no object and wrote through a null pointer (crashes in magic_0718,
// magic_0760 and magic_0740 while a commsphere recording played).
// The fix. On render-only invocations the driver runs with its clock one tick back (then put back), so no record
// starts or ends there; records already running are still called.
//
// Driver shapes (each found in the loaded DLL by signature, one hit required, all operands checked):
//   A  magic_0718 shape, 71 DLLs: clock and exec flag are globals of the DLL
//   B  magic_0760 shape, 11 DLLs: the same with the record table in EDI    (X2-MAG-030-B, on)
//   C  magic_0740 shape, 10 DLLs: the timeline is an object in ECX, clock at [ECX]   (X2-MAG-030-C, on)
// X2-MAG-030-DMG (on): the render-only re-run calls running records with the same frame as the logic call before it,
//   so one-shot actions keyed to a frame ran twice. For damage that used up a multi-hit attack's budget (Blitz Ace:
//   half the hits did nothing). While the re-run is active, ms_set_damage (FUN_0061b150) does nothing.
// History, measurements and evidence: re/ledger/MAG-TASK.md (it also keeps this file's earlier comments), DMGNUM.md.
#include "../x2.h"
#include "../../../third_party/minhook/include/MinHook.h"
#include <string.h>

namespace x2 {

extern Row row_MAG030;

// --- Driver signatures (offsets from the driver entry; addresses are magic_0718_us's, DLL base 0x10000000) ------------
// Shape A, magic_0718 FUN_10003870: __fastcall (ECX passed on to the exec getter), no stack arguments, plain RETs.
static const char SIG_ENTRY[] =     // entry: exec getter +0x400 -> [exec], live count test, record loop setup
    "55 8B EC 51 A1 ?? ?? ?? ?? 8B 80 00 04 00 00 FF D0 83 3D ?? ?? ?? ?? 00 A3 ?? ?? ?? ?? 75 09 B8 01 00 00 00 "
    "8B E5 5D C3 53 56 33 DB 57 BE ?? ?? ?? ?? 39 1D ?? ?? ?? ?? 0F 8E";
static const char SIG_START[] =     // entry+0x44: record test, start != -1, CMP start,[clock] (operand +0x5E)
    "83 C7 0C 33 D2 89 55 FC 39 16 0F 84 ?? ?? ?? ?? 8B 47 F8 83 F8 FF 74 ?? 3B 05";
static const char SIG_TAIL[] =      // entry+0xE8: if ([exec]) INC [clock] (operand +0xF3); plain RET
    "83 3D ?? ?? ?? ?? 00 74 06 FF 05 ?? ?? ?? ?? 5F 5E 33 C0 5B 8B E5 5D C3";
enum { OFF_SVC = 5, OFF_EXEC = 0x19, OFF_START = 0x44, OFF_CLOCK = 0x5E, OFF_TAIL = 0xE8, OFF_TAIL_EXEC = 0xEA, OFF_TAIL_CLOCK = 0xF3 };
// Shape B, magic_0760 FUN_10003680 (magic_0760, magic_0797 and its 9 language copies): loads the record table into EDI
// first and indexes it from there; otherwise the same record logic and clock / exec use as A.
static const char SIG_ENTRY_B[] =   // as A, plus MOV EDI,[record table] before MOV ESI,records
    "55 8B EC 51 A1 ?? ?? ?? ?? 8B 80 00 04 00 00 FF D0 83 3D ?? ?? ?? ?? 00 A3 ?? ?? ?? ?? 75 09 B8 01 00 00 00 "
    "8B E5 5D C3 53 56 33 DB 57 8B 3D ?? ?? ?? ?? BE ?? ?? ?? ?? 39 1D ?? ?? ?? ?? 0F 8E";
static const char SIG_START_B[] =   // entry+0x44: record test (short JE), start != -1, CMP start,[clock] (operand +0x57)
    "33 D2 89 55 FC 39 16 74 ?? 8B 47 04 83 F8 FF 74 ?? 3B 05";
// Shape C, magic_0740 FUN_10005570 (0614 0631 0740 0741 0759 0770 0799 0808 0872 0923): the timeline is an object in
// ECX (thiscall): clock [obj+0], record count [obj+4], live count [obj+8], records from obj+0xC (0x74 bytes each),
// record table [obj+0x74C], exec flag [obj+0x750]. The clock is found through ECX at each call.
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

// --- A small signature scanner for loaded spell DLLs (x2_sigs.cpp only scans FFX-2.exe) -------------------------------
// "55 8B ?? .." -> bytes b and mask m (1 = must match, 0 = ??).
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

// --- Hooked drivers (one slot per loaded timeline DLL) ---------------------------------------------------------------------
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
// X2-MAG-030-DMG: damage requests from inside a render-only driver re-run are dropped (the logic call already made them).
static uint32_t __cdecl hook_set_damage(uint32_t att, uint32_t tgt, uint32_t flag) {
    if (g_in_render_only_driver) { InterlockedIncrement(&g_damage_skipped); return 0; }
    return g_set_damage_orig(att, tgt, flag);
}

// Render-only invocation: run the driver with its clock one tick back, so it starts and ends nothing, then put it back.
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

// One hook function per slot (the slot number is baked in, since the driver's own arguments do not say which DLL).
template <int I> static uint32_t __fastcall hook_driver(void* ecx, void* edx) { return run_driver(ecx, edx, I); }
static void* const DRIVERS[MAX_SLOTS] = { (void*)&hook_driver<0>, (void*)&hook_driver<1>, (void*)&hook_driver<2>, (void*)&hook_driver<3>,
                                          (void*)&hook_driver<4>, (void*)&hook_driver<5>, (void*)&hook_driver<6>, (void*)&hook_driver<7> };

// --- DLL load / unload (runs under the Windows loader lock) ---------------------------------------------------------------
// The hooked first 9 bytes contain an absolute address (MOV EAX,[service table]); the hook goes in only once that
// address points inside the loaded DLL (relocated), so MinHook copies the final bytes.
static bool in_module(uint8_t* mod, uint32_t size, uint32_t v) { return v >= (uint32_t)mod && v - (uint32_t)mod < size; }

// A spell DLL loaded: if it has a driver of one of the shapes (exactly one hit, every operand checked), hook it.
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
