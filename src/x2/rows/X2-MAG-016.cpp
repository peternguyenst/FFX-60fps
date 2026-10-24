// SPDX-License-Identifier: Apache-2.0
// X2-MAG-016: "record" spell DLLs keep their timing at 60 fps.
//
// Background. 79 spell DLLs (magic_0221's family) keep their effect parts as "records" in a list. A driver function in
// the DLL walks the list from the DLL's draw, so every drawn frame: it counts each record's start delay and age down
// / up, calls its callbacks, steps its sound-effect (SE) script, and counts down the end node before the effect's
// end signal. At 60 fps all of that ran at double speed.
// The fix. On render-only invocations the driver still runs (it also builds matrices and draws), but:
//   mask 1     start delay and age are saved before the call and written back after it
//   mask 2/4/0x10  the per-call, at-age and +0x18C callbacks are cleared for the call, then restored
//   mask 8     the SE script is cleared the same way
//   mask 0x20  the end node (74 of the 79 DLLs) returns at once; its countdown runs on logic invocations
// (X2-MAG-016-MASK, default 0x3F = all). The particle step inside the driver is X2-MAG-006's (paused).
// The driver is the same 1132 bytes in all 79 DLLs; each part is found by signature in the loaded DLL (one hit) and
// checked against the others before anything is hooked. F10 switches the hold off and on (X2-MAG-016-HOLD).
// Not yet confirmed in play that it fixes something visible in FFX-2 (it did in FFX).
// History and evidence: re/ledger/MAG-X2.md §2/§4 (it also keeps this file's earlier header).
#include "../x2.h"
#include "../../../third_party/minhook/include/MinHook.h"
#include <string.h>

