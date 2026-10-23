// SPDX-License-Identifier: Apache-2.0
// X2-DEV-EFFTEST (test tool, user 22/10/2026: test every spell effect for blinks / crashes without reporting each one).
// Numpad 5 (X2-DEV-EFFTEST-KEY) toggles test mode. While it is on, every battle request for a magic effect plays the
// next untested magic DLL instead: the game's only setter of the effect number, FUN_00857820(int id) (cdecl, plain RET
// 0x857855; `MOV [g_dwMagicOverlayId 0xD6E400],EAX` at 0x85782D; callers FUN_006216b0 / FUN_00644610), gets the test
// number. The number becomes "magic_%04d" in FUN_008571b0. Numbers come from <game>\magicFiles\FFX-2\magic_NNNN.dll
// (language variants count once); numbers in the game's "unknown magic file" table (ints from 0xD6E408 until < 0, which
// FUN_008572d9 answers with a deliberate null write) are skipped. X2-DIAG-BLINK logs each one's verdict at unload,
// X2-DIAG-CRASH any fault. Progress survives restarts in <game>\fps60x2-efftest.txt ("start N" / "done N"): a number
// started but never done (crash, hang, soft reset) is reported at the next start and skipped.
// Effects play on the wrong caster / target, so wrong looks, waits or crashes of a test effect are test artefacts.
// ini [rows]: X2-DEV-EFFTEST=1 (row, default 1), X2-DEV-EFFTEST-KEY=<vk, decimal; 101 = numpad 5>,
// X2-DEV-EFFTEST-FROM=<first number, default 0>.
// X2-DEV-EFFTEST-ONLY=<number, default 0 = off> (user 23/10/2026: Ultima cannot be cast on demand): test mode plays
// that one magic DLL on every request, without the progress file, e.g. 106 = Ultima (ULTIMA-0106.md).
#include "../x2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace x2 {

extern Row row_DEVEFFTEST;

typedef void(__cdecl* set_t)(int id);
static set_t g_set_orig = nullptr;
static uint8_t* g_set = nullptr;
static int g_key = 0x65;
static bool g_on = false, g_held = false;
static uint8_t g_have[1000];          // 1 = DLL exists, 2 = done, 3 = failed before, 4 = unknown table
static int g_from = 0, g_cur = -1, g_total = 0, g_done = 0, g_only = 0;
static char g_prog[MAX_PATH];
static CRITICAL_SECTION g_cs;

static void prog_write(const char* what, int n) {
    FILE* f = nullptr;
    if (fopen_s(&f, g_prog, "a") == 0 && f) { fprintf(f, "%s %04d\n", what, n); fclose(f); }
}

static int next_id() {
    for (int i = g_from; i < 1000; i++) if (g_have[i] == 1) return i;
    return -1;
}

static void __cdecl h_set(int id) {
    if (g_on && !g_disabled && g_only) {
        log_line("X2-DEV-EFFTEST: request magic_%04d -> magic_%04d (ONLY)", id, g_only);
        id = g_only;
    } else if (g_on && !g_disabled) {
        EnterCriticalSection(&g_cs);
        int t = next_id();
        if (t >= 0) {
            g_have[t] = 2;                           // taken: not offered again this run
            g_cur = t;
            g_done++;
            prog_write("start", t);
            log_line("X2-DEV-EFFTEST: request magic_%04d -> test magic_%04d (%d of %d)", id, t, g_done, g_total);
            id = t;
        } else {
            log_line("X2-DEV-EFFTEST: every magic DLL tested - request magic_%04d plays as is", id);
        }
        LeaveCriticalSection(&g_cs);
    }
    g_set_orig(id);
}

static void on_unload_name(int n) {
    EnterCriticalSection(&g_cs);
    if (n >= 0 && n == g_cur) { prog_write("done", n); g_cur = -1; }
    LeaveCriticalSection(&g_cs);
}
static uint8_t* g_loaded_base = nullptr;
static int g_loaded_n = -1;
static void on_load(uint8_t* base, uint32_t, const char* name) {
    const char* p = strstr(name, "magic_");
    if (!p) return;
    g_loaded_base = base;
    g_loaded_n = atoi(p + 6);
}
static void on_unload(uint8_t* base) {
    if (base == g_loaded_base) { on_unload_name(g_loaded_n); g_loaded_base = nullptr; }
}

static void pre() {
    bool down = (GetAsyncKeyState(g_key) & 0x8000) != 0;
    if (down) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        down = pid == GetCurrentProcessId();
    }
    if (down && !g_held) {
        g_on = !g_on;
        int left = 0;
        for (int i = g_from; i < 1000; i++) left += g_have[i] == 1;
        if (g_only) log_line("X2-DEV-EFFTEST: test mode %s (every request plays magic_%04d)", g_on ? "ON" : "off", g_only);
        else log_line("X2-DEV-EFFTEST: test mode %s (%d magic DLLs left, next magic_%04d)", g_on ? "ON" : "off", left, next_id());
    }
    g_held = down;
}

