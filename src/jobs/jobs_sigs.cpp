// SPDX-License-Identifier: Apache-2.0
// Signatures for FFX.exe (BuildID 25501027). Each pattern must match exactly once in the executable sections;
// the expected gva is only logged. Evidence for every site: re/ledger/JOBS-CHR.md.
#include "jobs.h"
#include <string.h>

namespace jobs {

uint8_t* g_ply = nullptr;

uint8_t* exe_base() { return (uint8_t*)GetModuleHandleW(nullptr); }

uint32_t rt_to_gva(const void* rt) { return (uint32_t)((const uint8_t*)rt - exe_base()) + 0x400000u; }

struct Pattern { uint8_t b[96]; uint8_t m[96]; int len; };

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool parse_pattern(const char* s, Pattern* p) {
    p->len = 0;
    while (*s) {
        if (*s == ' ') { s++; continue; }
        if (p->len >= (int)sizeof(p->b)) return false;
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

static bool pattern_matches(const uint8_t* at, const Pattern& p) {
    for (int i = 0; i < p.len; i++) if (p.m[i] && at[i] != p.b[i]) return false;
    return true;
}

// Hits in the executable sections of FFX.exe, counting stops at 2 ("more than one").
static int scan_text(const Pattern& p, uint8_t** first) {
    uint8_t* base = exe_base();
    *first = nullptr;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int hits = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* lo = base + sec[i].VirtualAddress;
        const uint8_t* hi = lo + sec[i].Misc.VirtualSize;
        for (const uint8_t* a = lo; a + p.len <= hi; a++) {
            if (p.m[0] && a[0] != p.b[0]) continue;
            if (pattern_matches(a, p)) { if (!*first) *first = (uint8_t*)a; if (++hits > 1) return 2; }
        }
    }
    return hits;
}

static uint8_t* sig_find(const char* label, const char* pattern, uint32_t expected_gva) {
    Pattern p;
    if (!parse_pattern(pattern, &p)) { log_line("  sig %-28s BAD PATTERN (bug)", label); return nullptr; }
    uint8_t* first = nullptr;
    int hits = scan_text(p, &first);
    if (hits != 1) {
        log_line("  sig %-28s %s (%d hits)", label, hits == 0 ? "NOT FOUND" : "AMBIGUOUS", hits);
        return nullptr;
    }
    uint32_t gva = rt_to_gva(first);
    log_line("  sig %-28s rt=%p gva=0x%08X expected=0x%08X %s", label, first, gva, expected_gva,
             gva == expected_gva ? "match" : "SHIFTED");
    return first;
}

// FUN_00785330 (JOB-C12 getter): `IMUL EAX,EAX,0x94 / ADD EAX,<party block>` at +0x16.
static const char* SIG_PLY_GETTER =
    "55 8B EC 8B 45 08 25 FF 00 00 00 83 F8 11 77 0D 69 C0 94 00 00 00 05 ?? ?? ?? ?? 5D C3";

bool sigs_text_ready() {
    Pattern p;
    if (!parse_pattern(SIG_PLY_GETTER, &p)) return false;
    uint8_t* first = nullptr;
    return scan_text(p, &first) == 1;
}

bool sigs_resolve_all() {
    log_line("signatures:");
    uint8_t* getter = sig_find("ply_getter (FUN_00785330)", SIG_PLY_GETTER, 0x785330);
    if (!getter) return false;
    g_ply = (uint8_t*)(uintptr_t)*(const uint32_t*)(getter + 0x17);
    log_line("  party block rt=%p (gva 0x%08X)", g_ply, rt_to_gva(g_ply));
    return true;
}

bool safe_copy(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace jobs
