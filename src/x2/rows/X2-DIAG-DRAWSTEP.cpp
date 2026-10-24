// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-DRAWSTEP (diagnostic, off, observe only): finds spell DLLs that advance their own state while drawing.
//
// It compares each loaded spell DLL's writable memory before and after every render-only invocation and logs the
// ranges that changed: whatever changes there was stepped by a draw, which runs at double speed at 60 fps. Addresses
// are given as DLL Ghidra addresses (base 0x10000000).
#include "../x2.h"
#include <string.h>
#include <stdlib.h>

namespace x2 {

enum { MAX_MODS = 8, MAX_SECS = 4, REPORT_EVERY = 300, TOP = 12 };

struct Sec { uint8_t* p; uint32_t len; uint8_t* buf; uint16_t* hits; };
struct Mod {
    uint8_t* base;
    uint8_t* load_base;  // kept after unload (base goes null), for DLL-VA ranges in the report
    uint32_t size;
    char     name[24];
    bool     scanned, dead, snap;
    Sec      sec[MAX_SECS];
    int      nsec;
    uint32_t windows, changed_windows, since_report, reports;
    uint64_t changed_dwords;
};
static Mod g_mods[MAX_MODS];

static void free_mod(Mod& m) {
    for (int k = 0; k < m.nsec; k++) {
        if (m.sec[k].buf) VirtualFree(m.sec[k].buf, 0, MEM_RELEASE);
        if (m.sec[k].hits) VirtualFree(m.sec[k].hits, 0, MEM_RELEASE);
    }
    memset(&m, 0, sizeof(m));
}

static void scan(Mod& m) {
    m.scanned = true;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)m.base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(m.base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && m.nsec < MAX_SECS; i++) {
        uint32_t ch = sec[i].Characteristics;
        if (!(ch & IMAGE_SCN_MEM_WRITE) || (ch & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_SHARED))) continue;
        uint32_t len = sec[i].Misc.VirtualSize & ~3u;
        if (!len || sec[i].VirtualAddress + len > m.size) continue;
        Sec& s = m.sec[m.nsec];
        s.p = m.base + sec[i].VirtualAddress;
        s.len = len;
        s.buf = (uint8_t*)VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        s.hits = (uint16_t*)VirtualAlloc(nullptr, len / 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!s.buf || !s.hits) { log_line("X2-DIAG-DRAWSTEP: %s no memory - not watched", m.name); free_mod(m); return; }
        m.nsec++;
    }
}

struct Range { uint32_t off; uint32_t len; uint32_t maxhit; };
static int cmp_range(const void* a, const void* b) {
    uint32_t x = ((const Range*)a)->maxhit, y = ((const Range*)b)->maxhit;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void report(Mod& m, const char* why) {
    m.reports++;
    log_line("X2-DIAG-DRAWSTEP: %s %s: %u render-only draws, %u changed DLL memory (%.1f%%), %llu dwords changed in total",
             m.name, why, m.windows, m.changed_windows, m.windows ? 100.0 * m.changed_windows / m.windows : 0.0,
             m.changed_dwords);
    if (!m.changed_windows) return;
    static Range r[4096];
    int n = 0;
    for (int k = 0; k < m.nsec; k++) {
        Sec& s = m.sec[k];
        uint32_t nd = s.len / 4, i = 0;
        while (i < nd && n < 4096) {
            if (!s.hits[i]) { i++; continue; }
            uint32_t start = i, mx = 0, gap = 0, last = i;
            for (; i < nd && gap <= 2; i++) {
                if (s.hits[i]) { gap = 0; last = i; if (s.hits[i] > mx) mx = s.hits[i]; }
                else gap++;
            }
            r[n].off = (uint32_t)(s.p - m.load_base) + start * 4;
            r[n].len = (last - start + 1) * 4;
            r[n].maxhit = mx;
            n++;
            i = last + 1;
        }
    }
    qsort(r, n, sizeof(Range), cmp_range);
    for (int j = 0; j < n && j < TOP; j++)
        log_line("X2-DIAG-DRAWSTEP:   %s gva 0x%08X len 0x%X changed on %u of %u render-only draws",
                 m.name, 0x10000000u + r[j].off, r[j].len, r[j].maxhit, m.windows);
    if (n > TOP) log_line("X2-DIAG-DRAWSTEP:   %s ... %d more range(s)", m.name, n - TOP);
}

// magic_mods.cpp, loader lock
static void on_load(uint8_t* base, uint32_t size, const char* name) {
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        if (m.base || m.dead) continue;
        memset(&m, 0, sizeof(m));
        m.size = size;
        strncpy(m.name, name, sizeof(m.name) - 1);
        m.base = base;
        m.load_base = base;
        return;
    }
}

static void on_unload(uint8_t* base) {
    for (int i = 0; i < MAX_MODS; i++) if (g_mods[i].base == base) { g_mods[i].base = nullptr; g_mods[i].dead = true; }
}

static bool install() {
    if (!magic_mods_subscribe("X2-DIAG-DRAWSTEP", on_load, on_unload)) return false;
    log_line("  X2-DIAG-DRAWSTEP: watching magic DLLs for state changed by render-only draws (observe only)");
    return true;
}

static void pre() {
    bool build = render_only();
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        m.snap = false;
        if (m.dead) { report(m, "unloaded"); free_mod(m); continue; }
        if (!m.base || !build) continue;
        __try {
            if (!m.scanned) scan(m);
            if (!m.nsec) continue;
            for (int k = 0; k < m.nsec; k++) memcpy(m.sec[k].buf, m.sec[k].p, m.sec[k].len);
            m.snap = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { m.snap = false; }
    }
}

static void post() {
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        if (!m.snap || !m.base) continue;
        m.snap = false;
        uint32_t changed = 0;
        __try {
            for (int k = 0; k < m.nsec; k++) {
                const uint32_t* a = (const uint32_t*)m.sec[k].p;
                const uint32_t* b = (const uint32_t*)m.sec[k].buf;
                uint32_t nd = m.sec[k].len / 4;
                for (uint32_t j = 0; j < nd; j++)
                    if (a[j] != b[j]) { changed++; if (m.sec[k].hits[j] < 0xFFFF) m.sec[k].hits[j]++; }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        m.windows++;
        if (changed) { m.changed_windows++; m.changed_dwords += changed; }
        if (++m.since_report >= REPORT_EVERY) { m.since_report = 0; report(m, "so far"); }
    }
}

Row row_DIAGDRAWSTEP = {
    "X2-DIAG-DRAWSTEP",
    "diagnostic: log magic-DLL memory changed by render-only draws (draw-side stepping, 2x at 60 Hz)",
    0, install, pre, post, 0
};

} // namespace x2
