// SPDX-License-Identifier: Apache-2.0
// Diagnostic DIAG-DRAWSTEP (default off): compares each magic DLL's writable sections before and after every
// render-only invocation and reports the ranges a draw changed (draw-side stepping that runs 2x). Observe only.
// DIAG-DRAWSTEP-EXE=1 also watches FFX.exe's writable sections (one render-only invocation in EXE_SAMPLE, counts reset
// after each report, so a report covers the scene on screen; U 25/10/2026: title screen water fast at 120).
// Details: docs/fps60/rows/DIAG-DRAWSTEP.md
#include "../fps60.h"
#include <string.h>
#include <stdlib.h>

enum { MAX_MODS = 8, MAX_SECS = 4, REPORT_EVERY = 300, TOP = 12, EXE_SAMPLE = 8, EXE_REPORT_EVERY = 60, EXE_TOP = 60 };

struct Sec { uint8_t* p; uint32_t len; uint8_t* buf; uint16_t* hits; };
struct Mod {
    uint8_t* base;
    uint32_t size;
    char     name[24];
    bool     scanned, dead, snap, exe;
    uint32_t gva_base, skip;
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
        if (!s.buf || !s.hits) { log_line("DIAG-DRAWSTEP: %s no memory - not watched", m.name); free_mod(m); return; }
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
    log_line("DIAG-DRAWSTEP: %s %s: %u render-only draws, %u changed DLL memory (%.1f%%), %llu dwords changed in total",
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
            r[n].off = (uint32_t)(s.p - m.base) + start * 4;
            r[n].len = (last - start + 1) * 4;
            r[n].maxhit = mx;
            n++;
            i = last + 1;
        }
    }
    qsort(r, n, sizeof(Range), cmp_range);
    int top = m.exe ? EXE_TOP : TOP;
    for (int j = 0; j < n && j < top; j++) {
        if (!m.exe) {
            log_line("DIAG-DRAWSTEP:   %s gva 0x%08X len 0x%X changed on %u of %u render-only draws",
                     m.name, m.gva_base + r[j].off, r[j].len, r[j].maxhit, m.windows);
            continue;
        }
        // first dword of the range, before/after the last sampled draw (float accumulators show a small step)
        uint32_t before = 0, after = 0;
        for (int k = 0; k < m.nsec; k++) {
            uint32_t so = (uint32_t)(m.sec[k].p - m.base);
            if (r[j].off < so || r[j].off >= so + m.sec[k].len) continue;
            before = *(uint32_t*)(m.sec[k].buf + (r[j].off - so));
            after = *(uint32_t*)(m.sec[k].p + (r[j].off - so));
        }
        float fb, fa;
        memcpy(&fb, &before, 4);
        memcpy(&fa, &after, 4);
        log_line("DIAG-DRAWSTEP:   %s gva 0x%08X len 0x%X changed on %u of %u render-only draws; first %08X -> %08X (%g -> %g)",
                 m.name, m.gva_base + r[j].off, r[j].len, r[j].maxhit, m.windows, before, after, fb, fa);
    }
    if (n > top) log_line("DIAG-DRAWSTEP:   %s ... %d more range(s)", m.name, n - top);
}

static void reset_counts(Mod& m) {
    for (int k = 0; k < m.nsec; k++) memset(m.sec[k].hits, 0, m.sec[k].len / 2);
    m.windows = m.changed_windows = 0;
    m.changed_dwords = 0;
}

// magic_mods.cpp, loader lock
static void on_load(uint8_t* base, uint32_t size, const char* name) {
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        if (m.base || m.dead) continue;
        memset(&m, 0, sizeof(m));
        m.size = size;
        strncpy(m.name, name, sizeof(m.name) - 1);
        m.gva_base = 0x10000000u;
        m.base = base;
        return;
    }
}

static void on_unload(uint8_t* base) {
    for (int i = 0; i < MAX_MODS; i++) if (g_mods[i].base == base) { g_mods[i].base = nullptr; g_mods[i].dead = true; }
}

static bool install() {
    if (!magic_mods_subscribe("DIAG-DRAWSTEP", on_load, on_unload)) return false;
    log_line("  DIAG-DRAWSTEP: watching magic DLLs for state changed by render-only draws (observe only)");
    if (config_row("DIAG-DRAWSTEP-EXE", 0)) {
        Mod& m = g_mods[MAX_MODS - 1];   // magic DLLs take the free slots from the front
        uint8_t* b = exe_base();
        IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
        m.size = nt->OptionalHeader.SizeOfImage;
        strncpy(m.name, "FFX.exe", sizeof(m.name) - 1);
        m.gva_base = 0x400000u;
        m.exe = true;
        m.base = b;
        log_line("  DIAG-DRAWSTEP-EXE: also FFX.exe writable sections, 1 render-only invocation in %d, report every %d",
                 EXE_SAMPLE, EXE_REPORT_EVERY);
    }
    return true;
}

static void pre() {
    bool build = pacing_kind() == INV_BUILD;
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        m.snap = false;
        if (m.dead) { report(m, "unloaded"); free_mod(m); continue; }
        if (!m.base || !build) continue;
        if (m.exe && (m.skip++ % EXE_SAMPLE) != 0) continue;
        __try {
            if (!m.scanned) scan(m);
            if (!m.nsec || (!m.exe && mag021_in_window(m.base))) continue;
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
        if (++m.since_report >= (m.exe ? (uint32_t)EXE_REPORT_EVERY : (uint32_t)REPORT_EVERY)) {
            m.since_report = 0;
            report(m, m.exe ? "last window" : "so far");
            if (m.exe) reset_counts(m);
        }
    }
}

Row row_DIAG_DRAWSTEP = {
    "DIAG-DRAWSTEP",
    "diagnostic: log magic-DLL memory changed by render-only draws (draw-side stepping, 2x at 60 Hz)",
    0, install, pre, post, 0
};
