// SPDX-License-Identifier: Apache-2.0
// CFG-MENU (default on): the mod's user options as extra rows of the game's own main-menu Config page. Static RE:
// re/ledger/CONFIG-MENU.md. Ghidra VAs below.
//
// - The Config page (menu page 10) is native: a pointer array of 0x2C-byte row objects (DAT_0186a44c, count
//   DAT_0186a448, fixed 8 by the setup) drawn at 740/count spacing with no scroll; left/right cycle a row's value and
//   call its apply at once (CONFIG-MENU-004..007, 015).
// - Cfg_PageEnter 0x8CB3C0 (cdecl(desc), plain RET) is detoured: before it runs, the array is pointed at a mod copy
//   (the stock rows + ours) and the count raised. The original then resets the cursor and calls every row's init.
//   The cursor/count/array globals are read from the enter's own operands (+0x1C, +0x23, +0x31).
// - Cfg_LabelText 0x8DC9B0 and Cfg_HelpText 0x8DC970 (cdecl(u16 id) -> text, plain RET) are detoured: ids 0xF00+ are
//   ours (stock ids end at 0x26, CONFIG-MENU-010) and return static FFX-charset buffers (the help setter keeps the
//   pointer, CONFIG-MENU-011). ON/OFF value texts are the stock ones (ids 0x0B/0x0C); help is looked up by the
//   value id, so each mod value has its own id.
// - A change applies at once through the rows' runtime paths (row_set, pacing_set, smooth_set). The Config page is
//   reachable from the field menu only, so no battle is running. fps60.ini is written once the page is left (page
//   state desc+0x1C past 3: 4/5/0xB exit).
#include "../fps60.h"
#include "smooth.h"
#include <string.h>
#include <stdio.h>

struct CfgRow {                                     // CONFIG-MENU-004
    uint32_t zero;
    int32_t  nvals;
    int32_t  cur;
    int32_t  selectable;
    void (__cdecl* init)(CfgRow*);
    void (__cdecl* apply)(CfgRow*);
    uint32_t label;
    uint32_t vals[4];
};
static_assert(sizeof(CfgRow) == 0x2C, "Config row object is 0x2C bytes");

typedef void (__cdecl* enter_t)(void* desc);
typedef uint8_t* (__cdecl* text_t)(uint32_t id);
typedef void (__cdecl* draw_t)();

enum { ID_LABEL = 0xF00, ID_VALUE = 0xF40, ID_ON = 0x0B, ID_OFF = 0x0C, MAX_STOCK = 16 };

static uint8_t* g_enter_fn = nullptr;  static enter_t g_enter = nullptr;
static uint8_t* g_label_fn = nullptr;  static text_t  g_label = nullptr;
static uint8_t* g_help_fn = nullptr;   static text_t  g_help = nullptr;
static volatile int8_t*  g_cursor = nullptr;        // DAT_0186a444
static volatile int32_t* g_count = nullptr;         // DAT_0186a448
static void* volatile*   g_array = nullptr;         // DAT_0186a44c
static bool g_hooked = false;
static volatile int32_t* g_desc_state = nullptr;    // page descriptor +0x1C (from the enter's argument)
static bool g_dirty = false;

// CFG-MENU-SCROLL (default 1): the row draw spaces rows 740/count apart and sizes each bar for 8 rows, so 16 rows ran
// into each other (U 16/10/2026; thinner bars cut through the text, same day). Cfg_DrawRows 0x8CBAB0 (no args, plain
// RET, one caller 0x8CB33A) is detoured: around the original, the row array, count and cursor are pointed at a window
// of the stock row count (8) that follows the cursor, so the page looks stock and scrolls; up/down still walk the
// whole list. The HD row 0 test (DAT_0186a438, read only by the row draw) is moved off the window when the window
// does not start at row 0.
static uint8_t* g_draw_fn = nullptr; static draw_t g_draw = nullptr;
static volatile int32_t* g_row0 = nullptr;          // DAT_0186a438
static int g_top = 0;

// CFG-MENU-ROWS (default 14; 8 = the stock layout): rows visible at once. The row draw spaces rows 740/count apart
// and sizes each row for 8 (bar 60 + 6 shadow, strip 54, text at +228, cursor at +232), so more rows need a smaller
// layout (16 rows at stock sizes overlapped; bars alone shrunk cut through the text, U 16/10/2026). Six constant loads
// in the Config-only draw functions point at the floats/doubles below instead of the shared .rdata constants:
//   FUN_008cb530 0x8CB548 FLD [60.0] bar | FUN_008cbae0 0x8CBC7D FLD [54.0] strip, 0x8CBB0C FDIVR [740.0] list
//   height, 0x8CBB9F / 0x8CBBF1 FADD [228.0] text y, 0x8CBCF3 FADD [232.0] cursor y.
// With more than 8 visible rows the list is 780 high, bar = spacing - 6, strip = bar - 6, text centred in the bar.
enum { LAY_BAR, LAY_STRIP, LAY_LIST, LAY_TEXT_A, LAY_TEXT_B, LAY_CURSOR, LAY_N };
static uint8_t* g_lay_op[LAY_N];                    // address of each site's absolute operand
static uint32_t g_lay_orig[LAY_N];
static float    g_bar_h = 60.0f, g_strip_h = 54.0f;
static double   g_list_h = 740.0, g_text_y = 228.0, g_cursor_y = 232.0;
static bool     g_lay_patched = false;
static int      g_vis = 14;

