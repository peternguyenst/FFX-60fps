// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-FTBLINK (diagnostic, default 0, observe only): tests H1 of re/ledger/FT-BLINK.md for the Full Throttle blink
// (magic_0681 sword / fire drawn on logic invocations only). Chr actors reach the HD model list (hdframe+0xE9CC count,
// list +0xE9D4) only through Ch_DrawMain loop 3 (FUN_00449f20 at 0x6DCD98), which runs once per tick; the list is cleared
// in FUN_00464e80 (0x46500D, every invocation) when the state block [0xDCED88] has [0] != 1 or byte +0x338 / +0x33C set.
// While a listed magic DLL is loaded (X2-DIAG-FTBLINK-DLL, default "681 80": Full Throttle and a plain attack for
// comparison) every invocation logs, at its end: kind, the list count, state [0], +0x338, +0x33C and the BG / light colours
// (max 1200 lines per load).
// H1 holds if, for 0681 only, the state clears the list each invocation and RENDER counts drop below LOGIC counts.
// X2-DIAG-FTBLINK-ALT=1 (dressphere outer-model blink, re/ledger/XFADE-BLINK.md; U 18/10: two consecutive frames draw the
// new outfit faint, then opaque): per invocation snapshot every actor record (table 0x01CC9604, stride 0x910, up to 48)
// and 0x400 bytes of the first 4 HD model-list objects (hdframe+0xE9D4 dword handles); per dword count A/B/A/B
// alternation (value == value two invocations back, != the previous one). Every 120 invocations of a watched DLL, log
// the dwords that alternated on >= 80% of them, with the last LOGIC and RENDER values (hex and float).
// Also (U 18/10: the sword close-up frames alternate near-black / lit; tint, fade, MAG-025 ruled out) the HD state:
// hdframe [0xDCDA04] 0xFC40 bytes, PostProcessManager [0xDD10F4] 0x12400 bytes and the MotionBlur object [0xDDC74C]
// 0x100 bytes (pointers from X2-DIAG-FLASH, which must be on).
// Ultima (magic_0106, U 23/10/2026: background grey / black on consecutive frames): hdframe now reaches the colour at
// +0xFC24..+0xFC2C (FUN_00453be0, set through service +0x554 by EgoBgVal), and the light globals 0x1725080..0x1725520
// (flags 0x172508C, colour 0x1725390, RGBA 0x1725500; FUN_007bb1e0 / 007bb410 / 007baf80) are snapshotted as "light".
#include "../x2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace x2 {

static uint8_t** g_state = nullptr;              // &DAT_00dced88 (pointer to the state block)
static uint8_t** g_hdframe = nullptr;            // &DAT_00dcda04
static uint8_t* volatile* g_table = nullptr;     // actor array pointer (0x01CC9604), count 0x01CC9600, stride 0x910
static volatile int32_t*  g_acount = nullptr;
static int  g_list[8], g_nlist = 0;
static uint8_t* g_watch = nullptr;               // loaded watched DLL base (one at a time)
static char  g_name[24];
static volatile LONG g_active = 0;
static int   g_lines = 0;
extern Row row_DIAGFTBLINK;

enum { ALT_ACT = 48, ALT_MDL = 4, ALT_MDLB = 0x400, ALT_HF = 0xFC40, ALT_PPM = 0x12400, ALT_MB = 0x100, ALT_GL = 0x4A0,
       ALT_O_MDL = ALT_ACT * 0x910, ALT_O_HF = ALT_O_MDL + ALT_MDL * ALT_MDLB, ALT_O_PPM = ALT_O_HF + ALT_HF,
       ALT_O_MB = ALT_O_PPM + ALT_PPM, ALT_O_GL = ALT_O_MB + ALT_MB, ALT_BYTES = ALT_O_GL + ALT_GL, ALT_DW = ALT_BYTES / 4 };
