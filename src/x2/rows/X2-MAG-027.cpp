// SPDX-License-Identifier: Apache-2.0
// X2-MAG-027: cross-fade spells run each timeline event's start and end once (fixes a crash in Full Throttle).
//
// 27 cross-fade spell DLLs (magic_0681's family) walk an event timeline from their draw, on every invocation. Each
// event has a start and an end frame; its handler is called with the timeline clock, and the clock advances only on
// logic invocations. So at 60 fps every clock value was handled twice (render-only, then logic), and an event's start
// and end ran twice: magic_0681's handler frees a buffer at the end, twice = heap corruption.
// The fix, on render-only invocations, for that one walk: events starting at this clock are taken out of the list
// (they start on the logic invocation), and events ending at this clock get end + 1 (they run their normal in-range
// draw instead of their end). The list and the end fields are restored right after. If the shortened list made the
// walk mark the timeline finished, the clock is put back: the logic invocation decides. (A first build also hid ending
// events, which made each element vanish for one frame as it ended.) Evidence: re/ledger/CRASH-0681.md.
#include "../x2.h"
#include <intrin.h>
#include <string.h>

namespace x2 {

typedef void(__fastcall* walk_t)(void* list, void* edx);

enum { MAX_SLOTS = 8, MAX_EV = 256 };
struct Slot { uint8_t* mod; uint32_t size; uint8_t* entry; walk_t orig; char name[24]; };
static Slot g_slots[MAX_SLOTS];
// X2-MAG-027-MODE: 1 = hide start-tick events + end+1 for end-tick events; 2 (default) = end+1 only (the end phase,
// e.g. the buffer free, still runs only on logic invocations; start phases may run on the render-only invocation first,
// the event handlers guard their one-shot work with their own started flag +6). U 17/10/2026: Full Throttle sword/fire
// still blinked with mode 1 (H: the hidden starts).
static int g_mode = 2;
static volatile LONG g_hidden = 0, g_extended = 0, g_walks = 0, g_kept_clock = 0;
extern Row row_MAG027;

// prologue PUSH EBP; MOV EBP,ESP; SUB ESP,0xC; PUSH EBX/ESI/EDI; MOV EDI,ECX; MOV ECX,0xFFFF; MOV EAX,[EDI];
// MOV ESI,1; MOVZX EAX,word [EAX+6]; MOV EBX,ESI; CMP AX,CX; JZ
static const uint8_t SIG_A[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x53, 0x56, 0x57, 0x8B, 0xF9, 0xB9, 0xFF, 0xFF, 0x00,
    0x00, 0x8B, 0x07, 0xBE, 0x01, 0x00, 0x00, 0x00, 0x0F, 0xB7, 0x40, 0x06, 0x8B, 0xDE, 0x66, 0x3B, 0xC1, 0x0F, 0x84};
// exec getter call and the clock INC: MOV EAX,[EAX+0x400]; CALL EAX; TEST EAX,EAX; JZ +6; MOV EAX,[EDI]; INC word [EAX+6]
static const uint8_t SIG_B[] = {0x8B, 0x80, 0x00, 0x04, 0x00, 0x00, 0xFF, 0xD0, 0x85, 0xC0, 0x74, 0x06, 0x8B, 0x07, 0x66,
    0xFF, 0x40, 0x06};

static Slot* slot_of(const void* p) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        Slot& s = g_slots[i];
        if (s.mod && (const uint8_t*)p >= s.mod && (const uint8_t*)p < s.mod + s.size) return &s;
    }
    return nullptr;
}

