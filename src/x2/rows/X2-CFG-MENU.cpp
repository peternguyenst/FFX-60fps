// SPDX-License-Identifier: Apache-2.0
// X2-CFG-MENU: the mod's options as extra rows on the game's own Config page (main menu), in all 8 languages.
//
// How the page works (re/ledger/CFG-MENU-X2.md): the Config page draws an array of 0x20-byte row objects (pointer at
// gva 0x16D5F40, count at 0x16D5F48). Its setup resets that array to the stock rows every time the menu opens, and
// the page already scrolls, so adding rows needs no layout change. Each row has a label, up to 3 values and the
// current value; left / right only change the current value, and when the page closes the game calls every row's
// apply function once.
// What we do:
//   - hook the page's enter function (gva 0x788920): point the array at our copy (the stock rows + ours) and raise the
//     count; the game then calls every row's init, ours included (row_init shows the current state)
//   - our rows' apply (row_apply) switches the option and writes fps60x2.ini, when the page is closed
//   - hook the two text lookups (label / value text and help text): ids 0x7E00..0x7E7F are ours and come from
//     x2_text_strings.h (generated with the game's own glyph tables, tools/gen_text_x2.py); every other id passes on.
//     Each value has its own id, because the help text is looked up by the current value.
// Options: 60 FPS, Smooth Motion, 60 FPS Menu, Soft Reset, Turbo, Idle Motion, V-Sync, Fast Results (battle results at
// 30 fps; default on under Wine / Proton). Dev builds add Load Mode Pacing, Idle-Wait Hold, Battle Setup Hold, F8
// Bisect Tool and the three Smooth rows, in English only.
// An option whose row was off at startup is shown and saved, and takes effect at the next start (rows are never
// installed at runtime). Soft Reset writes the [x2] soft_reset key: off writes 0, on the default button chord.
#include "../x2.h"
#include "../x2_text_strings.h"
#include "x2_smooth.h"
#include <string.h>