static uint8_t* g_light = nullptr;               // 0x1725080 = RGBA 0x1725500 (FUN_007bb1e0 operand) - 0x480
extern uint8_t** g_ctx_ptr;                      // X2-DIAG-FLASH
extern uint8_t** g_obj_ptr;
static int      g_alt_on = 0;
static uint32_t g_snap[3][ALT_DW];               // ring: [g_si] newest
static uint32_t g_lastL[ALT_DW], g_lastR[ALT_DW];
static uint16_t g_alt[ALT_DW];
static int      g_si = 0, g_nsnap = 0, g_nalt = 0;
static uint16_t g_actid[ALT_ACT];

static void alt_snapshot() {
    uint32_t* cur = g_snap[g_si];
    memset(cur, 0, sizeof(g_snap[0]));
    __try {
        uint8_t* base = *g_table; int n = *g_acount;
        if (n > ALT_ACT) n = ALT_ACT;
        for (int i = 0; base && i < n; i++) {
            memcpy((uint8_t*)cur + i * 0x910, base + i * 0x910, 0x910);
            g_actid[i] = *(uint16_t*)(base + i * 0x910);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        uint8_t* hf = *g_hdframe;
        int cnt = hf ? *(int32_t*)(hf + 0xE9CC) : 0;
        uint32_t* arr = hf ? *(uint32_t**)(hf + 0xE9D4) : nullptr;
        for (int j = 0; arr && j < cnt && j < ALT_MDL; j++) {
            __try { memcpy((uint8_t*)cur + ALT_ACT * 0x910 + j * ALT_MDLB, (void*)(uintptr_t)arr[j], ALT_MDLB); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { uint8_t* hf = *g_hdframe; if (hf) memcpy((uint8_t*)cur + ALT_O_HF, hf, ALT_HF); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { uint8_t* p = g_ctx_ptr ? *g_ctx_ptr : nullptr; if (p) memcpy((uint8_t*)cur + ALT_O_PPM, p, ALT_PPM); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { uint8_t* p = g_obj_ptr ? *g_obj_ptr : nullptr; if (p) memcpy((uint8_t*)cur + ALT_O_MB, p, ALT_MB); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { if (g_light) memcpy((uint8_t*)cur + ALT_O_GL, g_light, ALT_GL); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    memcpy(render_only() ? g_lastR : g_lastL, cur, sizeof(g_snap[0]));
    if (g_nsnap >= 2) {
        const uint32_t* p1 = g_snap[(g_si + 2) % 3];
        const uint32_t* p2 = g_snap[(g_si + 1) % 3];
        for (int o = 0; o < ALT_DW; o++)
            if (cur[o] == p2[o] && cur[o] != p1[o] && g_alt[o] < 0xFFFF) g_alt[o]++;
        g_nalt++;
    }
    g_nsnap++;
    g_si = (g_si + 1) % 3;
    if (g_nalt < 120) return;
    int shown = 0;
    log_line("FTBLINK-ALT %s: dwords alternating on >= 80%% of %d invocations:", g_name, g_nalt);
    for (int o = 0; o < ALT_DW && shown < 150; o++) {
        if (g_alt[o] * 5 < g_nalt * 4) continue;
        uint32_t b = o * 4, L = g_lastL[o], R = g_lastR[o];
        float fl, fr; memcpy(&fl, &L, 4); memcpy(&fr, &R, 4);
        if (b < ALT_ACT * 0x910)
            log_line("  actor %d (id %04X) +0x%03X: LOGIC %08X (%g) RENDER %08X (%g) alt %d", b / 0x910, g_actid[b / 0x910],
                     b % 0x910, L, fl, R, fr, g_alt[o]);
        else if (b < ALT_O_HF)
            log_line("  model %d +0x%03X: LOGIC %08X (%g) RENDER %08X (%g) alt %d", (b - ALT_O_MDL) / ALT_MDLB,
                     (b - ALT_O_MDL) % ALT_MDLB, L, fl, R, fr, g_alt[o]);
        else
            log_line("  %s +0x%05X: LOGIC %08X (%g) RENDER %08X (%g) alt %d",
                     b < ALT_O_PPM ? "hdframe" : b < ALT_O_MB ? "ppm" : b < ALT_O_GL ? "motionblur" : "light 0x1725080",
                     b - (b < ALT_O_PPM ? ALT_O_HF : b < ALT_O_MB ? ALT_O_PPM : b < ALT_O_GL ? ALT_O_MB : ALT_O_GL), L, fl, R, fr,
                     g_alt[o]);
        shown++;
    }
    log_line("FTBLINK-ALT %s: %d dwords shown", g_name, shown);
    memset(g_alt, 0, sizeof(g_alt));
    g_nalt = 0;
}

static void on_load(uint8_t* base, uint32_t, const char* name) {
    const char* p = strstr(name, "magic_");
    int n = p ? atoi(p + 6) : -1;
    for (int i = 0; i < g_nlist; i++)
        if (g_list[i] == n && !g_watch) {
            g_watch = base;
            strncpy_s(g_name, name, _TRUNCATE);
            g_lines = 0;
            g_nsnap = 0; g_nalt = 0; memset(g_alt, 0, sizeof(g_alt));
            InterlockedExchange(&g_active, 1);
            log_line("X2-DIAG-FTBLINK: %s loaded - logging HD list count and clear state per invocation", name);
        }
}
static void on_unload(uint8_t* base) {
    if (base == g_watch) { InterlockedExchange(&g_active, 0); g_watch = nullptr; }
}

// X2-DIAG-FLASH logs only while a watched DLL is loaded when this row is on (battle bloom is never 0).
bool ftblink_gate() { return !row_DIAGFTBLINK.on || g_active != 0; }
bool ftblink_watching() { return row_DIAGFTBLINK.on && g_active != 0; }
uint8_t* ftblink_base() { return g_active ? g_watch : nullptr; }

static void post() {
    if (g_active && g_alt_on) alt_snapshot();
    if (!g_active || g_lines >= 1200) return;
    int32_t cnt = -1, st = -1, b338 = -1, b33c = -1;
    __try {
        uint8_t* hf = *g_hdframe;
        if (hf) cnt = *(int32_t*)(hf + 0xE9CC);
        uint8_t* s = *g_state;
        if (s) { st = *(int32_t*)s; b338 = s[0x338]; b33c = s[0x33C]; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // loop-3 gate fields (FT-BLINK B6) of the first two sword actors (id 0x0133): +2, +0x18F, +0x1A0, +0x1A4, +0x360
    char act[200]; int k = 0; act[0] = 0;
    __try {
        uint8_t* base = *g_table; int n = *g_acount, shown = 0;
        for (int i = 0; base && i < n && i < 64 && shown < 2; i++) {
            uint8_t* a = base + i * 0x910;
            if (*(uint16_t*)a != 0x0133) continue;
            k += _snprintf_s(act + k, sizeof(act) - k, _TRUNCATE, " [s%d +2=%u 18F=%02X 1A0=%08X 1A4=%08X 360=%08X]", i, a[2],
                             a[0x18F], *(uint32_t*)(a + 0x1A0), *(uint32_t*)(a + 0x1A4), *(uint32_t*)(a + 0x360));
            shown++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Ultima background (U 23/10/2026): BG colour hdframe +0xFC24..2C (FUN_00453be0), RGBA 0x1725500 (svc +0x554) and
    // 0x17254C4 (+0xA5C), light flags 0x172508C, colour 0x1725390 (+0x5A8)
    char col[200]; col[0] = 0;
    __try {
        uint8_t* hf = *g_hdframe;
        if (hf && g_light) {
            const float* bg = (const float*)(hf + 0xFC24);
            const uint8_t* a = g_light + 0x480;
            const uint8_t* b = g_light + 0x444;
            const float* lc = (const float*)(g_light + 0x310);
            _snprintf_s(col, sizeof(col), _TRUNCATE, " bg=%.3f,%.3f,%.3f rgba500=%u,%u,%u,%u rgba4C4=%u,%u,%u,%u fl=%X lc=%.3f,%.3f,%.3f",
                        bg[0], bg[1], bg[2], a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], *(uint32_t*)(g_light + 0xC),
                        lc[0], lc[1], lc[2]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    static const char* K[] = {"LOGIC", "RENDER", "BACKLOG", "STOCK"};
    log_line("FTBLINK %s #%d %s list=%d state=%d b338=%d b33c=%d%s%s", g_name, g_lines, K[pacing_kind() & 3], cnt, st, b338, b33c,
             act, col);
    g_lines++;
}

static bool install() {
    uint8_t* t = sig_find("X2-DIAG-FTBLINK HD list clear test",
        "A3 ?? ?? ?? ?? 80 B8 38 03 00 00 00 75 0E 80 B8 3C 03 00 00 00 75 05 83 38 01 74 07 8B CE E8", 0x464FEF);
    uint8_t* a = sig_find("X2-DIAG-FTBLINK loop-3 model add",
        "FF B6 44 08 00 00 E8 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 83 C4 08 E8 ?? ?? ?? ?? 3D 2B 02 00 00", 0x6DCD92);
    uint8_t* thunk = a ? call_target(a + 6) : nullptr;
    if (!t || !thunk || !sig_match(thunk, "55 8B EC FF 75 08 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 5D C3")) {
        log_line("  X2-DIAG-FTBLINK: sites not as proven - off");
        return false;
    }
    uint8_t* add = call_target(thunk + 12);
    if (!add || !sig_match(add, "55 8B EC FF 81 CC E9 00 00")) { log_line("  X2-DIAG-FTBLINK: list add +0xE9CC not proven - off"); return false; }
    g_state = (uint8_t**)abs_operand("HD state ptr", t + 1);
    g_hdframe = (uint8_t**)abs_operand("hdframe ptr", thunk + 8);
    if (!g_state || !g_hdframe) return false;
    uint8_t* tb = sig_find("X2-DIAG-FTBLINK actor table", "8B 0D ?? ?? ?? ?? 8B 35 ?? ?? ?? ?? 69 C9 10 09 00 00 03 CE", 0x6DBAA5);
    if (!tb) return false;
    g_acount = (volatile int32_t*)abs_operand("actor count", tb + 2);
    g_table = (uint8_t* volatile*)abs_operand("actor table", tb + 8);
    if (!g_acount || !g_table) return false;
    char buf[128];
    config_str("rows", "X2-DIAG-FTBLINK-DLL", buf, sizeof(buf), "681");
    for (char* p = buf; *p && g_nlist < 8;) {
        char* e = nullptr; long v = strtol(p, &e, 10);
        if (e == p) { p++; continue; }
        g_list[g_nlist++] = (int)v; p = e;
    }
    g_alt_on = config_row("X2-DIAG-FTBLINK-ALT", 0);
    if (g_alt_on) {
        // FUN_007bb1e0: PUSH EBP; MOV EBP,ESP; MOV ECX,[EBP+8]; SUB ESP,0x10; MOVZX EAX,byte [ECX]; MOV [0x01725500],AL; ...
        uint8_t* c = sig_find("X2-DIAG-FTBLINK colour setter FUN_007bb1e0",
            "55 8B EC 8B 4D 08 83 EC 10 0F B6 01 A2 ?? ?? ?? ?? 0F B6 41 04 A2 ?? ?? ?? ?? 0F B6 41 08 A2", 0x7BB1E0);
        uint8_t* rgba = c ? abs_operand("light RGBA", c + 13) : nullptr;
        if (rgba) g_light = rgba - 0x480;
        log_line("  X2-DIAG-FTBLINK: light globals gva=0x%08X (expected 0x01725080)", g_light ? rt_to_gva(g_light) : 0);
    }
    if (!magic_mods_subscribe("X2-DIAG-FTBLINK", on_load, on_unload)) return false;
    log_line("  X2-DIAG-FTBLINK: state ptr gva=0x%08X (expected 0x00DCED88), hdframe ptr gva=0x%08X (expected 0x00DCDA04)",
             rt_to_gva(g_state), rt_to_gva(g_hdframe));
    return true;
}

Row row_DIAGFTBLINK = {
    "X2-DIAG-FTBLINK",
    "diagnostic: HD model list count / clear state per invocation while magic_0681 (or listed DLLs) is loaded",
    0, install, nullptr, post, 0
};

} // namespace x2