static void set_layout(int vis) {
    if (vis <= 8 || !g_lay_patched) { g_bar_h = 60.0f; g_strip_h = 54.0f; g_list_h = 740.0; g_text_y = 228.0; g_cursor_y = 232.0; return; }
    g_list_h = 780.0;
    double s = g_list_h / vis;
    double bar = s - 6.0 < 60.0 ? s - 6.0 : 60.0;
    g_bar_h = (float)bar;
    g_strip_h = (float)(bar - 6.0);
    g_text_y = 220.0 + (bar - 43.0) / 2.0;          // stock: bar 60, text at +228 (text about 43 high)
    if (g_text_y < 220.0) g_text_y = 220.0;
    g_cursor_y = g_text_y + 4.0;
}

// ---------------------------------------------------------------- options
// v = value index as shown: binary rows are ON (0) / OFF (1), the stock order.
// ---------------------------------------------------------------- Frame Rate row
// The rate is any multiple of 30 from 60 to 240 (rate.cpp), plus 30 = the game's own pacing. The row is a spinner:
// three values, lower / current / higher, with the current one always selected in the middle. Left or right moves
// the game's cursor to a neighbour, its apply steps the rate there, and the refresh puts the cursor back in the
// middle with the window rebuilt around the new rate (U 18/10/2026: four fixed values, or three with the third at
// the page edge, looked poor). Past 30 or 240 the outer slot is blank and stepping onto it does nothing. A rate the
// host limiter cannot be moved to is refused by rate_set_fps and the rate stays where it was. PERF-002 may have
// stepped to a lower rate; the row follows what was asked for, not what it stepped to.
//
// The page's text is generated per language (text/strings.json -> text_strings.h, tools/gen_text.py in the
// outer repo), so a row showing numbers would seem to need a string per number per language. It does not:
// digits are 0x30 + d in all eight languages - L_FPS reads "60 FPS" in every one of them - so the numbers can
// be written directly, and the sentences that already name a rate can have that number swapped for the one
// being offered. Nothing here adds a string.
//   label   L_FPS without its leading "60 "            -> "FPS"
//   value   the rate itself, in digits
//   help    H_FPS_ON with its number replaced; H_FPS_OFF as it stands for the 30 fps value
// Only the first run of digits is touched, and only when it reads the number that string is known to carry:
// jp, kr and ch are multi-byte, and a trail byte can look like a digit further along. A translation that does
// not spell the number where it is expected is used exactly as it is.
// CFG-MENU-SPIN (default 1): a Config row's values are centred at 750 + 420 * index in the game's 1920-wide
// design space (Cfg_DrawRow 0x8CBAE0; the x scaler 0x6447E0 is x * 512 / 1920). The stock rows use two, so a
// third value lands at 1590, at the right end of the bar (U 18/10/2026 screenshot). The step and base are
// re-read from .rdata for every value (the selection underline uses the same x), so their operands are
// re-pointed at our own floats (the trick CFG-MENU-ROWS uses for the vertical layout) and a hook on the per-row
// draw sets 750 / 210 while the Frame Rate row draws: its three values sit at 750 / 960 / 1170, inside the span
// of the stock rows' two. Every other row keeps the game's own spacing. Without the patch the spinner still
// works at the stock spacing.
static float g_val_base = 750.0f, g_val_step = 420.0f;
static const float k_base = 750.0f, k_step = 420.0f;             // the game's own
static const float k_spin_base = 750.0f, k_spin_step = 210.0f;   // 750 / 960 / 1170
enum { VAL_N = 5 };                                              // 3 step sites + 2 base sites
static uint8_t* g_val_op[VAL_N];
static uint32_t g_val_orig[VAL_N];
static uint8_t* g_row_fn = nullptr;
typedef void (__cdecl* rowdraw_t)(int, int, int);
static rowdraw_t g_row_draw = nullptr;

enum { FPS_STOCK = 30, FPS_TOP = 240, FPS_STEP = 30, FPS_MID = 1 };
static int k_fps[3] = { 90, 120, 150 };                          // lower / current / higher; 0 = blank slot
static uint8_t g_fps_label[40];
static uint8_t g_fps_val[3][8];
static uint8_t g_fps_help[3][128];
static int g_fps_lang = -1;

static bool fps_digit(uint8_t c) { return c >= 0x30 && c <= 0x39; }

static int fps_put(uint8_t* out, int n) {                        // n as digits; returns the length
    uint8_t tmp[8];
    int k = 0;
    if (n <= 0) { out[0] = (uint8_t)0x30; return 1; }
    while (n > 0 && k < (int)sizeof(tmp)) { tmp[k++] = (uint8_t)(0x30 + n % 10); n /= 10; }
    for (int i = 0; i < k; i++) out[i] = tmp[k - 1 - i];
    return k;
}

static void fps_copy(const uint8_t* src, uint8_t* out, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) out[i] = src[i];
    out[i] = 0;
}

