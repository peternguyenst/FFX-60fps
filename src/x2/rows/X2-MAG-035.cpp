// SPDX-License-Identifier: Apache-2.0
// X2-MAG-035: fix blinking spell effects automatically, but only the ones that actually blink.
//
// Background: at 60 fps the game alternates two kinds of frames. "Logic" frames run the game and draw it;
// "render-only" frames only draw it again. Some spell effects draw part of themselves only on logic frames, so that
// part flickers on and off at 30 Hz: a blink. X2-MAG-008 fixes this for one spell DLL by running its effect on
// render-only frames too ("passing" it).
//
// 310 of the 844 spell DLLs are built from the same code ("walker template"). Several of them blink, but passing all
// of them is not safe: magic_0449 crashed once while passed. So this row passes a template DLL only after the blink
// detector (X2-DIAG-BLINK) has seen it blink.
//
// How the decision works: after every render-only frame the detector reports whether it drew fewer things than the
// logic frames around it (a "short" frame) or more (a "long" frame). We keep the last 24 reports per DLL. When shorts
// minus longs reach 20, the DLL blinks and is passed for the rest of the effect. The bar is high on purpose: even with
// no spell on screen, about a third of frames come out short just by chance, while a real blink is short on ~94%.
//
// ini keys: X2-MAG-035-PAIRS (window size, 24), X2-MAG-035-NEED (shorts minus longs needed, 20),
// X2-MAG-035-EXCLUDE (DLL numbers never passed, "449"). Needs X2-DIAG-BLINK on.
// The first ~0.4 s of a blinking effect still blinks while the window fills.
// History and measurements: re/ledger/CRASH-0946.md, ARENA-VS.md, XFADE-BLINK.md (ffx2-fps60 repo).
#include "../x2.h"
#include <stdlib.h>
#include <string.h>

