// SPDX-License-Identifier: Apache-2.0
// Byte patterns ("55 8B ?? EC", ?? = any byte): reading the text, matching at an address, and counting hits in a
// module's code. One copy for x2_sigs.cpp (FFX-2.exe) and the rows that find code inside spell DLLs (X2-MAG-016,
// X2-MAG-030). Tested by tests/x2/test_dll_scanner.cpp.
#include "x2.h"
#include <string.h>

namespace x2 {

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool parse_pattern(const char* s, BytePattern* p) {
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

bool pattern_matches(const uint8_t* at, const BytePattern& p) {
    for (int i = 0; i < p.len; i++) if (p.m[i] && at[i] != p.b[i]) return false;
    return true;
}

bool matches_at(const uint8_t* at, const char* pattern) {
    BytePattern p;
    return parse_pattern(pattern, &p) && pattern_matches(at, p);
}

int count_hits(const uint8_t* lo, const uint8_t* hi, const BytePattern& p, uint8_t** first) {
    int hits = 0;
    for (const uint8_t* a = lo; a + p.len <= hi; a++) {
        if (p.m[0] && a[0] != p.b[0]) continue;
        if (pattern_matches(a, p)) { if (!hits) *first = (uint8_t*)a; if (++hits > 1) break; }
    }
    return hits;
}

int scan_module(uint8_t* mod, uint32_t size, const char* pattern, uint8_t** first) {
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
        uint8_t* found = nullptr;
        hits += count_hits(mod + va, mod + va + vs, p, &found);
        if (found && !*first) *first = found;
        if (hits > 1) return 2;                 // "more than one": counting stops at 2
    }
    return hits;
}

} // namespace x2