// src with its first digit run replaced by n, when that run reads `want`; otherwise src unchanged.
static void fps_sub(const uint8_t* src, int want, int n, uint8_t* out, int cap) {
    int len = 0;
    while (src[len] && len < 1024) len++;
    int a = 0;
    while (a < len && !fps_digit(src[a])) a++;
    int b = a;
    int run = 0;
    while (b < len && fps_digit(src[b])) { run = run * 10 + (src[b] - 0x30); b++; }
    uint8_t num[8];
    int nn = fps_put(num, n);
    if (a == len || run != want || a + nn + (len - b) >= cap) { fps_copy(src, out, cap); return; }
    int k = 0;
    for (int i = 0; i < a; i++) out[k++] = src[i];
    for (int i = 0; i < nn; i++) out[k++] = num[i];
    for (int i = b; i < len; i++) out[k++] = src[i];
    out[k] = 0;
}

static void build_fps_text() {
    g_fps_lang = ffx_text_lang();
    const uint8_t* lab = ffx_text(T_L_FPS);
    int i = 0;
    while (fps_digit(lab[i])) i++;                               // the leading "60"
    if (i && lab[i] == 0x3A) i++;                                // the space the Latin languages put after it
    fps_copy(lab[i] ? lab + i : lab, g_fps_label, sizeof(g_fps_label));
    int f = k_fps[FPS_MID];
    for (int v = 0; v < 3; v++) {
        int k = k_fps[v] ? fps_put(g_fps_val[v], k_fps[v]) : 0;
        g_fps_val[v][k] = 0;
        // every slot carries the current rate's help: the cursor rests on the middle, and the help setter keeps
        // whichever slot's pointer it was last given while the cursor passes through a neighbour
        if (f > FPS_STOCK) fps_sub(ffx_text(T_H_FPS_ON), 60, f, g_fps_help[v], sizeof(g_fps_help[v]));
        else fps_copy(ffx_text(T_H_FPS_OFF), g_fps_help[v], sizeof(g_fps_help[v]));
    }
}

// The rate in force as the page shows it: 30 = the game's own pacing, else what the player asked for.
static int fps_now() { return g_cfg.pacing ? rate_user_fps() : FPS_STOCK; }

// The window around the current rate; the texts are rebuilt in place, so pointers the game holds stay valid.
static void build_fps_values() {
    int f = fps_now();
    k_fps[0] = f > FPS_STOCK ? f - FPS_STEP : 0;
    k_fps[FPS_MID] = f;
    k_fps[2] = f < FPS_TOP ? f + FPS_STEP : 0;
    build_fps_text();
}

static int  get_fps() {
    if (k_fps[FPS_MID] != fps_now()) build_fps_values();         // changed elsewhere (F-key, hot reload)
    return FPS_MID;
}
static void set_fps(int v) {
    if (v < 0 || v >= 3 || v == FPS_MID || !k_fps[v]) return;   // the middle, or the blank slot past an end
    int f = k_fps[v];
    if (f == FPS_STOCK) pacing_set(false);
    else if (f == rate_user_fps() || rate_set_fps(f)) pacing_set(true);   // refused: left as it was
    build_fps_values();
}
static int  get_smooth()  { return smooth_enabled() ? 0 : 1; }
static void set_smooth(int v) { smooth_set(v == 0); }
static int  row_get(Row& r) { return r.on ? 0 : 1; }
static int  get_sensor()  { return row_get(row_QOLSENSOR); }
static void set_sensor(int v) { row_set(row_QOLSENSOR, v == 0); }
static int  get_ap()      { return row_get(row_QOLAP); }
static void set_ap(int v) { row_set(row_QOLAP, v == 0); }
static int  get_rate()    { return qol_ap_rate() >= 100 ? 0 : 1; }                       // Full / Half
static void set_rate(int v) { qol_ap_set_rate(v == 0 ? 100 : 50); }
static bool ap_on()       { return row_QOLAP.on != 0; }
static int  get_retry()   { return row_get(row_QOLRETRY); }
static void set_retry(int v) { row_set(row_QOLRETRY, v == 0); }
static int  get_reset()   { return row_get(row_SR003); }
static void set_reset(int v) { row_set(row_SR003, v == 0); }
static int  get_turbo()   { return row_get(row_DEVTURBO); }
static void set_turbo(int v) { row_set(row_DEVTURBO, v == 0); }
static int  get_voice()   { return row_get(row_CLK013); }
static void set_voice(int v) { row_set(row_CLK013, v == 0); }
static int  get_saveicon() { return row_QOLSAVEICON.on ? 1 : 0; }                         // ON = stock chocobo shown
static void set_saveicon(int v) { row_set(row_QOLSAVEICON, v == 1); }
static int  get_vsync()   { return row_get(row_VSYNC); }
static void set_vsync(int v) { row_set(row_VSYNC, v == 0); }

struct Opt {                                        // T_* = text_strings.h ids (text/strings.json, per language)
    int  label;
    int  vals[4];                                   // -1 = stock ON / OFF text
    int  help[4];                                   // per value
    int  nvals;
    int  (*get)();
    void (*set)(int v);
    bool (*enabled)();                              // nullptr = always selectable; else greyed out and skipped when false
};

