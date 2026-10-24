// SPDX-License-Identifier: Apache-2.0
// X2-SAVE-001: the save / load list shows story completion with its decimal ("16.4%" instead of "16%").
//
// The game counts completion in steps of 0.2%: a save holds a total of 525 story events and a table of one bit per
// event, cleared when the event is seen; completion = (total - bits still set) / 5 percent. The 64-byte header at the
// start of each save file, which is all the list reads, keeps only the whole number (header byte 0xC). This row
// redirects two calls in the list builder (FUN_0051dd00): the _wfopen of each save file, to remember its path, and the
// formatter of the completion text (FUN_00488070 with text 0x39A), which then reads that file's event table and adds
// the tenths digit after the number. A file whose table does not give the header's whole number keeps the stock text.
// Evidence: re/ledger/SAVE.md.
#include "../x2.h"
#include <stdio.h>
#include <string.h>

namespace x2 {

// Save file layout (offsets from the start of the file; the game state follows the 0x40-byte header)
static const size_t   HEADER_LEN = 0x40;
static const size_t   HEADER_COMPLETION = 0x0C;                     // byte, whole percent
static const size_t   EVENT_TOTAL = HEADER_LEN + 0x138;             // int32, 525 in every save seen
static const size_t   EVENT_BITS = HEADER_LEN + 0x21EC;             // one bit per event not seen yet
static const size_t   EVENT_BITS_LEN = 0x4000;
static const size_t   READ_LEN = EVENT_BITS + EVENT_BITS_LEN;
static const int      EVENTS_PER_PERCENT = 5;
static const int      FULL_TENTHS = 1000;                           // 100.0%
// Game text encoding (every language's glyph table): digit n = 0x30 + n, '.' = 0x48
static const uint8_t  GLYPH_ZERO = 0x30;
static const uint8_t  GLYPH_POINT = 0x48;

typedef FILE*(__cdecl* wfopen_t)(const wchar_t*, const wchar_t*);
typedef void(__cdecl* format_number_t)(const char* text, char* out, size_t cap, unsigned value);

static wfopen_t        g_wfopen = nullptr;                          // the game's _wfopen (from its import table)
static void*           g_wfopen_slot = nullptr;                     // the patched CALL [g_wfopen_slot] lands in h_wfopen
static format_number_t g_format_number = nullptr;                   // FUN_00488070
static wchar_t         g_last_save_path[MAX_PATH];
static volatile LONG   g_shown = 0, g_kept = 0;
extern Row row_SAVE001;

// Completion in tenths of a percent from the first READ_LEN bytes of a save file; -1 when the event table does not
// give the header's whole number (the game writes the header byte from the same formula, capped at 100).
static int completion_tenths(const uint8_t* file, size_t len) {
    if (len < READ_LEN) return -1;
    int32_t total;
    memcpy(&total, file + EVENT_TOTAL, sizeof total);
    int not_seen = 0;
    for (size_t i = 0; i < EVENT_BITS_LEN; ++i)
        for (uint8_t b = file[EVENT_BITS + i]; b; b &= b - 1) ++not_seen;
    int seen = total - not_seen;
    if (total <= 0 || seen < 0) return -1;
    int whole = seen / EVENTS_PER_PERCENT;
    if (whole > 100) whole = 100;
    if (whole != file[HEADER_COMPLETION]) return -1;
    int tenths = seen * 10 / EVENTS_PER_PERCENT;
    return tenths > FULL_TENTHS ? FULL_TENTHS : tenths;
}

// The formatter copied `text` into `out` with `whole` in decimal in place of its number placeholder (which starts
// with control byte 0x05, never a digit). Inserts '.' and the tenths digit after that number; false = out unchanged.
static bool add_tenths_digit(const char* text, char* out, size_t cap, unsigned whole, int tenth) {
    if (tenth < 0 || tenth > 9) return false;
    char digits[16];
    int n = snprintf(digits, sizeof digits, "%u", whole);
    size_t at = 0;
    while (text[at] && text[at] == out[at]) ++at;
    if (n <= 0 || strncmp(out + at, digits, (size_t)n) != 0) return false;
    size_t len = strlen(out);
    if (len + 2 >= cap) return false;
    char* after = out + at + n;
    memmove(after + 2, after, strlen(after) + 1);
    after[0] = (char)GLYPH_POINT;
    after[1] = (char)(GLYPH_ZERO + tenth);
    return true;
}

static FILE* __cdecl h_wfopen(const wchar_t* path, const wchar_t* mode) {
    if (path) wcsncpy_s(g_last_save_path, path, _TRUNCATE);
    return g_wfopen(path, mode);
}

static void __cdecl h_format_completion(const char* text, char* out, size_t cap, unsigned whole) {
    g_format_number(text, out, cap, whole);
    if (!row_SAVE001.on || g_disabled || !g_last_save_path[0]) return;
    static uint8_t file[READ_LEN];
    size_t got = 0;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_last_save_path, L"rb") == 0 && f) {
        got = fread(file, 1, READ_LEN, f);
        fclose(f);
    }
    int tenths = completion_tenths(file, got);
    if (tenths >= 0 && tenths < FULL_TENTHS && (unsigned)(tenths / 10) == whole &&
        add_tenths_digit(text, out, cap, whole, tenths % 10)) {
        InterlockedIncrement(&g_shown);
    } else {
        InterlockedIncrement(&g_kept);
    }
}