namespace x2 {

extern Row row_MAG016;

// --- Driver signatures (addresses are magic_0221's, DLL base 0x10000000) ----------------------------------------------
// Each must hit exactly once in the DLL's executable sections; the offsets from the driver entry are fixed (S, all 79).
// Driver FUN_100016C0: no stack args, ecx/edx not read, single plain RET (C3) at +0x46B, eax = (no live record) ? 1 : 0.
static const char SIG_GATE[] =      // 0x1000177A = entry+0xBA: delay test, +0x184 call, +0x180 trigger test
    "8B 86 94 01 00 00 85 C0 0F 85 ?? ?? ?? ?? 8B 86 84 01 00 00 85 C0 74 06 56 FF D0 83 C4 04 "
    "8B 8E 80 01 00 00 85 C9 74 14 8B 86 9C 01 00 00 3B 86 98 01 00 00";
static const char SIG_AGE[] =       // 0x10001ACC = entry+0x40C: age++ / delay--
    "FF 86 98 01 00 00 EB 07 48 89 86 94 01 00 00";
static const char SIG_SE[] =        // 0x10001070: SE script, fastcall(ecx = script), wait +0x2C--
    "55 8B EC 83 EC 08 53 56 57 8B F9 89 7D F8 8B 47 2C 85 C0 7E 04 48 89 47 2C";
static const char SIG_END[] =       // 0x10001EE0: end node, cdecl(node), plain RET; +0x14-- else call +0x360
    "55 8B EC 8B 4D 08 8B 41 14 85 C0 7F 18 A1 ?? ?? ?? ?? FF 35 ?? ?? ?? ?? 8B 80 60 03 00 00 FF D0 83 C4 04 5D C3 "
    "48 89 41 14 5D C3";
// At the entry: frame, cookie, A1 <list head> (+0x14), ..., A3 <list cursor = head+8> (+0x20).
static const char PRO_ENTRY[] = "55 8B EC 81 EC 94 00 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 56 A3";
static const char PRO_SECALL[] = "8B 8E 90 01 00 00 E8";      // entry+0x401: mov ecx,[esi+0x190]; call SE script
static const char PRO_EPI[] =       // entry+0x458: eax = (edi == 0); cookie check; plain RET
    "33 C0 85 FF 0F 94 C0 5F 33 CD 5E E8 ?? ?? ?? ?? 8B E5 5D C3";
enum { OFF_GATE = 0xBA, OFF_AGE = 0x40C, OFF_SECALL = 0x401, OFF_EPI = 0x458, OFF_HEAD = 0x14, OFF_CURSOR = 0x20 };

// --- A small signature scanner for loaded spell DLLs (same as X2-MAG-030's) ---------------------------------------------
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

// --- Hooked drivers (one slot per loaded record DLL) ---------------------------------------------------------------------
typedef uint32_t(__fastcall* driver_fn)(void* ecx, void* edx);   // no stack args, plain RET (regs passed through)
typedef void(__cdecl* end_node_fn)(void* node);                        // cdecl(node), plain RET

enum { MAX_SLOTS = 8 };
struct DriverSlot {
    uint8_t* mod;             // DLL base (null = free)
    uint8_t* entry;           // driver entry
    uint8_t* end;             // end node entry (null = this DLL has none / not hooked)
    uint8_t* head_op;         // entry+0x14: the list global's address operand (read per call: relocated by then)
    driver_fn orig;
    end_node_fn    orig_end;
    char     name[24];
};
static DriverSlot          g_slots[MAX_SLOTS];
static volatile LONG g_opt_hold = 1;           // F10 toggles
static LONG          g_opt_mask = 0x3F;        // [rows] X2-MAG-016-MASK
// Mask bits: 1 delay/age, 2 +0x184, 4 +0x180, 8 SE script +0x190, 0x10 +0x18C, 0x20 end countdown node.
static volatile LONG g_driver_calls_held = 0, g_end_nodes_held = 0, g_record_list_overflows = 0;

// PPP record (stride 0x1B0) fields, FFX-2 offsets (S, magic_0221 FUN_100016c0).
enum { REC_CB_AT_AGE = 0x180, REC_CB_EVERY_CALL = 0x184, REC_CB_288 = 0x18C, REC_SE_SCRIPT = 0x190, REC_START_DELAY = 0x194, REC_AGE = 0x198, REC_FLAGS = 0x1A4 };
static uint32_t& rec_field(uint8_t* rec, int off) { return *(uint32_t*)(rec + off); }
// List (global): +0 head node, +4 tail, +8 cursor, +0xC count. Node: +0 prev, +4 next, +8 record (S, FUN_10001380).
static uint8_t* node_next(uint8_t* node) { return *(uint8_t**)(node + 4); }
static uint8_t* node_rec(uint8_t* node) { return *(uint8_t**)(node + 8); }

struct SavedRecord { uint8_t* rec; uint32_t cb_trig, cb_call, cb_288, se, delay, age; };
enum { MAX_RECS = 256 };

static bool holding_now() { return g_opt_hold && row_MAG016.on && !g_disabled && render_only(); }

// Saves every record's fields and clears the masked callback pointers; false = a fault while walking the list (the n
// entries saved so far are still restored).
static bool save_and_clear(uint8_t* list, LONG mask, SavedRecord* sv, int* n) {
    __try {
        for (uint8_t* node = *(uint8_t**)list; node; node = node_next(node)) {
            uint8_t* rec = node_rec(node);
            if (!rec) continue;
            if (*n >= MAX_RECS) { InterlockedIncrement(&g_record_list_overflows); break; }   // the rest runs stock
            SavedRecord& s = sv[(*n)++];
            s.rec = rec;
            s.cb_trig = rec_field(rec, REC_CB_AT_AGE); s.cb_call = rec_field(rec, REC_CB_EVERY_CALL); s.cb_288 = rec_field(rec, REC_CB_288);
            s.se = rec_field(rec, REC_SE_SCRIPT); s.delay = rec_field(rec, REC_START_DELAY); s.age = rec_field(rec, REC_AGE);
            if (mask & 4)    rec_field(rec, REC_CB_AT_AGE) = 0;
            if (mask & 2)    rec_field(rec, REC_CB_EVERY_CALL) = 0;
            if (mask & 0x10) rec_field(rec, REC_CB_288) = 0;
            if (mask & 8)    rec_field(rec, REC_SE_SCRIPT) = 0;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void restore_records(const SavedRecord* sv, int n, LONG mask) {
    for (int i = 0; i < n; i++) {
        uint8_t* rec = sv[i].rec;
        // pointers: put back what this row cleared (a record killed in the call gets its stock values back too,
        // unless the call wrote a new value)
        if ((mask & 4) && rec_field(rec, REC_CB_AT_AGE) == 0)   rec_field(rec, REC_CB_AT_AGE) = sv[i].cb_trig;
        if ((mask & 2) && rec_field(rec, REC_CB_EVERY_CALL) == 0)   rec_field(rec, REC_CB_EVERY_CALL) = sv[i].cb_call;
        if ((mask & 0x10) && rec_field(rec, REC_CB_288) == 0) rec_field(rec, REC_CB_288) = sv[i].cb_288;
        if ((mask & 8) && rec_field(rec, REC_SE_SCRIPT) == 0)        rec_field(rec, REC_SE_SCRIPT) = sv[i].se;
        if (!(*(uint16_t*)(rec + REC_FLAGS) & 0x8000)) continue;      // killed in this call: clocks left as they are
        if (mask & 1) { rec_field(rec, REC_START_DELAY) = sv[i].delay; rec_field(rec, REC_AGE) = sv[i].age; }
    }
}

static uint32_t run_driver(void* ecx, void* edx, int idx) {
    DriverSlot& sl = g_slots[idx];
    if (!holding_now()) return sl.orig(ecx, edx);
    LONG mask = g_opt_mask;
    uint8_t* list = *(uint8_t**)sl.head_op;
    SavedRecord sv[MAX_RECS];
    int n = 0;
    if (!save_and_clear(list, mask, sv, &n)) {
        restore_records(sv, n, mask);
        log_line("X2-MAG-016: %s fault walking the record list - row off", sl.name);
        row_MAG016.on = 0;
        return sl.orig(ecx, edx);
    }
    uint32_t r = sl.orig(ecx, edx);
    restore_records(sv, n, mask);
    InterlockedIncrement(&g_driver_calls_held);
    return r;
}

static void run_end_node(void* node, int idx) {
    DriverSlot& sl = g_slots[idx];
    if (holding_now() && (g_opt_mask & 0x20)) { InterlockedIncrement(&g_end_nodes_held); return; }   // countdown + end signal wait
    sl.orig_end(node);
}

template <int I> static uint32_t __fastcall hook_driver(void* ecx, void* edx) { return run_driver(ecx, edx, I); }
template <int I> static void __cdecl hook_end_node(void* node) { run_end_node(node, I); }
static void* const DRIVER_HOOKS[MAX_SLOTS] = { (void*)&hook_driver<0>, (void*)&hook_driver<1>, (void*)&hook_driver<2>, (void*)&hook_driver<3>,
                                          (void*)&hook_driver<4>, (void*)&hook_driver<5>, (void*)&hook_driver<6>, (void*)&hook_driver<7> };
static void* const END_NODE_HOOKS[MAX_SLOTS] = { (void*)&hook_end_node<0>, (void*)&hook_end_node<1>, (void*)&hook_end_node<2>, (void*)&hook_end_node<3>,
                                       (void*)&hook_end_node<4>, (void*)&hook_end_node<5>, (void*)&hook_end_node<6>, (void*)&hook_end_node<7> };

// --- DLL load / unload (runs under the Windows loader lock) ---------------------------------------------------------------
// Hooks: hook_entry (x2_hooks.cpp, MinHook create+enable, initialises MinHook); unhook: MH_RemoveHook.
static uint8_t* find_one(uint8_t* mod, uint32_t size, const char* name, const char* what, const char* sig, int* hits) {
    uint8_t* at = nullptr;
    *hits = scan_module(mod, size, sig, &at);
    if (*hits != 1 && *hits != 0) log_line("X2-MAG-016: %s %s: %d hits - refused", name, what, *hits);
    return *hits == 1 ? at : nullptr;
}

// The DLL's code does not run before the load notification returns. The hooked prologues (driver: 9 bytes, end node:
// 6 bytes) hold no absolute operand, so MinHook's copies stay valid whether or not relocation has been applied yet.
static void on_load(uint8_t* mod, uint32_t size, const char* name) {
    if (!row_MAG016.on || g_disabled) return;
    int h = 0;
    uint8_t* gate = find_one(mod, size, name, "record gate", SIG_GATE, &h);
    if (!gate) return;                                       // 0 hits: not an R overlay (silent)
    uint8_t* entry = gate - OFF_GATE;
    uint8_t* age = find_one(mod, size, name, "age/delay", SIG_AGE, &h);
    uint8_t* se = find_one(mod, size, name, "SE script", SIG_SE, &h);
    int he = 0;
    uint8_t* end = find_one(mod, size, name, "end node", SIG_END, &he);
    uint32_t off = (uint32_t)(gate - mod) - OFF_GATE;
    const char* why = nullptr;
    if (entry <= mod + 1 || (entry[-1] != 0xCC && entry[-1] != 0xC3) || !matches_at(entry, PRO_ENTRY)) why = "entry prologue";
    else if (age != entry + OFF_AGE) why = "age/delay site";
    else if (!matches_at(entry + OFF_EPI, PRO_EPI)) why = "epilogue (plain RET)";
    else if (!matches_at(entry + OFF_SECALL, PRO_SECALL) || !se ||
             entry + OFF_SECALL + 11 + *(int32_t*)(entry + OFF_SECALL + 7) != se) why = "SE script call";
    else if (*(uint32_t*)(entry + OFF_CURSOR) != *(uint32_t*)(entry + OFF_HEAD) + 8) why = "list head/cursor operands";
    else if (he > 1) why = "end node ambiguous";
    if (why) { log_line("X2-MAG-016: %s driver at +0x%X: %s mismatch - refused", name, off, why); return; }

    int idx = -1;
    for (int i = 0; i < MAX_SLOTS; i++) if (!g_slots[i].mod) { idx = i; break; }
    if (idx < 0) { log_line("X2-MAG-016: %s - no free slot, refused", name); return; }
    DriverSlot& s = g_slots[idx];
    s.entry = entry; s.end = nullptr; s.head_op = entry + OFF_HEAD; s.orig = nullptr; s.orig_end = nullptr;
    memcpy(s.name, name, sizeof(s.name));
    s.name[sizeof(s.name) - 1] = 0;
    if (!hook_entry("X2-MAG-016 driver", entry, DRIVER_HOOKS[idx], (void**)&s.orig)) {
        log_line("X2-MAG-016: %s driver hook failed - refused", name);
        return;
    }
    if (end) {
        if (hook_entry("X2-MAG-016 end node", end, END_NODE_HOOKS[idx], (void**)&s.orig_end)) s.end = end;
        else log_line("X2-MAG-016: %s end node hook failed - countdown stays stock", name);
    }
    s.mod = mod;
    log_line("X2-MAG-016: %s hooked: variant B (global list), driver +0x%X (gva 0x%08X), end node %s%X, slot %d, hold=%ld mask=0x%lX",
             name, off, 0x10000000u + off, s.end ? "+0x" : "none ", s.end ? (uint32_t)(s.end - mod) : 0u,
             idx, g_opt_hold, g_opt_mask);
}

static void on_unload(uint8_t* mod) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        DriverSlot& s = g_slots[i];
        if (s.mod != mod) continue;
        MH_STATUS a = MH_RemoveHook(s.entry);
        MH_STATUS b = s.end ? MH_RemoveHook(s.end) : MH_OK;
        log_line("X2-MAG-016: %s unloaded, slot %d freed (unhook %s/%s)", s.name, i, MH_StatusToString(a), MH_StatusToString(b));
        s.mod = nullptr;
    }
}

static bool install() {
    g_opt_hold = config_row("X2-MAG-016-HOLD", 1) ? 1 : 0;
    g_opt_mask = config_row("X2-MAG-016-MASK", 0x3F) & 0x3F;
    if (!magic_mods_subscribe("X2-MAG-016", on_load, on_unload)) return false;
    log_line("  X2-MAG-016: magic overlay load hook registered (R record driver), hold=%ld (F10 toggles), mask=0x%lX",
             g_opt_hold, g_opt_mask);
    return true;
}

// --- Every invocation: F10 toggle, first-use log lines ----------------------------------------------------------------------
static void pre() {
    static bool down_prev = false;
    bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (down && !down_prev) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            LONG v = g_opt_hold ? 0 : 1;
            InterlockedExchange(&g_opt_hold, v);
            log_line("X2-MAG-016: F10 -> hold=%ld", v);
        }
    }
    down_prev = down;
}

static void post() {
    static LONG reported = 0, ends_reported = 0, overflow_reported = 0;
    if (!reported && g_driver_calls_held) { reported = 1; log_line("X2-MAG-016: first held record driver call on a render-only invocation"); }
    if (!ends_reported && g_end_nodes_held) { ends_reported = 1; log_line("X2-MAG-016: first held end countdown on a render-only invocation"); }
    if (!overflow_reported && g_record_list_overflows) {
        overflow_reported = 1;
        log_line("X2-MAG-016: record list longer than %d - the rest ran unheld", (int)MAX_RECS);
    }
}

Row row_MAG016 = {
    "X2-MAG-016",
    "R magic overlays: record driver delay/age/callbacks/SE script and end countdown held on render-only invocations",
    1, install, pre, post, 0
};

} // namespace x2