static const Opt k_opts[] = {
    // OPT_FPS: lookup() answers this row from the buffers above, not from these ids (which stay as the
    // fallback if the text module never resolved the language getter).
    { T_L_FPS,    { -1, -1, -1 }, { T_H_FPS_ON, T_H_FPS_ON, T_H_FPS_ON }, 3, get_fps, set_fps },
    { T_L_SMOOTH, { -1, -1 }, { T_H_SMOOTH_ON, T_H_SMOOTH_OFF }, 2, get_smooth, set_smooth },
    { T_L_SENSOR, { -1, -1 }, { T_H_SENSOR_ON, T_H_SENSOR_OFF }, 2, get_sensor, set_sensor },
    { T_L_AP,     { -1, -1 }, { T_H_AP_ON, T_H_AP_OFF }, 2, get_ap, set_ap },
    { T_L_NFAP,   { T_V_FULL, T_V_HALF }, { T_H_NFAP_FULL, T_H_NFAP_HALF }, 2, get_rate, set_rate, ap_on },
    { T_L_RETRY,  { -1, -1 }, { T_H_RETRY_ON, T_H_RETRY_OFF }, 2, get_retry, set_retry },
    { T_L_RESET,  { -1, -1 }, { T_H_RESET_ON, T_H_RESET_OFF }, 2, get_reset, set_reset },
    { T_L_TURBO,  { -1, -1 }, { T_H_TURBO_ON, T_H_TURBO_OFF }, 2, get_turbo, set_turbo },
    { T_L_VOICE,  { -1, -1 }, { T_H_VOICE_ON, T_H_VOICE_OFF }, 2, get_voice, set_voice },     // CLK-013 (opt-in)
    { T_L_SAVEICON, { -1, -1 }, { T_H_SAVEICON_ON, T_H_SAVEICON_OFF }, 2, get_saveicon, set_saveicon },   // QOL-SAVEICON
    { T_L_VSYNC,  { -1, -1 }, { T_H_VSYNC_ON, T_H_VSYNC_OFF }, 2, get_vsync, set_vsync },     // VSYNC (option)
};
enum { N_OPTS = sizeof(k_opts) / sizeof(k_opts[0]), OPT_FPS = 0 };

static CfgRow g_rows[N_OPTS];
static void*  g_arr[MAX_STOCK + N_OPTS];
static void*  g_stock = nullptr;
static int    g_stock_n = 0;

static int opt_index(CfgRow* r) {
    int i = (int)(r - g_rows);
    return i >= 0 && i < N_OPTS ? i : -1;
}

static void refresh(CfgRow* r) {
    int i = opt_index(r);
    if (i < 0) return;
    r->cur = k_opts[i].get();
    r->selectable = !k_opts[i].enabled || k_opts[i].enabled() ? 1 : 0;
}

static void __cdecl row_init(CfgRow* r) { refresh(r); }

static void __cdecl row_apply(CfgRow* r) {
    int i = opt_index(r);
    if (i < 0) return;
    __try {
        k_opts[i].set(r->cur);
        int now = k_opts[i].get();
        if (i == OPT_FPS) log_line("CFG-MENU: Frame Rate %d fps", k_fps[FPS_MID]);   // the spinner re-centres: now != cur
        else if (now != r->cur) log_line("CFG-MENU: option %d could not change (shows value %d)", i, now);
        g_dirty = true;
        for (int k = 0; k < N_OPTS; k++) refresh(&g_rows[k]);   // a row may depend on this one (Non-Fighter AP)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("CFG-MENU: %s apply faulted", k_opts[i].label);
    }
}

// ---------------------------------------------------------------- detours
static void __cdecl h_draw() {
    if (g_disabled || !g_row0 || *g_array != (void*)g_arr || g_stock_n <= 0) { set_layout(8); g_draw(); return; }
    int n = *g_count, vis = n > g_vis ? g_vis : n, cur = *g_cursor;
    set_layout(vis);
    if (n <= vis) { g_draw(); return; }
    if (cur < 0) cur = 0;
    if (cur >= n) cur = n - 1;
    if (cur < g_top) g_top = cur;
    if (cur >= g_top + vis) g_top = cur - vis + 1;
    if (g_top > n - vis) g_top = n - vis;
    if (g_top < 0) g_top = 0;
    int8_t  save_cur = *g_cursor;
    int32_t save_row0 = *g_row0;
    *g_array = &g_arr[g_top];
    *g_count = vis;
    *g_cursor = (int8_t)(cur - g_top);
    if (g_top) *g_row0 = -1;                        // row 0 (HD) not in the window: no window slot is it
    __try { g_draw(); }
    __finally {
        *g_array = g_arr;
        *g_count = n;
        *g_cursor = save_cur;
        *g_row0 = save_row0;
    }
}