static bool install() {
    // FUN_0051dd00: LEA EAX,[EBP-0x404] (path); PUSH mode; PUSH EAX; CALL [_wfopen]; then fread of the 0x40 header
    uint8_t* open_at = sig_find("X2-SAVE-001 save list _wfopen call",
        "8D 85 FC FB FF FF 68 ?? ?? ?? ?? 50 FF 15 ?? ?? ?? ?? 8B F0 83 C4 1C 85 F6 0F 84 ?? ?? ?? ?? 56 6A 40",
        0x51DDBF);
    // FUN_0051dd00: MOVZX ESI,[EBP-0x438] (header completion byte); fetch text 0x39A; CALL FUN_00488070(text,out,0x80,ESI)
    uint8_t* fmt_at = sig_find("X2-SAVE-001 save list completion format call",
        "0F B6 B5 C8 FB FF FF 68 ?? ?? ?? ?? 68 9A 03 00 00 E8 ?? ?? ?? ?? 56 68 80 00 00 00 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? E8",
        0x51E26E);
    if (!open_at || !fmt_at) return false;
    if (fmt_at < open_at || fmt_at > open_at + 0x600) {
        log_line("  X2-SAVE-001: the two call sites are not in the same list builder - refused");
        return false;
    }
    uint8_t* open_operand = open_at + 0x0E;
    uint8_t* fmt_site = fmt_at + 0x26;
    uint8_t* fmt_fn = call_target(fmt_site);
    // FUN_00488070 prologue: PUSH EBP; MOV EBP,ESP; SUB ESP,0x54; cookie; PUSH EBX; MOV EBX,[EBP+8]
    if (!sig_match(fmt_fn, "55 8B EC 83 EC 54 A1 ?? ?? ?? ?? 33 C5 89 45 FC 53 8B 5D 08")) {
        log_line("  X2-SAVE-001: CALL target %p is not the number formatter (prologue mismatch) - refused", fmt_fn);
        return false;
    }
    uint32_t iat_slot = 0, wfopen_fn = 0;
    if (!safe_read_u32(open_operand, &iat_slot) || !safe_read_u32((const void*)(uintptr_t)iat_slot, &wfopen_fn) ||
        !wfopen_fn) {
        log_line("  X2-SAVE-001: _wfopen import slot unreadable - refused");
        return false;
    }
    g_wfopen = (wfopen_t)(uintptr_t)wfopen_fn;
    g_format_number = (format_number_t)fmt_fn;
    g_wfopen_slot = (void*)&h_wfopen;
    if (!patch_call_rel32(fmt_site, fmt_fn, (void*)&h_format_completion)) {
        log_line("  X2-SAVE-001: formatter CALL patch failed");
        return false;
    }
    if (!patch_u32(open_operand, (uint32_t)(uintptr_t)&g_wfopen_slot)) {
        patch_call_rel32(fmt_site, (void*)&h_format_completion, fmt_fn);
        log_line("  X2-SAVE-001: _wfopen CALL patch failed");
        return false;
    }
    log_line("  X2-SAVE-001: _wfopen CALL at gva=0x%08X (import slot gva=0x%08X) and completion format CALL at "
             "gva=0x%08X (formatter gva=0x%08X) -> stubs",
             rt_to_gva(open_at + 0x0C), rt_to_gva((const void*)(uintptr_t)iat_slot), rt_to_gva(fmt_site),
             rt_to_gva(fmt_fn));
    return true;
}

static void post() {
    static LONG reported_shown = 0, reported_kept = 0;
    if (g_shown != reported_shown || g_kept != reported_kept) {
        reported_shown = g_shown;
        reported_kept = g_kept;
        log_line("X2-SAVE-001: completion with decimal on %ld save(s), stock text on %ld", reported_shown,
                 reported_kept);
    }
}

Row row_SAVE001 = {
    "X2-SAVE-001",
    "save / load list: completion shown with its decimal (0.2% steps) read from each save's event table",
    1, install, nullptr, post, 0
};

} // namespace x2