namespace x2 {

extern Row row_MAG035;

// --- Recognising a template DLL -------------------------------------------------------------------------------------

// The template's draw function, byte for byte (taken from magic_0386 at 0x10001270). Bytes that hold addresses differ
// from DLL to DLL, so the mask marks which bytes must match (1) and which are skipped (0). Checked offline: this
// matches exactly the 310 template DLLs.
static const uint8_t kTemplateDrawBytes[] = {
    0xA1, 0, 0, 0, 0, 0x56, 0x8B, 0x70, 0x04, 0x85, 0xF6, 0x75, 0x04, 0x33, 0xC0, 0x5E, 0xC3, 0x8B, 0x46, 0x58, 0x85,
    0xC0, 0x75, 0x05, 0xA1, 0, 0, 0, 0, 0xFF, 0x35, 0, 0, 0, 0, 0x89, 0x46, 0x54, 0xA1, 0, 0, 0, 0, 0x8B, 0x80, 0x40,
    0x0B, 0x00, 0x00, 0xFF, 0xD0, 0xA1, 0, 0, 0, 0, 0xFF, 0x35, 0, 0, 0, 0, 0x8B, 0x80, 0x3C, 0x0B, 0x00, 0x00, 0xFF,
    0xD0, 0xA1, 0, 0, 0, 0, 0x6A, 0x01, 0x8B, 0x80, 0x14, 0x0B, 0x00, 0x00, 0x6A, 0x01, 0xFF, 0xD0, 0xA1};
static const uint8_t kTemplateDrawMask[] = {
    1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1,
    1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
static_assert(sizeof(kTemplateDrawBytes) == sizeof(kTemplateDrawMask), "one mask entry per byte");

typedef void** (__cdecl* get_table_fn)(int);

// Every spell DLL exports GetEffectOverlayTable, a tiny function that returns a table of the effect's functions.
// Entry 4 of that table is the draw function, which we compare with the template.
static bool is_template_dll(uint8_t* dll_base) {
    __try {
        get_table_fn get_table = (get_table_fn)GetProcAddress((HMODULE)dll_base, "GetEffectOverlayTable");
        if (!get_table) return false;
        // Same code in all 844 DLLs: PUSH EBP; MOV EBP,ESP; XOR ECX,ECX; CMP [EBP+8],ECX; MOV EAX,<table address>
        uint8_t* code = (uint8_t*)get_table;
        bool expected_shape = code[0] == 0x55 && code[1] == 0x8B && code[2] == 0xEC && code[3] == 0x33 &&
                              code[4] == 0xC9 && code[8] == 0xB8;
        if (!expected_shape) return false;
        void** table = *(void***)(code + 9);   // the <table address> operand of MOV EAX
        uint8_t* draw_fn = (uint8_t*)table[4];
        for (size_t i = 0; i < sizeof(kTemplateDrawBytes); i++)
            if (kTemplateDrawMask[i] && draw_fn[i] != kTemplateDrawBytes[i]) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// --- Watching loaded template DLLs ----------------------------------------------------------------------------------

enum { MAX_WINDOW = 64, MAX_WATCHED = 8, MAX_EXCLUDED = 16 };
enum Verdict : int8_t { FRAME_LONG = -1, FRAME_SAME = 0, FRAME_SHORT = 1 };

struct WatchedDll {
    uint8_t* base;                // nullptr = free slot
    char name[24];
    uint32_t frames_seen;         // render-only frames reported so far
    int8_t recent[MAX_WINDOW];    // last window_size verdicts, used as a ring buffer
    bool decided;                 // passed already: stop watching
};

static WatchedDll g_watched[MAX_WATCHED];
static int g_window_size = 24;    // X2-MAG-035-PAIRS
static int g_shorts_needed = 20;  // X2-MAG-035-NEED
static int g_excluded[MAX_EXCLUDED], g_excluded_count = 0;
static volatile LONG g_passed_total = 0;

static int dll_number(const char* name) {
    const char* p = strstr(name, "magic_");
    return p ? atoi(p + 6) : -1;
}

static bool is_excluded(int number) {
    for (int i = 0; i < g_excluded_count; i++)
        if (g_excluded[i] == number) return true;
    return false;
}

static void on_load(uint8_t* base, uint32_t, const char* name) {
    if (!row_MAG035.on || g_disabled) return;
    if (is_excluded(dll_number(name)) || !is_template_dll(base)) return;
    for (WatchedDll& w : g_watched)
        if (!w.base) {
            memset(&w, 0, sizeof(w));
            strncpy_s(w.name, name, _TRUNCATE);
            w.base = base;
            return;
        }
}

static void on_unload(uint8_t* base) {
    for (WatchedDll& w : g_watched)
        if (w.base == base) w.base = nullptr;
}

// Called by X2-DIAG-BLINK after each render-only frame. draw_diff > 0: the frame drew fewer things than the logic
// frames before and after it (short); < 0: more (long); 0: neither.
void mag035_pair(uint8_t* base, int draw_diff) {
    if (!row_MAG035.on || g_disabled) return;
    for (WatchedDll& w : g_watched) {
        if (w.base != base || w.decided) continue;

        Verdict v = draw_diff > 0 ? FRAME_SHORT : draw_diff < 0 ? FRAME_LONG : FRAME_SAME;
        w.recent[w.frames_seen % g_window_size] = v;
        w.frames_seen++;
        if (w.frames_seen < (uint32_t)g_window_size) continue;   // window not full yet

        int shorts = 0, longs = 0;
        for (int i = 0; i < g_window_size; i++) {
            if (w.recent[i] == FRAME_SHORT) shorts++;
            if (w.recent[i] == FRAME_LONG) longs++;
        }
        if (shorts - longs < g_shorts_needed) continue;

        w.decided = true;
        if (mag008_pass_late(base, w.name)) {
            InterlockedIncrement(&g_passed_total);
            log_line("X2-MAG-035: %s blinks (%d of the last %d render-only frames below both neighbours, %d above; pair %u)"
                     " - passed (%ld so far)", w.name, shorts, g_window_size, longs, w.frames_seen, g_passed_total);
        }
    }
}

// --- Setup ----------------------------------------------------------------------------------------------------------

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// "449 512" -> {449, 512}
static void read_exclude_list() {
    char text[96];
    config_str("rows", "X2-MAG-035-EXCLUDE", text, sizeof(text), "449");
    for (char* p = text; *p && g_excluded_count < MAX_EXCLUDED;) {
        char* end = nullptr;
        long number = strtol(p, &end, 10);
        if (end == p) { p++; continue; }
        g_excluded[g_excluded_count++] = (int)number;
        p = end;
    }
}

static bool install() {
    g_window_size = clamp(config_row("X2-MAG-035-PAIRS", 24), 4, MAX_WINDOW);
    g_shorts_needed = clamp(config_row("X2-MAG-035-NEED", 20), 1, g_window_size);
    read_exclude_list();
    if (!magic_mods_subscribe("X2-MAG-035", on_load, on_unload)) return false;
    log_line("  X2-MAG-035: walker-template DLLs passed once %d of the last %d render-only frames show a blink (%d excluded)",
             g_shorts_needed, g_window_size, g_excluded_count);
    return true;
}

Row row_MAG035 = {
    "X2-MAG-035",
    "walker-template magic DLLs (310): X2-MAG-008 pass, no chain, as soon as X2-DIAG-BLINK sees them blink",
    1, install, nullptr, nullptr, 0
};

} // namespace x2
