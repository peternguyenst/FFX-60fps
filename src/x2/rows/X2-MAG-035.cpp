// SPDX-License-Identifier: Apache-2.0
// X2-MAG-035: automatic X2-MAG-008 pass for blinking "walker template" magic DLLs (user 22/10/2026: "why not just fix
// them if it's same pattern"). 310 of the 844 FFX-2 magic DLLs share one Mag_Draw (decompile survey 22/10: `+0xB40`,
// `+0xB3C`, `(*(svc+0xB14))(1,1)`, `+0xB34`; byte-identical but for absolute operands in 0212, 0283, 0330, 0386,
// 0449). Several blink at 60 Hz and were fixed one by one with X2-MAG-008-PASS-LIST (212, 330: U 21/10; 386 Requiem,
// 283 Big Bang: U 22/10, no chain). Passing every one blindly is not safe: magic_0449 (same template, 55 casts
// unpassed without a fault, no blink reported) crashed once while passed (22/10 22:14:18, FFX-2.exe 0x864C50 under its
// walker draw; ARENA-VS.md). So a template DLL is passed only when it is seen to blink: X2-DIAG-BLINK reports each
// render-only frame vs the logic frames on both sides (mag035_pair: +1 below both, -1 above both); after
// X2-MAG-035-PAIRS (default 12) pairs with (below - above) at least half of them, the DLL is passed for the rest of its run (mag008_pass_late: listed profile, no chain).
// X2-MAG-035-EXCLUDE (default "449") is never passed. magic_0946 (excluded 24/10 after a crash under its late pass)
// is passable again since X2-MAG-025-LOOP1F stopped running uncounted spawn loops on update walks (CRASH-0946.md, U).
// Needs X2-DIAG-BLINK on. The first ~0.2 s of a blinking effect
// still blinks.
#include "../x2.h"
#include <stdlib.h>
#include <string.h>

namespace x2 {

extern Row row_MAG035;

// Mag_Draw of the walker template (magic_0386 0x10001270, 0x58 bytes), absolute operands masked (0); offline check
// 22/10 over magicFiles/FFX-2: exactly 310 hits
static const uint8_t TB[] = {
    0xA1, 0, 0, 0, 0, 0x56, 0x8B, 0x70, 0x04, 0x85, 0xF6, 0x75, 0x04, 0x33, 0xC0, 0x5E, 0xC3, 0x8B, 0x46, 0x58, 0x85,
    0xC0, 0x75, 0x05, 0xA1, 0, 0, 0, 0, 0xFF, 0x35, 0, 0, 0, 0, 0x89, 0x46, 0x54, 0xA1, 0, 0, 0, 0, 0x8B, 0x80, 0x40,
    0x0B, 0x00, 0x00, 0xFF, 0xD0, 0xA1, 0, 0, 0, 0, 0xFF, 0x35, 0, 0, 0, 0, 0x8B, 0x80, 0x3C, 0x0B, 0x00, 0x00, 0xFF,
    0xD0, 0xA1, 0, 0, 0, 0, 0x6A, 0x01, 0x8B, 0x80, 0x14, 0x0B, 0x00, 0x00, 0x6A, 0x01, 0xFF, 0xD0, 0xA1};
static const uint8_t TM[] = {
    1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1,
    1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
static_assert(sizeof(TB) == sizeof(TM), "template mask");

// Rolling window (U/D 23/10/2026 21:21: magic_0938, a dressphere change, blinked on 94% of 120 pairs and was never
// passed: the first 12 pairs were judged once, before the blink began, and the DLL was marked done): the last
// X2-MAG-035-PAIRS deltas are kept and the DLL is passed the first time the window holds a blink.
enum { MAXWIN = 64 };
struct Slot { uint8_t* base; char name[24]; uint32_t pairs; int8_t win[MAXWIN]; bool done; };
static Slot g_s[8];
static int g_need = 12;
static int g_excl[16], g_nexcl = 0;
static volatile LONG g_auto = 0;

typedef void** (__cdecl* table_t)(int);

static bool is_template(uint8_t* base) {
    __try {
        table_t get = (table_t)GetProcAddress((HMODULE)base, "GetEffectOverlayTable");
        if (!get) return false;
        uint8_t* g = (uint8_t*)get;   // `PUSH EBP; MOV EBP,ESP; XOR ECX,ECX; CMP [EBP+8],ECX; MOV EAX,<table>` (all 844)
        if (!(g[0] == 0x55 && g[1] == 0x8B && g[2] == 0xEC && g[3] == 0x33 && g[4] == 0xC9 && g[8] == 0xB8)) return false;
        void** tbl = *(void***)(g + 9);
        uint8_t* draw = (uint8_t*)tbl[4];                     // table +0x10 = Mag_Draw (MAGIC-ABI.md)
        for (size_t i = 0; i < sizeof(TB); i++) if (TM[i] && draw[i] != TB[i]) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void on_load(uint8_t* base, uint32_t, const char* name) {
    if (!row_MAG035.on || g_disabled) return;
    const char* p = strstr(name, "magic_");
    int n = p ? atoi(p + 6) : -1;
    for (int i = 0; i < g_nexcl; i++) if (g_excl[i] == n) return;
    if (!is_template(base)) return;
    for (Slot& s : g_s)
        if (!s.base) {
            memset(&s, 0, sizeof(s));
            strncpy_s(s.name, name, _TRUNCATE);
            s.base = base;
            return;
        }
}

static void on_unload(uint8_t* base) {
    for (Slot& s : g_s) if (s.base == base) s.base = nullptr;
}

void mag035_pair(uint8_t* base, int delta) {
    if (!row_MAG035.on || g_disabled) return;
    for (Slot& s : g_s) {
        if (s.base != base || s.done) continue;
        s.win[s.pairs % g_need] = (int8_t)(delta > 0 ? 1 : delta < 0 ? -1 : 0);
        s.pairs++;
        if (s.pairs < (uint32_t)g_need) continue;
        int shorts = 0, longs = 0;
        for (int i = 0; i < g_need; i++) { if (s.win[i] > 0) shorts++; if (s.win[i] < 0) longs++; }
        if ((shorts - longs) * 2 >= g_need) {
            s.done = true;
            if (mag008_pass_late(base, s.name)) {
                InterlockedIncrement(&g_auto);
                log_line("X2-MAG-035: %s blinks (%d of the last %d render-only frames below both neighbours, %d above; pair %u)"
                         " - passed (%ld so far)", s.name, shorts, g_need, longs, s.pairs, g_auto);
            }
        }
    }
}

static bool install() {
    g_need = config_row("X2-MAG-035-PAIRS", 12);
    if (g_need < 4) g_need = 4;
    if (g_need > MAXWIN) g_need = MAXWIN;
    char buf[96];
    config_str("rows", "X2-MAG-035-EXCLUDE", buf, sizeof(buf), "449");
    for (char* q = buf; *q && g_nexcl < 16;) {
        char* e = nullptr; long v = strtol(q, &e, 10);
        if (e == q) { q++; continue; }
        g_excl[g_nexcl++] = (int)v; q = e;
    }
    if (!magic_mods_subscribe("X2-MAG-035", on_load, on_unload)) return false;
    log_line("  X2-MAG-035: walker-template DLLs passed once %d render-only frames show a blink (%d excluded)", g_need,
             g_nexcl);
    return true;
}

Row row_MAG035 = {
    "X2-MAG-035",
    "walker-template magic DLLs (310): X2-MAG-008 pass, no chain, as soon as X2-DIAG-BLINK sees them blink",
    1, install, nullptr, nullptr, 0
};

} // namespace x2