namespace x2 {

extern Row row_CFGMENU, row_MENU002, row_SR003, row_DEVTURBO, row_CHR002, row_SMOOTH001, row_SMOOTH002, row_SMOOTH003, row_VSYNC;
#ifdef FPS60_DEV
extern Row row_LOAD001, row_DEVBISECT;
#endif

#pragma pack(push, 1)
struct CfgRow {                                    // one row of the Config page (the game's layout)
    uint16_t enabled;                              // +0x00 written by some stock inits, never read
    int16_t  label;                                // +0x02
    int16_t  nvals;                                // +0x04
    int16_t  vals[3];                              // +0x06
    int16_t  cur;                                  // +0x0C
    uint16_t pad;
    void (__cdecl* init)(CfgRow*);                 // +0x10
    void (__cdecl* apply)(CfgRow*);                // +0x14
    uint32_t vib;                                  // +0x18 (1 only on the stock Vibration row, never read)
    uint32_t mode;                                 // +0x1C 1 = screen-position editor: 0 here
};
#pragma pack(pop)
static_assert(sizeof(CfgRow) == 0x20, "row object");

typedef void (__cdecl* page_enter_fn)(void* desc, int arg);
typedef const uint8_t* (__cdecl* text_fn)(int id);

static uint8_t* g_page_enter = nullptr;
static uint8_t* g_label_text = nullptr;
static uint8_t* g_help_text = nullptr;
static page_enter_fn  g_page_enter_orig = nullptr;
static text_fn   g_label_text_orig = nullptr, g_help_text_orig = nullptr;
static void* volatile*  g_row_array = nullptr;            // [0x16D5F40]
static volatile int8_t* g_row_count = nullptr;            // [0x16D5F48]
static bool g_hooks_installed = false;

enum { ID_LABEL = 0x7E00, ID_VALUE = 0x7E40, MAX_STOCK = 32 };

// --- Our options -----------------------------------------------------------------------------------------------------
// Values are shown in the stock order: ON (index 0), OFF (index 1).
struct Option {
    int  label, help_on, help_off;
    int  (*get)();                                 // 1 = on
    void (*set)(bool on);                          // runtime (installed rows only)
    const char* sect;                              // ini
    const char* key;
    int  want;                                     // the code default at first, then the saved choice
    bool (*live)();                                // the runtime state follows the choice
};

static bool smooth_installed() { return row_SMOOTH001.installed || row_SMOOTH002.installed || row_SMOOTH003.installed; }
static int  get_fps()     { return g_cfg.pacing != 0; }
static void set_fps(bool on) { pacing_set(on); }
static bool live_always() { return true; }
static int  get_smooth()  { return smooth_get_live(); }
static void set_smooth(bool on) { smooth_set_live(on); }
static int  get_menu()    { return menu002_user_on(); }
static void set_menu(bool on) { menu002_set_user(on); }
static bool live_menu()   { return row_MENU002.installed != 0; }
static int  get_reset()   { return row_SR003.on != 0; }
static void set_reset(bool on) { row_set(row_SR003, on); }
static bool live_reset()  { return row_SR003.installed != 0; }
static int  get_turbo()   { return row_DEVTURBO.on != 0; }
static void set_turbo(bool on) { row_set(row_DEVTURBO, on); }
static bool live_turbo()  { return row_DEVTURBO.installed != 0; }
static int  get_idle()    { return row_CHR002.on != 0; }
static void set_idle(bool on) { row_set(row_CHR002, on); }
static bool live_idle()   { return row_CHR002.installed != 0; }
static int  get_vsync()   { return row_VSYNC.on != 0; }
static void set_vsync(bool on) { row_set(row_VSYNC, on); }
static bool live_vsync()  { return row_VSYNC.installed != 0; }
static int  get_results() { return pacing_menudiv2_get(); }
static void set_results(bool on) { pacing_menudiv2_set(on); }
#ifdef FPS60_DEV
// Dev build only (msbuild /p:Fps60Dev=1): developer options, English text in every language (U 19/10/2026).
static int  get_load()    { return row_LOAD001.on != 0; }
static void set_load(bool on) { row_set(row_LOAD001, on); }
static bool live_load()   { return row_LOAD001.installed != 0; }
static int  get_idlehold() { return load_idlehold_get(); }
static void set_idlehold(bool on) { load_idlehold_set(on); }
static int  get_bhold()   { return load_battlehold_get(); }
static void set_bhold(bool on) { load_battlehold_set(on); }
static int  get_bisect()  { return row_DEVBISECT.on != 0; }
static void set_bisect(bool on) { row_set(row_DEVBISECT, on); }
static bool live_bisect() { return row_DEVBISECT.installed != 0; }
static int  get_sm1()     { return row_SMOOTH001.on != 0; }
static void set_sm1(bool on) { row_set(row_SMOOTH001, on); }
static bool live_sm1()    { return row_SMOOTH001.installed != 0; }
static int  get_sm2()     { return row_SMOOTH002.on != 0; }
static void set_sm2(bool on) { row_set(row_SMOOTH002, on); }
static bool live_sm2()    { return row_SMOOTH002.installed != 0; }
static int  get_sm3()     { return row_SMOOTH003.on != 0; }
static void set_sm3(bool on) { row_set(row_SMOOTH003, on); }
static bool live_sm3()    { return row_SMOOTH003.installed != 0; }
#endif

static Option g_options[] = {
    { T_L_FPS,    T_H_FPS_ON,    T_H_FPS_OFF,    get_fps,    set_fps,    "x2",   "pacing",       1, live_always },
    { T_L_SMOOTH, T_H_SMOOTH_ON, T_H_SMOOTH_OFF, get_smooth, set_smooth, "rows", "X2-SMOOTH",    1, smooth_installed },
    { T_L_MENU,   T_H_MENU_ON,   T_H_MENU_OFF,   get_menu,   set_menu,   "rows", "X2-MENU-002",  1, live_menu },
    { T_L_RESET,  T_H_RESET_ON,  T_H_RESET_OFF,  get_reset,  set_reset,  "x2",   "soft_reset",   1, live_reset },
    { T_L_TURBO,  T_H_TURBO_ON,  T_H_TURBO_OFF,  get_turbo,  set_turbo,  "rows", "X2-DEV-TURBO", 1, live_turbo },
    { T_L_IDLE,   T_H_IDLE_ON,   T_H_IDLE_OFF,   get_idle,   set_idle,   "rows", "X2-CHR-002",   1, live_idle },
    { T_L_VSYNC,  T_H_VSYNC_ON,  T_H_VSYNC_OFF,  get_vsync,  set_vsync,  "rows", "X2-VSYNC",     0, live_vsync },
    { T_L_RESULTS, T_H_RESULTS_ON, T_H_RESULTS_OFF, get_results, set_results, "rows", "X2-PACE-MENUDIV2", 0, live_always },
#ifdef FPS60_DEV
    { T_L_DEV_LOAD,   T_H_DEV_LOAD_ON,   T_H_DEV_LOAD_OFF,   get_load,     set_load,     "rows", "X2-LOAD-001",            1, live_load },
    { T_L_DEV_IDLE,   T_H_DEV_IDLE_ON,   T_H_DEV_IDLE_OFF,   get_idlehold, set_idlehold, "rows", "X2-LOAD-001-IDLEHOLD",   0, live_always },
    { T_L_DEV_BATTLE, T_H_DEV_BATTLE_ON, T_H_DEV_BATTLE_OFF, get_bhold,    set_bhold,    "rows", "X2-LOAD-001-BATTLEHOLD", 0, live_always },
    { T_L_DEV_BISECT, T_H_DEV_BISECT_ON, T_H_DEV_BISECT_OFF, get_bisect,   set_bisect,   "rows", "X2-DEV-BISECT",          1, live_bisect },
    { T_L_DEV_SM1,    T_H_DEV_SM1_ON,    T_H_DEV_SM1_OFF,    get_sm1,      set_sm1,      "rows", "X2-SMOOTH-001",          1, live_sm1 },
    { T_L_DEV_SM2,    T_H_DEV_SM2_ON,    T_H_DEV_SM2_OFF,    get_sm2,      set_sm2,      "rows", "X2-SMOOTH-002",          1, live_sm2 },
    { T_L_DEV_SM3,    T_H_DEV_SM3_ON,    T_H_DEV_SM3_OFF,    get_sm3,      set_sm3,      "rows", "X2-SMOOTH-003",          1, live_sm3 },
#endif
};
enum { OPTION_COUNT = sizeof(g_options) / sizeof(g_options[0]) };

static CfgRow g_option_rows[OPTION_COUNT];
static void*  g_page_rows[MAX_STOCK + OPTION_COUNT];
static void*  g_stock_rows[MAX_STOCK];
static int    g_stock_row_count = 0;

static int option_of(CfgRow* r) {
    int i = (int)(r - g_option_rows);
    return i >= 0 && i < OPTION_COUNT ? i : -1;
}

static bool shown_on(int i) { return g_options[i].live() ? g_options[i].get() != 0 : g_options[i].want != 0; }

static void __cdecl row_init(CfgRow* r) {
    int i = option_of(r);
    if (i >= 0) r->cur = shown_on(i) ? 0 : 1;
}

static void save_option(int i, bool on) {
    Option& o = g_options[i];
    char v[96];
    if (!strcmp(o.key, "soft_reset")) {
        config_str("x2", "soft_reset", v, sizeof(v), "");
        if (on) { if (!v[0] || !strcmp(v, "0") || !_stricmp(v, "off")) strcpy_s(v, "L1+L2+R1+R2+START+SELECT"); }
        else strcpy_s(v, "0");
    } else strcpy_s(v, on ? "1" : "0");
    if (!config_write(o.sect, o.key, v)) log_line("X2-CFG-MENU: could not write [%s] %s=%s", o.sect, o.key, v);
}

static void __cdecl row_apply(CfgRow* r) {
    int i = option_of(r);
    if (i < 0 || r->cur < 0 || r->cur > 1) return;
    bool on = r->cur == 0;
    if (on == shown_on(i)) return;                 // unchanged
    __try {
        Option& o = g_options[i];
        o.want = on ? 1 : 0;
        if (o.live()) o.set(on);
        save_option(i, on);
        log_line("X2-CFG-MENU: %s -> %s%s", o.key, on ? "ON" : "OFF", o.live() ? "" : " (takes effect at the next start)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("X2-CFG-MENU: %s apply faulted", g_options[i].key);
    }
}

// --- Hooks ---------------------------------------------------------------------------------------------------------------
static void __cdecl hook_page_enter(void* desc, int arg) {
    __try {
        void* arr = *g_row_array;
        int n = *g_row_count;
        if (arr != (void*)g_page_rows && n > 0 && n <= MAX_STOCK) {   // the setup put a stock array back: take it
            memcpy(g_stock_rows, arr, n * sizeof(void*));
            g_stock_row_count = n;
        }
        if (g_stock_row_count > 0) {
            memcpy(g_page_rows, g_stock_rows, g_stock_row_count * sizeof(void*));
            for (int i = 0; i < OPTION_COUNT; i++) g_page_rows[g_stock_row_count + i] = &g_option_rows[i];
            *g_row_array = g_page_rows;
            *g_row_count = (int8_t)(g_stock_row_count + OPTION_COUNT);
            static bool logged = false;
            if (!logged) { logged = true; log_line("X2-CFG-MENU: Config page with %d stock + %d mod rows", g_stock_row_count, OPTION_COUNT); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("X2-CFG-MENU: enter faulted - stock rows");
    }
    g_page_enter_orig(desc, arg);
}

static const uint8_t* __cdecl hook_label_text(int id) {
    int u = id & 0xFFFF;
    if (row_CFGMENU.on) {
        if (u >= ID_LABEL && u < ID_LABEL + OPTION_COUNT) return x2_text(g_options[u - ID_LABEL].label);
        if (u >= ID_VALUE && u < ID_VALUE + 2 * OPTION_COUNT) return x2_text((u - ID_VALUE) & 1 ? T_V_OFF : T_V_ON);
    }
    return g_label_text_orig(id);
}

static const uint8_t* __cdecl hook_help_text(int id) {
    int u = id & 0xFFFF;
    if (row_CFGMENU.on && u >= ID_VALUE && u < ID_VALUE + 2 * OPTION_COUNT) {
        const Option& o = g_options[(u - ID_VALUE) / 2];
        return x2_text((u - ID_VALUE) & 1 ? o.help_off : o.help_on);
    }
    return g_help_text_orig(id);
}

// --- Install (find and check); the hooks go in at the first pre() ----------------------------------------------------
static bool install() {
    // Enter 0x788920: PUSH EBP; MOV EBP,ESP; MOV EAX,[EBP+8]; PUSH ESI; MOV [EAX+0x1C],0; MOV byte [..],1; CALL; CALL;
    // XOR ESI,ESI; CMP byte [count],0; JLE; LEA EBX,[EBX]; MOV EAX,[array]; MOV EAX,[EAX+ESI*4]; PUSH EAX; MOV EAX,[EAX+0x10]; CALL EAX
    g_page_enter = sig_find("X2-CFG-MENU Config page enter",
        "55 8B EC 8B 45 08 56 C7 40 1C 00 00 00 00 C6 05 ?? ?? ?? ?? 01 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 F6 80 3D ?? ?? ?? ?? 00 "
        "7E 23 8D 9B 00 00 00 00 A1 ?? ?? ?? ?? 8B 04 B0 50 8B 40 10 FF D0", 0x788920);
    if (!g_page_enter) return false;
    g_row_count = (volatile int8_t*)abs_operand("Config row count", g_page_enter + 0x23);
    g_row_array = (void* volatile*)abs_operand("Config row array", g_page_enter + 0x31);
    if (!g_row_count || !g_row_array) return false;
    // label call site 0x789B1E: MOV EAX,[array]; MOV EDI,[EBP-0x60]; MOV EAX,[EAX+EDI*4]; MOVSX EAX,word [EAX+2]; PUSH EAX; CALL label
    uint8_t* ls = sig_find("X2-CFG-MENU label call site",
        "A1 ?? ?? ?? ?? 8B 7D A0 8B 04 B8 0F BF 40 02 50 E8 ?? ?? ?? ?? 8B F0", 0x789B1E);
    // help call site 0x7891B4: MOV EAX,[array]; MOV ECX,[EAX+EDX*4]; MOVSX EAX,word [ECX+0xC]; MOVSX EAX,word [ECX+EAX*2+6];
    // PUSH EAX; CALL help; PUSH EAX; CALL help setter
    uint8_t* hs = sig_find("X2-CFG-MENU help call site",
        "A1 ?? ?? ?? ?? 8B 0C 90 0F BF 41 0C 0F BF 44 41 06 50 E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ??", 0x7891B4);
    if (!ls || !hs) return false;
    if (abs_operand("Config row array", ls + 1) != (uint8_t*)g_row_array || abs_operand("Config row array", hs + 1) != (uint8_t*)g_row_array) {
        log_line("  X2-CFG-MENU: call sites read another row array - off");
        return false;
    }
    g_label_text = call_target(ls + 0x10);
    g_help_text = call_target(hs + 0x12);
    // both: PUSH EBP; MOV EBP,ESP; PUSH 0; PUSH [EBP+8]; PUSH 1; CALL text; ADD ESP,0xC; POP EBP; RET
    const char* wrap = "55 8B EC 6A 00 FF 75 08 6A 01 E8 ?? ?? ?? ?? 83 C4 0C 5D C3";
    if (!g_label_text || !g_help_text || !sig_match(g_label_text, wrap) || !sig_match(g_help_text, wrap)) {
        log_line("  X2-CFG-MENU: text functions not as proven - off");
        return false;
    }
    for (int i = 0; i < OPTION_COUNT; i++) {
        CfgRow& r = g_option_rows[i];
        memset(&r, 0, sizeof(r));
        r.label = (int16_t)(ID_LABEL + i);
        r.nvals = 2;
        r.vals[0] = (int16_t)(ID_VALUE + 2 * i);
        r.vals[1] = (int16_t)(ID_VALUE + 2 * i + 1);
        r.vals[2] = 0xFF;
        r.init = row_init;
        r.apply = row_apply;
        Option& o = g_options[i];
        if (!strcmp(o.key, "X2-PACE-MENUDIV2") || !strcmp(o.key, "X2-LOAD-001-IDLEHOLD") ||
            !strcmp(o.key, "X2-LOAD-001-BATTLEHOLD")) o.want = on_wine() ? 1 : 0;   // on under Wine / Proton
        o.want = !strcmp(o.key, "soft_reset") ? config_row("X2-SR-003", 1)
               : !strcmp(o.key, "pacing") ? g_cfg.pacing : config_row(o.key, o.want);
    }
    x2_text(0);                                     // resolve the language pointer now (sig scan), not on the first draw
    log_line("  X2-CFG-MENU: enter gva=0x%08X (0x00788920), rows gva=0x%08X (0x016D5F40) count gva=0x%08X (0x016D5F48), "
             "label text gva=0x%08X (0x00779170), help text gva=0x%08X (0x00779150); %d mod rows, ids 0x%X+",
             rt_to_gva(g_page_enter), rt_to_gva((const void*)g_row_array), rt_to_gva((const void*)g_row_count), rt_to_gva(g_label_text),
             rt_to_gva(g_help_text), OPTION_COUNT, ID_LABEL);
    return true;
}

static void pre() {
    if (g_hooks_installed) return;
    g_hooks_installed = true;
    if (!hook_entry("X2-CFG-MENU label text", g_label_text, (void*)&hook_label_text, (void**)&g_label_text_orig)) goto fail;
    if (!hook_entry("X2-CFG-MENU help text", g_help_text, (void*)&hook_help_text, (void**)&g_help_text_orig)) { hook_remove(g_label_text); goto fail; }
    if (!hook_entry("X2-CFG-MENU enter", g_page_enter, (void*)&hook_page_enter, (void**)&g_page_enter_orig)) {
        hook_remove(g_label_text); hook_remove(g_help_text); goto fail;
    }
    log_line("X2-CFG-MENU: hooked on the game thread");
    return;
fail:
    log_line("X2-CFG-MENU: hook failed - row off");
    row_CFGMENU.on = 0;
}

// Dev hot reload: the game's Config row array must not point into this copy; the stock rows go back (a copy of them
// in this copy's memory, which stays mapped; the next copy's enter hook takes it as the stock array).
static void stop() {
    __try {
        if (g_row_array && *g_row_array == (void*)g_page_rows && g_stock_row_count > 0) { *g_row_array = g_stock_rows; *g_row_count = (int8_t)g_stock_row_count; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

Row row_CFGMENU = {
    "X2-CFG-MENU",
    "the mod's options (60 FPS, Smooth Motion, 60 FPS Menu, Soft Reset, Turbo, Idle Motion, V-Sync) on the main-menu Config page",
    1, install, pre, nullptr, 0, 0, stop
};

} // namespace x2