static void __cdecl h_enter(void* desc) {
    if (!g_disabled && row_CFGMENU.on) {
        __try {
            void* cur = *g_array;
            int   n = *g_count;
            if (cur == (void*)g_arr) { cur = g_stock; n = g_stock_n; }   // entered again with ours still in place
            else { g_stock = cur; g_stock_n = n; }
            g_top = 0;
            if (cur && n > 0 && n <= MAX_STOCK) {
                memcpy(g_arr, cur, n * sizeof(void*));
                for (int i = 0; i < N_OPTS; i++) g_arr[n + i] = &g_rows[i];
                *g_array = g_arr;
                *g_count = n + N_OPTS;
                g_desc_state = (volatile int32_t*)((uint8_t*)desc + 0x1C);
                static bool once = false;
                if (!once) { once = true; log_line("CFG-MENU: Config page opened: %d stock rows + %d mod rows", n, N_OPTS); }
            } else {
                log_line("CFG-MENU: unexpected Config row list (%p, %d) - left stock", cur, n);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            log_line("CFG-MENU: enter faulted - row off");
            row_CFGMENU.on = 0;
        }
    }
    g_enter(desc);
}

// Cfg_DrawRow(0, 0, idx). The row object is read from the live array rather than trusting idx, so it is right
// whatever CFG-MENU-SCROLL is doing to the window.
static void __cdecl h_rowdraw(int a, int b, int idx) {
    bool spin = false;
    if (!g_disabled && g_val_op[0]) {
        __try {
            void** arr = (void**)*g_array;
            if (arr && idx >= 0 && idx < *g_count) spin = arr[idx] == (void*)&g_rows[OPT_FPS];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_val_base = spin ? k_spin_base : k_base;
    g_val_step = spin ? k_spin_step : k_step;
    g_row_draw(a, b, idx);
}

static uint8_t* lookup(uint32_t id, bool help) {
    id &= 0xFFFF;
    if (g_fps_lang != ffx_text_lang()) build_fps_text();         // the player changed the game's text language
    if (id >= ID_LABEL && id < ID_LABEL + N_OPTS && !help)
        return id - ID_LABEL == OPT_FPS ? g_fps_label : (uint8_t*)ffx_text(k_opts[id - ID_LABEL].label);
    if (id >= ID_VALUE && id < ID_VALUE + N_OPTS * 4) {
        int i = (id - ID_VALUE) / 4, v = (id - ID_VALUE) % 4;
        if (v >= k_opts[i].nvals) v = 0;
        if (i == OPT_FPS) return help ? g_fps_help[v] : g_fps_val[v];
        if (help) return (uint8_t*)ffx_text(k_opts[i].help[v]);
        if (k_opts[i].vals[v] >= 0) return (uint8_t*)ffx_text(k_opts[i].vals[v]);
        return g_label(v == 0 ? ID_ON : ID_OFF);
    }
    return nullptr;
}

static uint8_t* __cdecl h_label(uint32_t id) {
    if (!g_disabled) if (uint8_t* t = lookup(id, false)) return t;
    return g_label(id);
}

static uint8_t* __cdecl h_help(uint32_t id) {
    if (!g_disabled) if (uint8_t* t = lookup(id, true)) return t;
    return g_help(id);
}

// ---------------------------------------------------------------- ini write-back
static void write_ini() {
    g_dirty = false;
    const char* on01[2] = { "1", "0" };              // v 0 = ON
    int fps = fps_now();
    config_write("fps60", "pacing", fps > FPS_STOCK ? "1" : "0");
    if (fps > FPS_STOCK) {
        char fb[8];
        _snprintf_s(fb, sizeof(fb), _TRUNCATE, "%d", fps);
        config_write("fps60", "fps", fb);
    }
    config_write("rows", "SMOOTH", on01[get_smooth()]);
    config_write("rows", "QOL-SENSOR", on01[get_sensor()]);
    config_write("rows", "QOL-AP", on01[get_ap()]);
    if (ap_on()) config_write("rows", "QOL-AP-RATE", get_rate() == 0 ? "100" : "50");
    config_write("rows", "QOL-RETRY", on01[get_retry()]);
    config_write("rows", "DEV-TURBO", on01[get_turbo()]);
    config_write("rows", "CLK-013", on01[get_voice()]);
    config_write("rows", "QOL-SAVEICON", get_saveicon() ? "1" : "0");
    config_write("rows", "VSYNC", on01[get_vsync()]);
    // soft_reset holds the chord: keep it when on, unless it is the off value
    char b[96];
    config_str("fps60", "soft_reset", b, sizeof(b), "");
    bool off_now = !b[0] || strcmp(b, "0") == 0 || _stricmp(b, "off") == 0;
    if (get_reset() == 1) config_write("fps60", "soft_reset", "0");
    else if (off_now && b[0]) config_write("fps60", "soft_reset", "L1+L2+R1+R2+START+SELECT");
    log_line("CFG-MENU: fps60.ini written (0 = on: fps %d, smooth %d, sensor %d, ap %d rate %d, retry %d, reset %d, turbo %d)",
             fps, get_smooth(), get_sensor(), get_ap(), get_rate(), get_retry(),
             get_reset(), get_turbo());
}

// ---------------------------------------------------------------- row
static bool install() {
    // Cfg_PageEnter: PUSH EBP; MOV EBP,ESP; PUSH ESI; CALL; MOV [..],EAX; MOV EAX,[EBP+8]; XOR ESI,ESI;
    // MOV dword [EAX+0x1C],0; MOV byte [cursor],0; CMP [count],ESI; JLE +0x21
    g_enter_fn = sig_find_unique("CFG-MENU Cfg_PageEnter",
        "55 8B EC 56 E8 ?? ?? ?? ?? A3 ?? ?? ?? ?? 8B 45 08 33 F6 C7 40 1C 00 00 00 00 C6 05 ?? ?? ?? ?? 00 39 35 ?? ?? ?? ?? 7E 21",
        0x8CB3C0);
    // Cfg_LabelText: FUN_0078fcf0(7, id, FUN_007851f0()); the tail of the next function makes it unique
    g_label_fn = sig_find_unique("CFG-MENU Cfg_LabelText",
        "55 8B EC E8 ?? ?? ?? ?? 50 FF 75 08 6A 07 E8 ?? ?? ?? ?? 83 C4 0C 5D C3 CC CC CC CC CC CC CC CC 55 8B EC 5D E9",
        0x8DC9B0);
    // Cfg_HelpText: FUN_0078fbb0(7 or 0xE, id, FUN_007851f0())
    g_help_fn = sig_find_unique("CFG-MENU Cfg_HelpText",
        "55 8B EC 56 E8 ?? ?? ?? ?? 8B F0 E8 ?? ?? ?? ?? 56 FF 75 08 85 C0 75 0D 6A 07 E8 ?? ?? ?? ?? 83 C4 0C 5E 5D C3 6A 0E E8",
        0x8DC970);
    if (!g_enter_fn || !g_label_fn || !g_help_fn) return false;
    // enter +0x30: MOV EAX,[array] (the init loop)
    if (!sig_match(g_enter_fn + 0x30, "A1 ?? ?? ?? ?? 8B 04 B0 50 8B 40 10 FF D0")) {
        log_line("  CFG-MENU: Cfg_PageEnter init loop has an unexpected shape - refused");
        return false;
    }
    g_cursor = (volatile int8_t*)read_abs(g_enter_fn + 0x1C);
    g_count = (volatile int32_t*)read_abs(g_enter_fn + 0x23);
    g_array = (void* volatile*)read_abs(g_enter_fn + 0x31);
    if (!g_cursor || !g_count || !g_array) return false;
    if (config_row("CFG-MENU-SCROLL", 1)) {
        // Cfg_DrawRows: PUSH ESI; CALL; XOR ESI,ESI; CMP [count],ESI; JLE; loop { PUSH ESI; PUSH 0; PUSH 0; CALL row; ...}
        g_draw_fn = sig_find_unique("CFG-MENU-SCROLL Cfg_DrawRows",
            "56 E8 ?? ?? ?? ?? 33 F6 39 35 ?? ?? ?? ?? 7E 1B 56 6A 00 6A 00 E8 ?? ?? ?? ?? 83 C4 0C E8 ?? ?? ?? ?? 46 3B 35", 0x8CBAB0);
        g_vis = config_row("CFG-MENU-ROWS", 14);
        if (g_vis < 8) g_vis = 8;
        if (g_vis > 16) g_vis = 16;
        // Layout sites (CFG-MENU-ROWS > 8): pattern, expected VA, operand offset, expected constant
        struct Site { const char* name; const char* pat; uint32_t va; int op; bool dbl; double val; };
        static const Site k_sites[LAY_N] = {
            { "bar height", "D9 05 ?? ?? ?? ?? D9 1C 24 E8 ?? ?? ?? ?? D9 5D FC 83 C4 04 D9 45 FC D8 45 0C 68 00 00 00 26",
              0x8CB548, 2, false, 60.0 },
            { "strip height", "D9 05 ?? ?? ?? ?? D9 1C 24 E8 ?? ?? ?? ?? D9 1C 24 51 D9 05 ?? ?? ?? ?? D9 1C 24 E8 ?? ?? ?? ?? D9 1C 24 51 DD 85 D0 FD FF FF",
              0x8CBC7D, 2, false, 54.0 },
            { "list height", "DB 05 ?? ?? ?? ?? 53 56 8B 75 10 D9 9D F4 FD FF FF 57 D9 85 F4 FD FF FF 56 DC 3D ?? ?? ?? ??",
              0x8CBAF3, 0x1B, true, 740.0 },
            { "text y (HD row)", "50 68 CD 01 00 00 E8 ?? ?? ?? ?? DD 85 D0 FD FF FF DC 05 ?? ?? ?? ?? D9 9D F8 FD FF FF",
              0x8CBB8E, 0x13, true, 228.0 },
            { "text y", "8D 85 FC FD FF FF EB 57 DD 85 D0 FD FF FF 83 EC 08 DC 05 ?? ?? ?? ?? D9 9D F8 FD FF FF",
              0x8CBBE0, 0x13, true, 228.0 },
            { "cursor y", "3B F0 75 40 DD 85 D0 FD FF FF 6A 00 DC 05 ?? ?? ?? ?? 51", 0x8CBCE7, 0x0E, true, 232.0 },
        };
        bool lay_ok = g_vis > 8;
        for (int i = 0; i < LAY_N && lay_ok; i++) {
            char nm[64];
            _snprintf_s(nm, sizeof(nm), _TRUNCATE, "CFG-MENU-ROWS %s", k_sites[i].name);
            uint8_t* at = sig_find_unique(nm, k_sites[i].pat, k_sites[i].va);
            uint8_t* c = at ? read_abs(at + k_sites[i].op) : nullptr;
            double v = !c ? -1.0 : k_sites[i].dbl ? *(double*)c : (double)*(float*)c;
            if (!at || v != k_sites[i].val) {
                log_line("  CFG-MENU-ROWS: %s not found or constant %.1f != %.1f - stock layout, 8 rows", k_sites[i].name, v, k_sites[i].val);
                lay_ok = false;
                break;
            }
            g_lay_op[i] = at + k_sites[i].op;
            g_lay_orig[i] = *(uint32_t*)g_lay_op[i];
        }
        if (!lay_ok) { g_vis = 8; for (auto& op : g_lay_op) op = nullptr; }
        // Cfg_DrawRow 0x8CBB7D: ADD ESP,8; CMP ESI,[row0]; JNZ +0x60; LEA EAX,[EBP-0x204]; PUSH EAX; PUSH 0x1CD; CALL
        uint8_t* r0 = sig_find_unique("CFG-MENU-SCROLL HD row test",
            "83 C4 08 3B 35 ?? ?? ?? ?? 75 60 8D 85 FC FD FF FF 50 68 CD 01 00 00 E8", 0x8CBB7D);
        if (g_draw_fn && r0 && (volatile int32_t*)read_abs(r0 + 5) && read_abs(g_draw_fn + 10) == (uint8_t*)(uintptr_t)g_count) {
            g_row0 = (volatile int32_t*)read_abs(r0 + 5);
        } else {
            g_draw_fn = nullptr;
            log_line("  CFG-MENU-SCROLL: row draw not found - all rows on one page");
        }
    }

    if (config_row("CFG-MENU-SPIN", 1) && g_draw_fn) {
        // The three reads of the value step and the two of the value base, all inside Cfg_DrawRow.
        struct VSite { const char* name; const char* pat; uint32_t va; float want; };
        static const VSite k_vsites[VAL_N] = {
            { "value step (loop head)", "D9 05 ?? ?? ?? ?? 8D 43 1C B9 D0 01 00 00", 0x8CBD62, 420.0f },
            { "value step (HD row)",    "D9 05 ?? ?? ?? ?? 83 C4 04 D9 1C 24 E8 ?? ?? ?? ?? DB 85 F8 FD FF FF", 0x8CBDA1, 420.0f },
            { "value step (loop tail)", "D9 05 ?? ?? ?? ?? 8B 8D F4 FD FF FF 83 AD F0 FD FF FF 02", 0x8CBFFF, 420.0f },
            { "value base (HD row)",    "D9 05 ?? ?? ?? ?? D9 1C 24 E8 ?? ?? ?? ?? DC 85 E0 FD FF FF 8D 85 D8 FD FF FF", 0x8CBDCA, 750.0f },
            { "value base",             "D9 05 ?? ?? ?? ?? D9 1C 24 E8 ?? ?? ?? ?? DC 85 E0 FD FF FF 56", 0x8CBE9D, 750.0f },
        };
        bool ok = true;
        uint8_t* at[VAL_N] = {};
        for (int i = 0; i < VAL_N && ok; i++) {
            char nm[64];
            _snprintf_s(nm, sizeof(nm), _TRUNCATE, "CFG-MENU-SPIN %s", k_vsites[i].name);
            at[i] = sig_find_unique(nm, k_vsites[i].pat, k_vsites[i].va);
            float* c = at[i] ? (float*)read_abs(at[i] + 2) : nullptr;
            if (!c || *c != k_vsites[i].want) {
                log_line("  CFG-MENU-SPIN: %s not found or constant %.1f != %.1f - Frame Rate row at stock spacing",
                         k_vsites[i].name, c ? *c : -1.0f, k_vsites[i].want);
                ok = false;
            }
        }
        // Cfg_DrawRow: the loop's own call target, cdecl(0, 0, row index)
        g_row_fn = ok ? call_target(g_draw_fn + 0x15) : nullptr;
        if (ok && (!g_row_fn || !sig_match(g_row_fn, "55 8B EC 81 EC 34 02 00 00"))) {
            log_line("  CFG-MENU-SPIN: Cfg_DrawRow not at the loop's call target - Frame Rate row at stock spacing");
            ok = false;
        }
        if (ok) {
            void* tgt[VAL_N] = { &g_val_step, &g_val_step, &g_val_step, &g_val_base, &g_val_base };
            for (int i = 0; i < VAL_N && ok; i++) {
                g_val_op[i] = at[i] + 2;
                g_val_orig[i] = *(uint32_t*)g_val_op[i];
                uint32_t v = (uint32_t)(uintptr_t)tgt[i];
                ok = patch_write(g_val_op[i], &v, 4) && ok;
            }
            if (ok) {
                log_line("  CFG-MENU-SPIN: value base/step gva=0x%08X/0x%08X -> %.0f/%.0f while the Frame Rate row draws "
                         "(stock %.0f/%.0f)",
                         rt_to_gva_exe((void*)(uintptr_t)g_val_orig[3]), rt_to_gva_exe((void*)(uintptr_t)g_val_orig[0]),
                         k_spin_base, k_spin_step, k_base, k_step);
            } else {
                for (int i = 0; i < VAL_N; i++) if (g_val_op[i]) patch_write(g_val_op[i], &g_val_orig[i], 4);
                for (auto& op : g_val_op) op = nullptr;
                log_line("  CFG-MENU-SPIN: operand patch failed - Frame Rate row at stock spacing");
            }
        }
        if (!ok) { g_row_fn = nullptr; for (auto& op : g_val_op) op = nullptr; }
    }

    for (int i = 0; i < N_OPTS; i++) {
        const Opt& o = k_opts[i];
        CfgRow& r = g_rows[i];
        memset(&r, 0, sizeof(r));
        r.nvals = o.nvals;
        r.selectable = !o.enabled || o.enabled() ? 1 : 0;
        r.init = row_init;
        r.apply = row_apply;
        r.label = ID_LABEL + i;
        for (int v = 0; v < r.nvals; v++) r.vals[v] = ID_VALUE + i * 4 + v;
    }
    ffx_text_init();
    build_fps_values();
    log_line("  CFG-MENU: Frame Rate spinner, 30 (stock) / 60..240 in 30 steps, now %d", k_fps[FPS_MID]);
    log_line("  CFG-MENU: enter gva=0x%08X label gva=0x%08X help gva=0x%08X; cursor 0x%08X count 0x%08X array 0x%08X; %d rows",
             rt_to_gva_exe(g_enter_fn), rt_to_gva_exe(g_label_fn), rt_to_gva_exe(g_help_fn), rt_to_gva_exe((void*)g_cursor),
             rt_to_gva_exe((void*)g_count), rt_to_gva_exe((void*)g_array), N_OPTS);
    return true;
}

static void pre() {
    if (!g_hooked) {
        g_hooked = true;
        if (!hook_entry("CFG-MENU label", g_label_fn, (void*)&h_label, (void**)&g_label) ||
            !hook_entry("CFG-MENU help", g_help_fn, (void*)&h_help, (void**)&g_help) ||
            !hook_entry("CFG-MENU enter", g_enter_fn, (void*)&h_enter, (void**)&g_enter)) {
            hook_remove("CFG-MENU label", g_label_fn);
            hook_remove("CFG-MENU help", g_help_fn);
            hook_remove("CFG-MENU enter", g_enter_fn);
            log_line("CFG-MENU: hook failed - row off");
            row_CFGMENU.on = 0;
            return;
        }
        log_line("CFG-MENU: Config page hooked on the game thread");
        if (g_row_fn && !hook_entry("CFG-MENU-SPIN row", g_row_fn, (void*)&h_rowdraw, (void**)&g_row_draw)) {
            for (int i = 0; i < VAL_N; i++) if (g_val_op[i]) patch_write(g_val_op[i], &g_val_orig[i], 4);
            for (auto& op : g_val_op) op = nullptr;
            log_line("CFG-MENU-SPIN: row hook failed - Frame Rate row at stock spacing");
        }
        if (g_draw_fn && !hook_entry("CFG-MENU-SCROLL draw", g_draw_fn, (void*)&h_draw, (void**)&g_draw)) g_row0 = nullptr;
        else if (g_draw_fn) {
            if (g_lay_op[0]) {
                void* tgt[LAY_N] = { &g_bar_h, &g_strip_h, &g_list_h, &g_text_y, &g_text_y, &g_cursor_y };
                bool ok = true;
                for (int i = 0; i < LAY_N; i++) {
                    uint32_t v = (uint32_t)(uintptr_t)tgt[i];
                    ok = patch_write(g_lay_op[i], &v, 4) && ok;
                }
                g_lay_patched = ok;
                if (!ok) { log_line("CFG-MENU-ROWS: layout patch failed - 8 rows"); g_vis = 8; }
            }
            log_line("CFG-MENU-SCROLL: %d rows visible, the list scrolls with the cursor", g_vis);
        }
    }
    if (g_dirty && g_desc_state && *g_desc_state > 3) write_ini();
}

// Dev hot reload / off: the stock row list back (the game must not keep pointers into this copy), unsaved changes
// written.
static void stop() {
    if (g_lay_patched) {
        g_lay_patched = false;
        for (int i = 0; i < LAY_N; i++) if (g_lay_op[i]) patch_write(g_lay_op[i], &g_lay_orig[i], 4);
        for (int i = 0; i < VAL_N; i++) if (g_val_op[i]) patch_write(g_val_op[i], &g_val_orig[i], 4);
    }
    __try {
        if (g_array && *g_array == (void*)g_arr && g_stock) { *g_array = g_stock; *g_count = g_stock_n; *g_cursor = 0; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_dirty) write_ini();
}

Row row_CFGMENU = {
    "CFG-MENU",
    "mod options (Frame Rate, Smooth, Sensor, AP, Non-Fighter AP, Retry, Soft Reset, Turbo, Voice Sync, Autosave Chocobo) as rows of the game's Config page",
    1, install, pre, nullptr, 0, stop
};
