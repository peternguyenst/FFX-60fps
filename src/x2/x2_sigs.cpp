// SPDX-License-Identifier: Apache-2.0
// FFX-2 signature scan with byte verification. Every pattern matches exactly once in FFX-2.exe (tested offline on
// the decomp export: outer repo tools/sigs_x2_loop.txt, re/ledger/LOOP.md X2-LOOP rows). A required miss or an
// ambiguous match installs nothing. Expected gva values are logged only.
#include "x2.h"
#include <string.h>

namespace x2 {

set_exec_t         g_set_exec = nullptr;
uint8_t*           g_get_exec = nullptr;
set_divider_t      g_set_divider = nullptr;
uint8_t*           g_sg_mainloop = nullptr;
uint8_t*           g_mainloop_callsite = nullptr;
volatile int8_t*   g_exec_byte = nullptr;
volatile uint32_t* g_divider = nullptr;
volatile int32_t*  g_backlog_count = nullptr;
volatile uint32_t* g_frame_counter = nullptr;

static const uint32_t GHIDRA_BASE = 0x400000;

uint8_t* exe_base() { return (uint8_t*)GetModuleHandleW(nullptr); }
uint32_t rt_to_gva(const void* rt) { return (uint32_t)((const uint8_t*)rt - exe_base()) + GHIDRA_BASE; }

bool safe_read_u32(const void* p, uint32_t* out) {
    __try { *out = *(const volatile uint32_t*)p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_copy(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------- patterns (parsing and matching: x2_pattern.cpp)
// FFX-2.exe's code section (only .text: the scan never looks at other sections).
static bool text_range(uint8_t** lo, uint8_t** hi) {
    uint8_t* base = exe_base();
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            *lo = base + sec[i].VirtualAddress;
            *hi = *lo + sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Hits of `pattern` in FFX-2.exe's .text: 0, 1, 2 (= more than one), -1 = bad pattern or headers.
static int scan(const char* pattern, uint8_t** first) {
    BytePattern p;
    *first = nullptr;
    if (!parse_pattern(pattern, &p)) return -1;
    uint8_t *lo, *hi;
    if (!text_range(&lo, &hi)) return -1;
    return count_hits(lo, hi, p, first);
}

uint8_t* call_target(const uint8_t* site) {
    if (!site || site[0] != 0xE8) return nullptr;
    return (uint8_t*)(site + 5 + *(const int32_t*)(site + 1));
}

bool in_image(const void* p) {
    uint8_t* base = exe_base();
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    return (const uint8_t*)p >= base && (const uint8_t*)p < base + nt->OptionalHeader.SizeOfImage;
}

// Absolute address operand at `at` (the loader relocated it), accepted only inside FFX-2.exe.
uint8_t* abs_operand(const char* name, const uint8_t* at) {
    uint8_t* v = (uint8_t*)(uintptr_t)*(const uint32_t*)at;
    if (!in_image(v)) { log_line("  %-18s operand %p outside FFX-2.exe - REJECTED", name, v); return nullptr; }
    log_line("  %-18s gva=0x%08X", name, rt_to_gva(v));
    return v;
}

uint8_t* sig_find(const char* label, const char* pattern, uint32_t expected_gva) {
    uint8_t* at = nullptr;
    int hits = scan(pattern, &at);
    if (hits != 1) {
        log_line("  sig %-26s %s (%d hits)", label, hits == 0 ? "NOT FOUND" : "AMBIGUOUS", hits);
        return nullptr;
    }
    uint32_t gva = rt_to_gva(at);
    log_line("  sig %-26s gva=0x%08X expected=0x%08X %s", label, gva, expected_gva, gva == expected_gva ? "match" : "SHIFTED");
    return at;
}

bool sig_match(const void* at, const char* pattern) {
    BytePattern p;
    if (!at || !parse_pattern(pattern, &p)) return false;
    __try { return pattern_matches((const uint8_t*)at, p); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------- the X2-LOOP signatures
static const char* const SIG_RUNNER =       // Sg_MainLoopBacklogRunner 0x607020 (loops while backlog != 0)
    "55 8B EC E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? D9 45 08 51 D9 1C 24 E8 ?? ?? ?? ?? 83 C4 04 "
    "E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 33 D2 3B C8 0F 9D C2 4A 23 D1 83 3D ?? ?? ?? ?? 00 89 15 ?? ?? ?? ?? 75 B9";
static const int RUNNER_CALL = 0x1E;        // E8 rel32 -> Sg_MainLoop
static const char* const SIG_MAINLOOP =     // Sg_MainLoop 0x605130 entry, verified at the runner's call target
    "55 8B EC 81 EC D4 00 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC D9 45 08 51 D9 1C 24 E8 ?? ?? ?? ?? 83 C4 04 D9 5D 08 "
    "E8 ?? ?? ?? ?? A3";
static const char* const SIG_SET_EXEC =     // Sg_SetExecFrames 0x6E0130 (exec byte operand at +0x0D)
    "55 8B EC 8B 4D 08 83 F9 63 7E 09 C6 05 ?? ?? ?? ?? 63 5D C3 33 C0 85 C9 0F 98 C0 48 23 C1 A2 ?? ?? ?? ?? 5D C3";
static const char* const SIG_GET_EXEC =     // Sg_GetCurExecFrames 0x6DFE00 (override select / value / raw byte)
    "80 3D ?? ?? ?? ?? 00 0F BE 0D ?? ?? ?? ?? 75 07 0F BE 0D ?? ?? ?? ?? 0F BE 05 ?? ?? ?? ?? 0B C1 C3";
static const char* const SIG_SET_DIVIDER =  // SKS_SetFrameDivider 0xAB49C0 (divider operand at +0x12)
    "55 8B EC 8B 45 08 83 F8 02 73 11 83 F8 01 73 11 C7 05 ?? ?? ?? ?? 01 00 00 00 5D C3 B8 02 00 00 00 A3 ?? ?? ?? ?? 5D C3";
static const char* const SIG_BACKLOG =      // Sg_MainLoop backlog consumer + no-skip ID list (count operand at +1)
    "A1 ?? ?? ?? ?? 83 F8 01 7E 79 83 FF FF 74 38 81 FE CE 02 00 00";
static const char* const SIG_FRAMECOUNT =   // Sg_MainLoop tail frame counter (operand at +1)
    "A1 ?? ?? ?? ?? 40 50 68 ?? ?? ?? ?? A3 ?? ?? ?? ?? E8 ?? ?? ?? ?? 68 80 00 00 00 6A 01 6A 00";

bool sigs_text_ready() {
    uint8_t* at = nullptr;
    return scan(SIG_RUNNER, &at) == 1;
}

bool sigs_resolve_all() {
    log_line("signature scan (FFX-2.exe base %p, Ghidra base 0x%X):", exe_base(), GHIDRA_BASE);
    bool ok = true;

    uint8_t* runner = sig_find("Sg_MainLoopBacklogRunner", SIG_RUNNER, 0x607020);
    if (runner) {
        g_mainloop_callsite = runner + RUNNER_CALL;
        uint8_t* ml = call_target(g_mainloop_callsite);
        BytePattern p;
        if (ml && in_image(ml) && parse_pattern(SIG_MAINLOOP, &p) && pattern_matches(ml, p)) {
            g_sg_mainloop = ml;
            log_line("  Sg_MainLoop        gva=0x%08X (runner call target, %d bytes verified)", rt_to_gva(ml), p.len);
        } else {
            log_line("  Sg_MainLoop        call target %p does not match - REJECTED", ml);
        }
    }
    if (!g_sg_mainloop) ok = false;

    uint8_t* se = sig_find("Sg_SetExecFrames", SIG_SET_EXEC, 0x6E0130);
    if (se) {
        g_set_exec = (set_exec_t)se;
        g_exec_byte = (volatile int8_t*)abs_operand("exec_byte", se + 0x0D);
        // the second store (A2 <abs> at +0x1E) must hit the same byte
        if (g_exec_byte && *(const uint32_t*)(se + 0x1F) != (uint32_t)(uintptr_t)g_exec_byte) {
            log_line("  exec_byte: setter's two stores disagree - REJECTED");
            g_exec_byte = nullptr;
        }
    }
    if (!g_exec_byte) ok = false;

    g_get_exec = sig_find("Sg_GetCurExecFrames", SIG_GET_EXEC, 0x6DFE00);   // optional (rows verify sites with it)

    uint8_t* sd = sig_find("SKS_SetFrameDivider", SIG_SET_DIVIDER, 0xAB49C0);
    if (sd) {
        g_divider = (volatile uint32_t*)abs_operand("divider", sd + 0x12);
        if (g_divider && *(const uint32_t*)(sd + 0x22) != (uint32_t)(uintptr_t)g_divider) {
            log_line("  divider: setter's two stores disagree - REJECTED");
            g_divider = nullptr;
        }
        if (g_divider) g_set_divider = (set_divider_t)sd;
    }
    if (!g_set_divider && g_cfg.pacing) ok = false;

    uint8_t* bl = sig_find("backlog consumer", SIG_BACKLOG, 0x605229);
    if (bl) g_backlog_count = (volatile int32_t*)abs_operand("backlog_count", bl + 1);
    if (!g_backlog_count) ok = false;

    uint8_t* fc = sig_find("frame counter tail", SIG_FRAMECOUNT, 0x605FD6);
    if (fc) g_frame_counter = (volatile uint32_t*)abs_operand("frame_counter", fc + 1);   // optional (logging)

    return ok;
}

} // namespace x2
