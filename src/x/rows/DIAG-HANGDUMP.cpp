// SPDX-License-Identifier: Apache-2.0
// Diagnostic DIAG-HANGDUMP (observe only): when the game thread stops completing Sg_MainLoop invocations, a
// watchdog writes fps60_hang.txt (breadcrumbs, game state, lock owners, every thread's stack) and fps60_hang.dmp.
// A stall while the HD pause menu is up is the game's own pause, logged in one line. Ctrl+Shift+H writes the same
// report on request (a soft-lock keeps Sg_MainLoop running, so the watchdog never fires on it).
// Details: docs/fps60/rows/DIAG-HANGDUMP.md
#include "../fps60.h"
#include <stdio.h>
#include <stdarg.h>
#include <dbghelp.h>

static volatile LONG g_count = 0;
static volatile DWORD g_game_tid = 0;
static int    g_sec = 10;
static int    g_data = 1;                        // DIAG-HANGDUMP-DATA: FFX.exe .data/.bss in the minidump
static int    g_key = 1;                         // DIAG-HANGDUMP-KEY: Ctrl+Shift+H writes a report on request
static HANDLE g_txt = INVALID_HANDLE_VALUE, g_dmp = INVALID_HANDLE_VALUE;
static void*  g_minidump = nullptr;              // System32 MiniDumpWriteDump
typedef LONG(NTAPI* NtQSI_t)(ULONG, PVOID, ULONG, PULONG);
static NtQSI_t g_ntqsi = nullptr;
static DWORD  g_watch_tid = 0;

// Game state (Ghidra VAs; resolved by signature at install, nullptr = unresolved):
static uint8_t* g_app_pp = nullptr;      // DAT_00cc9cd8: PApplication*; +0x3AD = pause layer up (animate skips Sg_MainLoop)
static uint8_t* g_esc_pp = nullptr;      // DAT_00ccc870: HD esc-menu Iggy player (FUN_0063da80: +0x70, +0xE4)
static uint8_t* g_k = nullptr;           // DAT_00b412d8: limiter seconds per divider unit (RATE-001)
static uint8_t* g_accum = nullptr;       // DAT_00cc9d00: limiter schedule accumulator
static volatile uint8_t* g_btl_state = nullptr;   // a8e0: battle state (0 = no battle)
static uint8_t* g_btl_result = nullptr;  // a8e3: battle result
static uint8_t* g_btl_timer = nullptr;   // c9ec: end-of-battle timer

// Rings, written by the game thread (battle state) or under the loader lock (magic DLLs), read by the report.
struct BtlEv { DWORD tick; LONG inv; uint8_t from, to; };
static BtlEv g_bs[16];
static volatile LONG g_bs_n = 0;
struct MagEv { DWORD tick; LONG inv; uint8_t* base; char name[24]; bool load; };
static MagEv g_mag[12];
static volatile LONG g_mag_n = 0;

// ---------------------------------------------------------------- report helpers (no heap, no locks)
static char g_line[1024];
static void rep(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    int n = _vsnprintf_s(g_line, sizeof(g_line) - 2, _TRUNCATE, fmt, a);
    va_end(a);
    if (n < 0) n = (int)strlen(g_line);
    g_line[n++] = '\r'; g_line[n++] = '\n';
    DWORD w;
    if (g_txt != INVALID_HANDLE_VALUE) WriteFile(g_txt, g_line, (DWORD)n, &w, nullptr);
}

static bool rd(const void* p, void* out, size_t n) { return safe_copy(out, p, n); }