static void scan_files() {
    char dir[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, dir, MAX_PATH);
    while (n && dir[n - 1] != '\\' && dir[n - 1] != '/') n--;
    dir[n] = 0;
    _snprintf_s(g_prog, sizeof(g_prog), _TRUNCATE, "%sfps60x2-efftest.txt", dir);
    char pat[MAX_PATH];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%smagicFiles\\FFX-2\\magic_*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        int v = atoi(fd.cFileName + 6);
        if (v >= 0 && v < 1000 && !g_have[v]) { g_have[v] = 1; g_total++; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void read_progress() {
    FILE* f = nullptr;
    if (fopen_s(&f, g_prog, "r") != 0 || !f) return;
    static uint8_t started[1000];
    char what[16];
    int v;
    int done = 0, failed = 0;
    while (fscanf_s(f, "%15s %d", what, (unsigned)sizeof(what), &v) == 2) {
        if (v < 0 || v >= 1000) continue;
        if (!strcmp(what, "start")) started[v] = 1;
        else if (!strcmp(what, "done")) started[v] = 2;
    }
    fclose(f);
    char list[512] = "";
    for (int i = 0; i < 1000; i++) {
        if (started[i] == 2 && g_have[i] == 1) { g_have[i] = 2; done++; }
        else if (started[i] == 1 && g_have[i] == 1) {
            g_have[i] = 3; failed++;
            size_t l = strlen(list);
            if (l < sizeof(list) - 8) _snprintf_s(list + l, sizeof(list) - l, _TRUNCATE, " %04d", i);
        }
    }
    g_done = done + failed;
    log_line("  X2-DEV-EFFTEST: progress file: %d done, %d started but never finished (crash / hang):%s", done, failed,
             failed ? list : " none");
}

static bool install() {
    InitializeCriticalSection(&g_cs);
    g_key = config_row("X2-DEV-EFFTEST-KEY", 0x65);
    g_from = config_row("X2-DEV-EFFTEST-FROM", 0);
    g_only = config_row("X2-DEV-EFFTEST-ONLY", 0);
    g_set = sig_find("X2-DEV-EFFTEST effect number setter FUN_00857820",
                     "55 8B EC 80 3D ?? ?? ?? ?? 00 8B 45 08 A3 ?? ?? ?? ?? 74 20 83 3D ?? ?? ?? ?? 00 74 17 3D 5B 01 00 00 75 10",
                     0x00857820);
    if (!g_set) return false;
    uint8_t* id_var = abs_operand("effect number", g_set + 14);
    if (!id_var || rt_to_gva(id_var) != 0x00D6E400) { log_line("  X2-DEV-EFFTEST: effect number is not 0xD6E400 - off"); return false; }
    scan_files();
    if (!g_total) { log_line("  X2-DEV-EFFTEST: no magicFiles\\FFX-2\\magic_*.dll found - off"); return false; }
    int unk = 0;
    __try {
        const int* t = (const int*)(id_var + 8);                 // DAT_00d6e408: unknown-file table (FUN_008572d9)
        for (int i = 0; i < 512 && t[i] >= 0; i++)   // 100 entries in the Steam build (22/10)
            if (t[i] < 1000 && g_have[t[i]]) { g_have[t[i]] = 4; unk++; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_only && (g_only >= 1000 || g_have[g_only] != 1)) {
        log_line("  X2-DEV-EFFTEST: ONLY magic_%04d not on disk or in the unknown table - ONLY off", g_only);
        g_only = 0;
    }
    if (!g_only) read_progress();
    if (!hook_entry("X2-DEV-EFFTEST setter", g_set, (void*)&h_set, (void**)&g_set_orig)) return false;
    if (!magic_mods_subscribe("X2-DEV-EFFTEST", on_load, on_unload)) return false;
    log_line("  X2-DEV-EFFTEST: %d magic DLLs on disk (%d in the unknown table, skipped); key 0x%X toggles; progress %s",
             g_total, unk, g_key, g_prog);
    if (g_only) log_line("  X2-DEV-EFFTEST: ONLY magic_%04d (every request in test mode)", g_only);
    return true;
}

Row row_DEVEFFTEST = {
    "X2-DEV-EFFTEST",
    "test tool: numpad 5 toggles effect test mode - each battle effect request plays the next untested magic DLL",
    1, install, pre, nullptr, 0
};

} // namespace x2