static void __fastcall h_walk(void* list, void* edx) {
    Slot* s = slot_of(_ReturnAddress());
    if (!s) s = slot_of(list);
    if (!s || !s->orig) return;                  // cannot happen: hooked only inside a slot's module
    if (!row_MAG027.on || g_disabled || !render_only()) { s->orig(list, edx); return; }
    uint16_t** ev = (uint16_t**)list;
    uint16_t* saved[MAX_EV + 2];
    uint16_t* ended[MAX_EV];
    int n = 0, ne = 0;
    bool changed = false;
    uint16_t* hdr = ev[0];
    int16_t clock = 0;
    __try {
        clock = (int16_t)hdr[3];
        if (clock != -1) {
            for (n = 1; n <= MAX_EV && ev[n][0] != 0xFFFF; n++) {}
            if (n > MAX_EV) n = 0;               // no terminator: leave the walk alone
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    if (n > 1) {
        memcpy(saved, ev, (n + 1) * sizeof(void*));    // [0] header .. [n] terminator
        int w = 1;
        for (int i = 1; i < n; i++) {
            uint16_t* e = saved[i];
            if (g_mode == 1 && (int)e[1] == (int)(uint16_t)clock) { InterlockedIncrement(&g_hidden); changed = true; continue; }
            if ((int)e[2] == (int)(uint16_t)clock && e[2] != 0xFFFE) { e[2]++; ended[ne++] = e; InterlockedIncrement(&g_extended); }
            ev[w++] = e;
        }
        ev[w] = saved[n];                            // terminator (its end field is the timeline length)
    }
    InterlockedIncrement(&g_walks);
    s->orig(list, edx);
    for (int i = 0; i < ne; i++) ended[i][2]--;
    if (changed || ne) {
        if (changed) memcpy(ev, saved, (n + 1) * sizeof(void*));
        if (hdr[3] == 0xFFFF && clock != -1) { hdr[3] = (uint16_t)clock; InterlockedIncrement(&g_kept_clock); }
    }
}

static int scan(uint8_t* mod, uint32_t size, uint8_t** at) {
    int hits = 0;
    __try {
        for (uint32_t i = 0; i + 0x80 <= size; i++) {
            if (mod[i] != 0x55 || memcmp(mod + i, SIG_A, sizeof(SIG_A)) != 0) continue;
            bool tail = false;
            for (uint32_t k = 0x30; k < 0x60 && !tail; k++) tail = memcmp(mod + i + k, SIG_B, sizeof(SIG_B)) == 0;
            if (tail) { if (!hits) *at = mod + i; hits++; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return hits;
}

static void on_load(uint8_t* mod, uint32_t size, const char* name) {
    if (!row_MAG027.on || g_disabled) return;
    uint8_t* entry = nullptr;
    int h = scan(mod, size, &entry);
    if (h == 0) return;                                      // no timeline walker (silent)
    if (h != 1) { log_line("X2-MAG-027: %s timeline walker: %d hits - refused", name, h); return; }
    if (entry[-1] != 0xCC && entry[-1] != 0xC3) { log_line("X2-MAG-027: %s walker entry boundary mismatch - refused", name); return; }
    int idx = -1;
    for (int i = 0; i < MAX_SLOTS; i++) if (!g_slots[i].mod) { idx = i; break; }
    if (idx < 0) { log_line("X2-MAG-027: %s - no free slot, refused", name); return; }
    Slot& s = g_slots[idx];
    s.size = size;
    s.entry = entry;
    s.orig = nullptr;
    memcpy(s.name, name, sizeof(s.name));
    s.name[sizeof(s.name) - 1] = 0;
    s.mod = mod;                                             // set before the hook: the detour finds its slot
    if (!hook_entry("X2-MAG-027 walker", entry, (void*)&h_walk, (void**)&s.orig)) {
        log_line("X2-MAG-027: %s walker hook failed - refused", name);
        s.mod = nullptr;
        return;
    }
    log_line("X2-MAG-027: %s hooked: timeline walker +0x%X (gva 0x%08X), slot %d", name, (uint32_t)(entry - mod),
             0x10000000u + (uint32_t)(entry - mod), idx);
}

static void on_unload(uint8_t* mod) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        Slot& s = g_slots[i];
        if (s.mod != mod) continue;
        hook_remove(s.entry);
        log_line("X2-MAG-027: %s unloaded, slot %d freed (start events hidden %ld, end events drawn in range %ld, clock kept %ld)",
                 s.name, i, g_hidden, g_extended, g_kept_clock);
        s.mod = nullptr;
        s.orig = nullptr;
    }
}

static bool install() {
    g_mode = config_row("X2-MAG-027-MODE", 2);
    if (g_mode != 1) g_mode = 2;
    if (!magic_mods_subscribe("X2-MAG-027", on_load, on_unload)) return false;
    log_line("  X2-MAG-027: cross-fade timeline walkers hooked per magic DLL load, mode %d (%s)", g_mode,
             g_mode == 1 ? "start events hidden, end events drawn in range" : "end events drawn in range only");
    return true;
}

Row row_MAG027 = {
    "X2-MAG-027",
    "cross-fade magic DLLs' event timeline: start/end events run on logic invocations only (double free crash)",
    1, install, nullptr, nullptr, 0
};

} // namespace x2