// Module + Ghidra VA of an address, from VirtualQuery and the module's own PE export name.
static void describe(uint32_t a, char* out, size_t cap) {
    MEMORY_BASIC_INFORMATION mb;
    if (a < 0x10000 || !VirtualQuery((void*)(uintptr_t)a, &mb, sizeof(mb)) || mb.Type != MEM_IMAGE) {
        _snprintf_s(out, cap, _TRUNCATE, "0x%08X", a);
        return;
    }
    uint8_t* base = (uint8_t*)mb.AllocationBase;
    char name[40] = "?";
    if (base == exe_base()) strcpy_s(name, "FFX.exe");
    else {
        IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS32 nt; IMAGE_EXPORT_DIRECTORY ex;
        if (rd(base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE && rd(base + dos.e_lfanew, &nt, sizeof(nt)) &&
            nt.Signature == IMAGE_NT_SIGNATURE &&
            nt.OptionalHeader.NumberOfRvaAndSizes > 0 && nt.OptionalHeader.DataDirectory[0].VirtualAddress &&
            rd(base + nt.OptionalHeader.DataDirectory[0].VirtualAddress, &ex, sizeof(ex)) && ex.Name) {
            rd(base + ex.Name, name, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
        }
    }
    uint32_t off = a - (uint32_t)(uintptr_t)base;
    if (base == exe_base()) _snprintf_s(out, cap, _TRUNCATE, "FFX.exe gva 0x%08X", 0x400000u + off);
    else if (_strnicmp(name, "magic", 5) == 0) _snprintf_s(out, cap, _TRUNCATE, "%s@%p gva 0x%08X", name, base, 0x10000000u + off);
    else _snprintf_s(out, cap, _TRUNCATE, "%s+0x%X", name, off);
}

static bool is_code(uint32_t a) {
    MEMORY_BASIC_INFORMATION mb;
    return a >= 0x10000 && VirtualQuery((void*)(uintptr_t)a, &mb, sizeof(mb)) && mb.Type == MEM_IMAGE &&
           (mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

// A return address: code, and a CALL right before it (E8 rel32, FF /2 with 2..7-byte forms).
static bool is_ret(uint32_t a) {
    if (!is_code(a)) return false;
    uint8_t b[8];
    if (!rd((void*)(uintptr_t)(a - 8), b, 8)) return false;
    if (b[3] == 0xE8) return true;                                   // a-5
    for (int k = 2; k <= 7; k++) if (b[8 - k] == 0xFF && ((b[9 - k] >> 3) & 7) == 2) return true;
    return false;
}

static uint32_t g_stack[2048];                   // 8 KB copy of a thread's stack top

static void report_thread(DWORD tid, const char* tags, ULONG state, ULONG wait) {
    char d[160];
    if (tid == g_watch_tid) return;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) { rep("thread %lu%s: OpenThread failed (%lu)", tid, tags, GetLastError()); return; }
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    int words = 0;
    bool ok = false;
    if (SuspendThread(h) != (DWORD)-1) {
        ok = GetThreadContext(h, &c) != 0;
        if (ok) {
            MEMORY_BASIC_INFORMATION mb;
            uint32_t sp = c.Esp, end = sp + sizeof(g_stack);
            if (VirtualQuery((void*)(uintptr_t)sp, &mb, sizeof(mb))) {
                uint32_t top = (uint32_t)(uintptr_t)mb.BaseAddress + (uint32_t)mb.RegionSize;
                if (end > top) end = top;
            }
            if (end > sp && rd((void*)(uintptr_t)sp, g_stack, end - sp)) words = (int)((end - sp) / 4);
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    if (!ok) { rep("thread %lu%s: no context", tid, tags); return; }
    describe(c.Eip, d, sizeof(d));
    rep("thread %lu%s: state %lu wait %lu, eip %s, esp %08lX ebp %08lX eax %08lX ecx %08lX edx %08lX ebx %08lX esi %08lX edi %08lX",
        tid, tags, state, wait, d, c.Esp, c.Ebp, c.Eax, c.Ecx, c.Edx, c.Ebx, c.Esi, c.Edi);
    // EBP chain (within the copied window)
    uint32_t bp = c.Ebp;
    for (int i = 0; i < 16; i++) {
        if (bp < c.Esp || bp + 8 > c.Esp + (uint32_t)words * 4) break;
        uint32_t next = g_stack[(bp - c.Esp) / 4], ret = g_stack[(bp - c.Esp) / 4 + 1];
        if (!is_code(ret)) break;
        describe(ret, d, sizeof(d));
        rep("    ebp[%d] %s", i, d);
        if (next <= bp) break;
        bp = next;
    }
    // Stack scan: CALL-preceded return addresses
    int shown = 0;
    for (int i = 0; i < words && shown < 28; i++) {
        uint32_t v = g_stack[i];
        if (!is_ret(v)) continue;
        describe(v, d, sizeof(d));
        rep("    [esp+0x%04X] %s", i * 4, d);
        shown++;
    }
}

static uint8_t g_spi[1 << 20];                   // SystemProcessInformation buffer (1 MB)

// The game's own pause: FFXApp_animate 0x42F520 skips Sg_MainLoop while app+0x3AD is set (set after the swap from
// FUN_0063da80, the esc-menu player test). -1 = unknown.
static int paused() {
    uint8_t* app = nullptr;
    uint8_t f = 0;
    if (!g_app_pp || !rd(g_app_pp, &app, 4) || !app || !rd(app + 0x3AD, &f, 1)) return -1;
    return f != 0;
}

static void report_state() {
    uint8_t* app = nullptr;
    uint8_t* esc = nullptr;
    uint8_t f3ad = 0, f39 = 0, esc70 = 0;
    int32_t esce4 = 0;
    bool ok_app = g_app_pp && rd(g_app_pp, &app, 4) && app && rd(app + 0x3AD, &f3ad, 1) && rd(app + 0x39, &f39, 1);
    bool ok_esc = g_esc_pp && rd(g_esc_pp, &esc, 4);
    bool ok_esc2 = ok_esc && esc && rd(esc + 0x70, &esc70, 1) && rd(esc + 0xE4, &esce4, 4);
    if (ok_app) rep("app %p: +0x3AD pause layer %u (Sg_MainLoop skipped while set), +0x39 exit %u", app, f3ad, f39);
    else rep("app: %s", g_app_pp ? "not readable" : "unresolved");
    if (ok_esc2) rep("esc-menu player %p: +0x70 %u, +0xE4 %ld -> menu %s", esc, esc70, (long)esce4,
                     (esc70 || esce4 >= 0) ? "UP" : "down");
    else rep("esc-menu player: %s", !g_esc_pp ? "unresolved" : ok_esc && !esc ? "none" : "not readable");
    double k = 0, acc = 0;
    uint32_t div = 0, fc = 0, vb = 0;
    int32_t bl = 0;
    int8_t ex = 0;
    if (g_divider) rd((void*)g_divider, &div, 4);
    if (g_frame_counter) rd((void*)g_frame_counter, &fc, 4);
    if (g_vblank) rd((void*)g_vblank, &vb, 4);
    if (g_backlog_count) rd((void*)g_backlog_count, &bl, 4);
    if (g_exec_byte) rd((void*)g_exec_byte, &ex, 1);
    if (g_k) rd(g_k, &k, 8);
    if (g_accum) rd(g_accum, &acc, 8);
    rep("limiter: divider %lu, period unit %.9f s, accumulator %.6f s; exec %d, backlog %ld, frame counter %lu, "
        "vblank %lu; pacing %s, inv kind %d, %d fps", div, k, acc, ex, (long)bl, fc, vb, pacing_active() ? "on" : "off",
        pacing_kind(), rate_fps());
    uint8_t bs = 0, br = 0, bt = 0;
    if (g_btl_state) {
        rd((void*)g_btl_state, &bs, 1);
        if (g_btl_result) rd(g_btl_result, &br, 1);
        if (g_btl_timer) rd(g_btl_timer, &bt, 1);
        rep("battle: state 0x%02X, result %u, end timer %u", bs, br, bt);
    } else rep("battle: unresolved");
    DWORD now = GetTickCount();
    LONG n = g_bs_n;
    for (LONG i = n > 16 ? n - 16 : 0; i < n; i++) {
        const BtlEv& e = g_bs[i % 16];
        rep("  battle state 0x%02X -> 0x%02X at inv %ld, %lu ms ago", e.from, e.to, e.inv, now - e.tick);
    }
    n = g_mag_n;
    if (!n) rep("magic DLLs: none loaded or unloaded this session");
    for (LONG i = n > 12 ? n - 12 : 0; i < n; i++) {
        const MagEv& e = g_mag[i % 12];
        rep("  magic %s %s @%p at inv %ld, %lu ms ago", e.load ? "load  " : "unload", e.name, e.base, e.inv, now - e.tick);
    }
}

// Minidump: FFX.exe's .data/.bss (game state) only; other modules' data segments are left out.
static BOOL CALLBACK dump_cb(PVOID, const PMINIDUMP_CALLBACK_INPUT in, PMINIDUMP_CALLBACK_OUTPUT out) {
    switch (in->CallbackType) {
    case ModuleCallback:
        if ((uintptr_t)in->Module.BaseOfImage != (uintptr_t)exe_base()) out->ModuleWriteFlags &= ~ModuleWriteDataSeg;
        return TRUE;
    case IncludeModuleCallback: case IncludeThreadCallback: case ThreadCallback: case ThreadExCallback:
        return TRUE;
    default:
        return FALSE;
    }
}

static void report(LONG count, bool requested) {
    char w[200], l[300];
    LONG where = g_where;
    where_text(where, w, sizeof(w));
    ldr_text(l, sizeof(l));
    DWORD ldr_owner = 0;
    {
        uint8_t* peb = (uint8_t*)(uintptr_t)__readfsdword(0x30);
        uint8_t* ll = nullptr;
        if (rd(peb + 0xA0, &ll, 4) && ll) rd(ll + 0x0C, &ldr_owner, 4);   // RTL_CRITICAL_SECTION.OwningThread
    }
    DWORD log_owner = log_cs_owner();
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (requested)
        rep("fps60 DIAG-HANGDUMP report %04u-%02u-%02u %02u:%02u:%02u: requested (Ctrl+Shift+H), Sg_MainLoop count %ld",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, count);
    else
        rep("fps60 DIAG-HANGDUMP report %04u-%02u-%02u %02u:%02u:%02u: no Sg_MainLoop invocation for %d s (count %ld)",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, g_sec, count);
    rep("game thread %lu: %s", g_game_tid, w);
    rep("%s", l);
    rep("loader lock owner: thread %lu; log CS owner: thread %lu; inv kind %d; MAG-021 %s",
        ldr_owner, log_owner, pacing_kind(), mag021_state());
    report_state();

    // Interesting threads first (in case the full listing gets stuck), then all.
    report_thread(g_game_tid, " [GAME]", 0, 0);
    if (ldr_owner && ldr_owner != g_game_tid) report_thread(ldr_owner, " [LOADER LOCK OWNER]", 0, 0);
    if (log_owner && log_owner != g_game_tid && log_owner != ldr_owner) report_thread(log_owner, " [LOG CS OWNER]", 0, 0);
    rep("---- all threads");
    ULONG len = 0;
    if (g_ntqsi && g_ntqsi(5 /*SystemProcessInformation*/, g_spi, sizeof(g_spi), &len) == 0) {
        DWORD pid = GetCurrentProcessId();
        for (uint32_t off = 0; off + 0xB8 <= sizeof(g_spi);) {
            uint8_t* p = g_spi + off;
            uint32_t next = *(uint32_t*)p, nthr = *(uint32_t*)(p + 4);
            if (*(uint32_t*)(p + 0x44) == pid) {
                for (uint32_t i = 0; i < nthr && i < 512; i++) {
                    uint8_t* th = p + 0xB8 + i * 0x40;
                    if (th + 0x40 > g_spi + sizeof(g_spi)) break;
                    DWORD tid = *(DWORD*)(th + 0x24);
                    char tags[64] = "";
                    if (tid == g_game_tid) strcat_s(tags, " [GAME]");
                    if (tid == ldr_owner) strcat_s(tags, " [LOADER LOCK OWNER]");
                    if (tid == log_owner) strcat_s(tags, " [LOG CS OWNER]");
                    report_thread(tid, tags, *(ULONG*)(th + 0x34), *(ULONG*)(th + 0x38));
                }
                break;
            }
            if (!next) break;
            off += next;
        }
    } else rep("NtQuerySystemInformation failed - other threads not listed");
    rep("---- end of report; minidump follows in fps60_hang.dmp");
    FlushFileBuffers(g_txt);
    if (requested) log_line_try("DIAG-HANGDUMP: report on request (Ctrl+Shift+H) - game thread %s - fps60_hang.txt", w);
    else log_line_try("DIAG-HANGDUMP: no Sg_MainLoop invocation for %d s - game thread %s; %s; loader lock owner %lu, "
                      "log CS owner %lu - report fps60_hang.txt", g_sec, w, l, ldr_owner, log_owner);

    typedef BOOL(WINAPI* md_t)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    static bool dumped = false;                  // one minidump per session (the file is pre-opened)
    if (g_minidump && g_dmp != INVALID_HANDLE_VALUE && !dumped) {
        dumped = true;
        int type = MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory;
        MINIDUMP_CALLBACK_INFORMATION cb = { dump_cb, nullptr };
        if (g_data) type |= MiniDumpWithDataSegs;
        BOOL ok = ((md_t)g_minidump)(GetCurrentProcess(), GetCurrentProcessId(), g_dmp, type, nullptr, nullptr,
                                     g_data ? &cb : nullptr);
        DWORD err = ok ? 0 : GetLastError();
        FlushFileBuffers(g_dmp);
        rep("minidump %s (err %lu)", ok ? "written" : "FAILED", err);
        log_line_try("DIAG-HANGDUMP: minidump fps60_hang.dmp %s (err %lu)", ok ? "written" : "FAILED", err);
    }
    FlushFileBuffers(g_txt);
}

// Ctrl+Shift+H while the game window has focus.
static bool key_down() {
    if (!(GetAsyncKeyState('H') & 0x8000) || !(GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
        !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) return false;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI watchdog(LPVOID) {
    g_watch_tid = GetCurrentThreadId();
    LONG last = -1;
    DWORD still_ms = 0;
    LONG where_1s = 0;
    char l[300] = "";
    bool reported = false, was_paused = false, key_prev = false;
    for (;;) {
        Sleep(250);
        LONG c = g_count;
        if (c == 0 || g_disabled) continue;              // game loop not started yet
        if (g_key && key_down()) {
            if (!key_prev) {
                if (g_txt != INVALID_HANDLE_VALUE) SetFilePointer(g_txt, 0, nullptr, FILE_END);
                report(c, true);
            }
            key_prev = true;
        } else key_prev = false;
        if (c != last) {
            if (still_ms >= 1000 && !reported && !was_paused) {
                char w[200];
                where_text(where_1s, w, sizeof(w));
                log_line_try("DIAG-HANGDUMP: game thread stalled ~%lu ms (recovered) at %s; %s", still_ms, w, l);
            }
            if (was_paused) log_line_try("DIAG-HANGDUMP: pause layer closed after ~%lu ms", still_ms);
            last = c; still_ms = 0; reported = false; was_paused = false;
            continue;
        }
        still_ms += 250;
        if (still_ms == 1000) {
            where_1s = g_where; ldr_text(l, sizeof(l));
        }
        bool p = paused() == 1;                          // the game's own pause: not a stall
        if (p && !was_paused && still_ms >= 1000) {
            was_paused = true;
            log_line_try("DIAG-HANGDUMP: no Sg_MainLoop invocation while the pause layer is up (app+0x3AD) - "
                         "game paused, not reported");
        }
        if (!reported && !p && still_ms >= (DWORD)g_sec * 1000) {
            reported = true;
            if (g_txt != INVALID_HANDLE_VALUE) SetFilePointer(g_txt, 0, nullptr, FILE_END);
            report(c, false);
        }
    }
}

static int g_test = 0;                           // DIAG-HANGDUMP-TEST=1: Ctrl+F6 stalls the game thread to test the report

static void pre() {
    if (!g_game_tid) g_game_tid = GetCurrentThreadId();
    LONG inv = InterlockedIncrement(&g_count);
    if (g_btl_state) {
        static uint8_t prev_bs = 0;
        uint8_t bs = *g_btl_state;
        if (bs != prev_bs) {
            BtlEv& e = g_bs[g_bs_n % 16];
            e.tick = GetTickCount(); e.inv = inv; e.from = prev_bs; e.to = bs;
            g_bs_n = g_bs_n + 1;
            prev_bs = bs;
        }
    }
    if (!g_test) return;
    static bool prev = false;
    bool down = (GetAsyncKeyState(VK_F6) & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
    bool edge = down && !prev;
    prev = down;
    if (!edge) return;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;
    log_line("DIAG-HANGDUMP: TEST - Ctrl+F6, game thread sleeps %d s inside rows_pre", g_sec + 3);
    Sleep((DWORD)(g_sec + 3) * 1000);
    log_line("DIAG-HANGDUMP: TEST - game thread resumed");
}

// The previous session's non-empty fps60_hang.<ext> becomes fps60_hang_<last write time>.<ext>; then a fresh one is opened.
static HANDLE open_rotated(const wchar_t* ext) {
    wchar_t dir[MAX_PATH], path[MAX_PATH], old[MAX_PATH];
    game_dir(dir, MAX_PATH);
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%sfps60_hang.%s", dir, ext);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh)) {
        FILETIME lt; SYSTEMTIME st;
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt);
        FileTimeToSystemTime(&lt, &st);
        _snwprintf_s(old, MAX_PATH, _TRUNCATE, L"%sfps60_hang_%04u%02u%02u-%02u%02u%02u.%s", dir, st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, ext);
        MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
    }
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Loader lock, any thread: plain stores into a static ring.
static void mag_event(uint8_t* base, const char* name, bool load) {
    MagEv& e = g_mag[g_mag_n % 12];
    e.tick = GetTickCount(); e.inv = g_count; e.base = base; e.load = load;
    e.name[0] = 0;
    if (name) strncpy_s(e.name, name, _TRUNCATE);
    else for (LONG i = g_mag_n - 1; i >= 0 && i >= g_mag_n - 12; i--)       // unload: name from its load
        if (g_mag[i % 12].load && g_mag[i % 12].base == base) { strcpy_s(e.name, g_mag[i % 12].name); break; }
    g_mag_n = g_mag_n + 1;
}
static void on_mag_load(uint8_t* base, uint32_t, const char* name) { mag_event(base, name, true); }
static void on_mag_unload(uint8_t* base) { mag_event(base, nullptr, false); }

static void resolve_state() {
    // SKS_WinMainLoop limiter (same site as RATE-001): K at +0x0E, accumulator at +0x16
    if (uint8_t* at = sig_find_unique("DIAG-HANGDUMP limiter (SKS_WinMainLoop)",
            "DD 9D ?? ?? ?? ?? DD 85 ?? ?? ?? ?? DC 0D ?? ?? ?? ?? D9 C1 DC 25 ?? ?? ?? ?? D8 E1 D8 D1 DF E0 F6 C4 41",
            0x62F30C)) {
        g_k = read_abs(at + 0x0E);
        g_accum = read_abs(at + 0x16);
    }
    // SKS_WinMainLoop: MOV EAX,[app]; CMP byte [EAX+0x39],0; JNZ; MOV ECX,EAX; MOV EAX,[EAX+0x268]; CALL EAX
    if (uint8_t* at = sig_find_unique("DIAG-HANGDUMP app pointer (SKS_WinMainLoop)",
            "A1 ?? ?? ?? ?? 80 78 39 00 75 0E 8B C8 8B 80 68 02 00 00 FF D0", 0x62F359))
        g_app_pp = read_abs(at + 1);
    // FUN_0063da80: MOV EAX,[esc player]; TEST; JZ; CMP byte [EAX+0x70],0; JNZ; CMP dword [EAX+0xE4],0; JGE
    if (uint8_t* at = sig_find_unique("DIAG-HANGDUMP esc-menu player FUN_0063da80",
            "A1 ?? ?? ?? ?? 85 C0 74 1E 80 78 70 00 75 0F 83 B8 E4 00 00 00 00 7D 06", 0x63DA80))
        g_esc_pp = read_abs(at + 1);
    // MsBattleProcess teardown test (QOL-RETRY): MOV AL,[a8e0]; CMP AL,0x21; JZ; MOV AL,[a8e3]
    if (uint8_t* at = sig_find_unique("DIAG-HANGDUMP battle state test",
            "A0 ?? ?? ?? ?? 3C 21 74 0D A0 ?? ?? ?? ?? 3C 05 74 04 3C 06 75", 0x790E6C)) {
        g_btl_state = read_abs(at + 1);
        g_btl_result = read_abs(at + 10);
    }
    // End-of-battle timer gate (QOL-RETRY): MOV CL,[c9ec]
    if (uint8_t* at = sig_find_unique("DIAG-HANGDUMP end-of-battle timer",
            "8A 0D ?? ?? ?? ?? A2 ?? ?? ?? ?? 84 C9 74 08 FE C9 88 0D", 0x790D2D))
        g_btl_timer = read_abs(at + 2);
}

static bool install() {
    g_sec = config_row("DIAG-HANGDUMP-SEC", 10);
    if (g_sec < 3) g_sec = 3;
    g_test = config_row("DIAG-HANGDUMP-TEST", 0);
    g_data = config_row("DIAG-HANGDUMP-DATA", 1);
    g_key = config_row("DIAG-HANGDUMP-KEY", 1);
    resolve_state();
    magic_mods_subscribe("DIAG-HANGDUMP", on_mag_load, on_mag_unload);
    g_minidump = proxy_real_minidump();
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    g_ntqsi = nt ? (NtQSI_t)GetProcAddress(nt, "NtQuerySystemInformation") : nullptr;
    g_txt = open_rotated(L"txt");
    g_dmp = open_rotated(L"dmp");
    HANDLE t = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr);
    if (!t) { log_line("  DIAG-HANGDUMP: watchdog thread failed - off"); return false; }
    CloseHandle(t);
    log_line("  DIAG-HANGDUMP: watchdog on - a %d s stall of Sg_MainLoop writes fps60_hang.txt (where/loader breadcrumbs, "
             "lock owners, all thread stacks) then fps60_hang.dmp; stalls >= 1 s that recover are logged "
             "(report file %s, minidump %s%s); game paused (app+0x3AD) is not a stall; %s",
             g_sec, g_txt != INVALID_HANDLE_VALUE ? "open" : "NOT open", g_minidump ? "resolved" : "NOT resolved",
             g_data ? " with FFX.exe data" : "", g_key ? "Ctrl+Shift+H writes a report on request" : "no report key");
    return true;
}

// Dev hot reload: the watchdog idles once g_disabled is set; free the report files for the next copy.
static void stop() {
    HANDLE t = g_txt, d = g_dmp;
    g_txt = g_dmp = INVALID_HANDLE_VALUE;
    if (t != INVALID_HANDLE_VALUE) CloseHandle(t);
    if (d != INVALID_HANDLE_VALUE) CloseHandle(d);
}

Row row_DIAG_HANGDUMP = {
    "DIAG-HANGDUMP",
    "diagnostic: where-breadcrumbs + all-thread stack report + minidump when Sg_MainLoop stalls; observe only",
    1, install, pre, nullptr, 0, stop
};
