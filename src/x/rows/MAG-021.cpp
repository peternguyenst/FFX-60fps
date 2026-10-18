// SPDX-License-Identifier: Apache-2.0
// MAG-021: with MAG-008, family-E DLLs draw parts of their effect only after an update, so those parts blink
// at 30 Hz. Render-only frames snapshot the DLL, replay its update through a shadow service table, let it draw,
// then restore the DLL's memory before the next invocation. Details: docs/fps60/rows/MAG-021.md
#include "../fps60.h"
#include <string.h>
#include <stdio.h>

enum { MAX_MODS = 8, MAX_SECS = 4, MAX_VARS = 8, TABLE_LEN = 0x1000, NSLOT = TABLE_LEN / 4, MAX_NEW = 32 };

struct Sec { uint8_t* p; uint32_t len; uint8_t* buf; };
struct Mod {
    uint8_t*  base;              // null = free slot
    uint32_t  size;
    char      name[24];
    bool      scanned, famE, ok;    // famE: setter + sections found; ok: table pointer(s) found too
    int       var_tries;
    bool      dead;              // unloaded: buffers to free on the game thread
    Sec       sec[MAX_SECS];
    int       nsec;
    uint32_t  total;
    uint32_t* vars[MAX_VARS];    // DLL globals holding the service-table pointer
    int       nvars;
    uint32_t  logic_seen;        // logic invocations since load
    bool      in_window;
};
static Mod g_mods[MAX_MODS];

static uint32_t* g_table = nullptr;          // service table (0x00C64CE8)
static uint32_t* g_shadow = nullptr;
static uint8_t*  g_thunks = nullptr;
static volatile uint32_t g_orig[NSLOT];
static volatile LONG     g_cnt[NSLOT];
static LONG      g_cnt_logged[NSLOT];
static uint8_t   g_cls[NSLOT];               // CLS_*
static uint32_t  g_val_seen[NSLOT];
static uint8_t   g_is_code[NSLOT];
enum { CLS_PASS = 0, CLS_STUB = 1, CLS_EXEC1 = 2, CLS_B28 = 3, CLS_B20 = 4, CLS_STUBM1 = 5, CLS_CREATE = 6 };

// Stateful slots: no-op on render-only (cdecl, plain RET, void or ignored result; S Ghidra FFX.exe).
static const uint16_t STUBS[] = {
    0x358,  // 0x788920 battle signal (scene+0xDFB 5->6)
    0x388,  // 0x788200 chr+0xDFB 4->5
    0x390,  // 0x7882C0 motion request
    0x354,  // 0x787950
    0x848,  // 0x788430 chr+0x402/+0x404
    0x844,  // 0x787C30 chr+0x405, FUN_007ab360
    0x3BC,  // 0x78D980
    0x090,  // 0x837AC0 Ch_SetMotion
    0x094,  // 0x837D00 -> Ch_SetMotion
    0x05C, 0x078, 0x3E4, 0x404,           // 0x82B010/080/380/2A0 actor setters
    0x3FC, 0x868,                         // 0x796150/070 position / chr+0x34
    0x590, 0x594,                         // 0x831390/3C0 sub-object frame
    0x370, 0x624, 0x660,                  // 0x7FFE10/D20/C60 SE command post
    0x4C8,  // 0x875FB0 counter ++
    0xB24,  // 0x711F20 PPP kill (frees EXE pool children)
    0xC2C,  // 0x645280 mesh release
    0x470, 0x474, 0x478, 0x3E8, 0x584,    // camera / light setters (H)
    0x4DC,  // 0x7EB300 -> FUN_007eb4a0: unregister a frame callback (by slot), table 0x20
    0x5E0,  // 0x7EB3A0 -> FUN_007eb4a0: the same, table 0x40 (magic_0070 unregisters its draw here, then signals
            // +0x358: a real unregister in a window lost the signal, Ixion soft-lock)
};
// Returns -1 ("no free slot"): +0x4E0/+0x5E4 register a DLL callback in an EXE table (FUN_007eb500, table
// 0x20/0x40) that would outlive the window.
static const uint16_t STUBS_M1[] = { 0x4E0, 0x5E4 };
// Actor create: +0x000 0x824F90 (cdecl, id) takes a free actor slot (a[2] == 0), builds an actor and returns it.
// The DLL uses it at once (update and draw), so it runs; actors made in a window are released at the restore with
// +0x600 0x8266F0 (cdecl, actor; sets a[2] = 0), as the restored DLL no longer knows them. Before: the orphans
// stayed active with +0x10C pointing into the DLL and crashed after the unload (Anima, magic_0152, 11/10/2026).
enum { MAX_CREATED = 16 };
static uint8_t*  g_created[MAX_CREATED];
static uint16_t  g_created_id[MAX_CREATED];
static int       g_ncreated = 0;

static uint8_t* g_rng_a = nullptr;   // DAT_00c0a000 (u32)
static uint8_t* g_rng_b = nullptr;   // DAT_00c8f728 (u32)
static uint8_t* g_rng_c = nullptr;   // DAT_00c42200 (u32)
static uint32_t g_rng_save[3];

// MsBattleScene_DrawDispatch internals (0x787E00)
typedef int  (__cdecl* f_count_t)(void);
typedef uint8_t* (__cdecl* f_chr_t)(int);
typedef uint8_t* (__cdecl* f_cur_t)(int);
typedef void (__cdecl* ov_fn_t)(int);
static f_count_t g_f_count = nullptr;
static f_chr_t   g_f_chr = nullptr;
static f_cur_t   g_f_cur = nullptr;
static volatile int32_t* g_cur_flag = nullptr;    // DAT_0113335c
static volatile uint8_t* g_st_3364 = nullptr;     // DAT_01133364
static uint8_t* volatile* g_ov_3360 = nullptr;    // DAT_01133360

static bool g_window = false;
static int  g_phase = 0;                  // crash line: 0 no window, 1 window open, 2 in a replayed update, 3 restore pending (frame being submitted)
static bool g_broken = false;            // a replay faulted: no more windows (restores still run)
static void* g_new_recs[MAX_NEW];
static int   g_nnew = 0;
static LONG  g_windows = 0, g_updates = 0, g_skipped_steps = 0, g_released = 0;
static LARGE_INTEGER g_qpf;
static int64_t g_ticks_copy = 0;

// ---------------------------------------------------------------- shadow table slots in C
typedef uint32_t(__cdecl* ppp_step_t)(void*, uint32_t);
static void __cdecl b28_stub(void* rec) {
    InterlockedIncrement(&g_cnt[0xB28 / 4]);
    if (g_nnew < MAX_NEW) g_new_recs[g_nnew++] = rec;
}
static uint32_t __cdecl b20_wrap(void* prog, uint32_t flags) {
    InterlockedIncrement(&g_cnt[0xB20 / 4]);
    for (int i = 0; i < g_nnew; i++) if (g_new_recs[i] == prog) { g_skipped_steps++; return 0; }
    return ((ppp_step_t)(uintptr_t)g_orig[0xB20 / 4])(prog, flags);
}

typedef uint8_t* (__cdecl* create_t)(uint32_t);
typedef void (__cdecl* release_t)(uint8_t*);
static uint8_t* __cdecl create_wrap(uint32_t id) {
    InterlockedIncrement(&g_cnt[0]);              // the only CLS_CREATE slot (+0x000)
    uint8_t* a = ((create_t)(uintptr_t)g_orig[0])(id);
    if (a && g_ncreated < MAX_CREATED) { g_created[g_ncreated] = a; g_created_id[g_ncreated] = *(uint16_t*)a; g_ncreated++; }
    else if (a) log_line("MAG-021: more than %d actors created in one window - actor %p kept", MAX_CREATED, a);
    return a;
}

// Restore time (game thread, between invocations): release what the window created and the DLL did not release.
static void release_created() {
    for (int i = 0; i < g_ncreated; i++) {
        uint8_t* a = g_created[i];
        if (a[2] == 0 || *(uint16_t*)a != g_created_id[i]) continue;
        ((release_t)(uintptr_t)g_orig[0x600 / 4])(a);
        if (g_released++ == 0) log_line("MAG-021: first actor created in a window released at the restore (id 0x%04X)", g_created_id[i]);
    }
    g_ncreated = 0;
}

static uint8_t* thunk_at(int i) { return g_thunks + i * 16; }

static void build_thunks() {
    for (int i = 0; i < NSLOT; i++) {
        uint8_t* t = thunk_at(i);
        t[0] = 0xFF; t[1] = 0x05; *(uint32_t*)(t + 2) = (uint32_t)(uintptr_t)&g_cnt[i];     // inc [cnt]
        if (g_cls[i] == CLS_STUB) {
            t[6] = 0x33; t[7] = 0xC0; t[8] = 0xC3;                                          // xor eax,eax; ret
        } else if (g_cls[i] == CLS_STUBM1) {
            t[6] = 0x83; t[7] = 0xC8; t[8] = 0xFF; t[9] = 0xC3;                             // or eax,-1; ret
        } else if (g_cls[i] == CLS_EXEC1) {
            t[6] = 0xB8; *(uint32_t*)(t + 7) = 1; t[11] = 0xC3;                             // mov eax,1; ret
        } else {
            t[6] = 0xFF; t[7] = 0x25; *(uint32_t*)(t + 8) = (uint32_t)(uintptr_t)&g_orig[i]; // jmp [orig]
        }
    }
    FlushInstructionCache(GetCurrentProcess(), g_thunks, NSLOT * 16);
}

static bool is_code(uint32_t v) {
    if (v < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mb;
    if (!VirtualQuery((void*)(uintptr_t)v, &mb, sizeof(mb)) || mb.State != MEM_COMMIT) return false;
    return (mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static void refresh_shadow() {
    for (int i = 0; i < NSLOT; i++) {
        uint32_t v = g_table[i];
        if (v != g_val_seen[i] || !g_orig[i]) {
            g_val_seen[i] = v;
            g_is_code[i] = is_code(v) ? 1 : 0;
            g_orig[i] = v;
        }
        if (!g_is_code[i])               g_shadow[i] = v;   // data pointer slot: as is
        else if (g_cls[i] == CLS_B28)    g_shadow[i] = (uint32_t)(uintptr_t)&b28_stub;
        else if (g_cls[i] == CLS_B20)    g_shadow[i] = (uint32_t)(uintptr_t)&b20_wrap;
        else if (g_cls[i] == CLS_CREATE) g_shadow[i] = (uint32_t)(uintptr_t)&create_wrap;
        else                             g_shadow[i] = (uint32_t)(uintptr_t)thunk_at(i);
    }
}

// ---------------------------------------------------------------- module tracking
// magic_mods.cpp, loader lock
static void on_load(uint8_t* base, uint32_t size, const char* name) {
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        if (m.base || m.dead) continue;
        memset(&m, 0, sizeof(m));
        m.size = size;
        strncpy(m.name, name, sizeof(m.name) - 1);
        m.base = base;                        // publish last
        return;
    }
}

static void on_unload(uint8_t* base) {
    for (int i = 0; i < MAX_MODS; i++) if (g_mods[i].base == base) {
        g_mods[i].base = nullptr;             // never touch its memory again
        g_mods[i].in_window = false;
        g_mods[i].dead = true;                // buffers freed in pre() (game thread)
    }
}

static void free_mod(Mod& m) {
    for (int k = 0; k < m.nsec; k++) if (m.sec[k].buf) VirtualFree(m.sec[k].buf, 0, MEM_RELEASE);
    memset(&m, 0, sizeof(m));
}

// MAG-021-SKIP (default "0573"): magic DLL numbers never replayed. U 18/10/2026: Braska's Final Aeon grab (magic_0573)
// drew the held character beside the hand; MAG-021=0 fixed it. D fps60.log 21:32:16 (MAG-021-ACTORS test build): the
// replayed update wrote 0 dwords of the actor table, yet the held actor's root still moved on render-only invocations.
// S FUN_00832760 (root/bone build, slot +0x004): actor +0x10C != 0 -> root copied from that matrix; bone +0xC8 != 0 ->
// bone copied from it, then cleared. Both point into the DLL, so render-only frames built the held actor from the
// replayed (one step ahead) DLL matrices; the bone array is EXE heap the .data restore does not cover.
static char g_skip[128] = "";
static bool skipped(const char* name) {
    const char* d = strstr(name, "magic_");
    if (!d || !g_skip[0]) return false;
    char num[8] = {};
    for (int i = 0; i < 4 && d[6 + i] >= '0' && d[6 + i] <= '9'; i++) num[i] = d[6 + i];
    if (strlen(num) != 4) return false;
    for (const char* p = g_skip; (p = strstr(p, num)) != nullptr; p += 4) {
        bool lo = p == g_skip || p[-1] == ',' || p[-1] == ' ';
        bool hi = p[4] == 0 || p[4] == ',' || p[4] == ' ';
        if (lo && hi) return true;
    }
    return false;
}

static void scan(Mod& m) {
    m.scanned = true;
    if (skipped(m.name)) { log_line("MAG-021: %s in MAG-021-SKIP - not replayed", m.name); return; }
    if (famE_find_setter(m.base, m.size, nullptr) != 1) return;   // not family E (this shape)
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)m.base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(m.base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t ch = sec[i].Characteristics;
        if (!(ch & IMAGE_SCN_MEM_WRITE) || (ch & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_SHARED))) continue;
        uint32_t len = (sec[i].Misc.VirtualSize + 3) & ~3u;
        if (!len || sec[i].VirtualAddress + len > m.size) continue;
        if (m.nsec >= MAX_SECS) { log_line("MAG-021: %s has more than %d writable sections - not handled", m.name, MAX_SECS); return; }
        Sec& s = m.sec[m.nsec++];
        s.p = m.base + sec[i].VirtualAddress;
        s.len = len;
        s.buf = (uint8_t*)VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!s.buf) { log_line("MAG-021: %s no memory for 0x%X bytes - not handled", m.name, len); return; }
        m.total += len;
    }
    if (!m.nsec) { log_line("MAG-021: %s family E but no writable section - not handled", m.name); return; }
    m.famE = true;
}

// The DLL's init stores the table pointer after the load notification: retried at every logic invocation.
static void find_vars(Mod& m) {
    for (int k = 0; k < m.nsec; k++)
        for (uint32_t* p = (uint32_t*)m.sec[k].p; p < (uint32_t*)(m.sec[k].p + m.sec[k].len); p++)
            if (*p == (uint32_t)(uintptr_t)g_table) {
                if (m.nvars >= MAX_VARS) {
                    log_line("MAG-021: %s has more than %d table pointers - not handled", m.name, MAX_VARS);
                    m.famE = false;
                    return;
                }
                m.vars[m.nvars++] = p;
            }
    if (!m.nvars) {
        if (++m.var_tries == 300) log_line("MAG-021: %s no service-table pointer after 300 logic ticks - not handled", m.name);
        if (m.var_tries >= 300) m.famE = false;
        return;
    }
    m.ok = true;
    char vs[96]; int k = 0;
    for (int i = 0; i < m.nvars && k < 80; i++)
        k += _snprintf_s(vs + k, sizeof(vs) - k, _TRUNCATE, " 0x%08X", 0x10000000u + (uint32_t)((uint8_t*)m.vars[i] - m.base));
    log_line("MAG-021: %s family E: %d writable section(s), %u KB snapshot, table pointer var(s) gva%s",
             m.name, m.nsec, m.total / 1024, vs);
}

static bool owns(const void* p) {
    for (int i = 0; i < MAX_MODS; i++) {
        const Mod& m = g_mods[i];
        if (m.base && m.in_window && (const uint8_t*)p >= m.base && (const uint8_t*)p < m.base + m.size) return true;
    }
    return false;
}

// ---------------------------------------------------------------- update replay (MAG-008 hook)
static void run_update(uint8_t* ov) {
    if (!ov || !owns(ov)) return;
    ov_fn_t fn = *(ov_fn_t*)(ov + 0xC);
    if (!owns((void*)fn)) return;
    if (g_f_cur(0) == ov) *g_cur_flag = 1;
    g_phase = 2;
    fn(0);
    g_phase = 1;
    *g_cur_flag = 0;
    g_updates++;
}

static void build_hook() {
    if (!g_window || g_broken || g_disabled) return;
    __try {
        if (g_f_count()) {
            for (int i = 0; i < 0x1F; i++) {
                uint8_t* chr = g_f_chr(i);
                uint8_t st = chr[0xDFB];
                if (st == 4 || st == 5) run_update(*(uint8_t**)(chr + 0xF8C));
            }
        }
        uint8_t st = *g_st_3364;
        if (st == 4 || st == 5) run_update(*g_ov_3360);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *g_cur_flag = 0;
        log_line("MAG-021: fault 0x%08lX in the render-only update - row off", GetExceptionCode());
        g_broken = true;                 // this window is still restored by post()
    }
}

// ---------------------------------------------------------------- install
static bool resolve_rng() {
    // RNG roll (slot +0x4E8): seeds from stream b if it is 0, then reads stream a
    static const char RNG[] = "55 8B EC 51 83 3D ?? ?? ?? ?? 00 75 ?? ?? ?? ?? ?? ?? ?? ?? A1";
    uint8_t* f = (uint8_t*)(uintptr_t)g_table[0x4E8 / 4];
    if (!sig_match(f, RNG)) {
        log_line("  MAG-021: slot +0x4E8 target %p has an unexpected shape - refused", f);
        return false;
    }
    g_rng_b = read_abs(f + 6);
    g_rng_a = read_abs(f + 21);
    uint8_t* g = (uint8_t*)(uintptr_t)g_table[0x650 / 4];
    // Slot +0x650: the same roll shape, which must use the same two streams
    if (!sig_match(g, "55 8B EC 51 83 3D ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? A1") ||
        read_abs(g + 6) != g_rng_b || read_abs(g + 21) != g_rng_a) {
        log_line("  MAG-021: slot +0x650 does not use the +0x4E8 RNG - refused");
        return false;
    }
    uint8_t* h = (uint8_t*)(uintptr_t)g_table[0x4CC / 4];
    // Slot +0x4CC: a short wrapper that calls the third RNG (stream c, multiplied by 0x5D588B65)
    uint8_t* r = sig_match(h, "55 8B EC 51 E8") ? call_target(h + 4) : nullptr;
    if (!sig_match(r, "8B 0D ?? ?? ?? ?? 69 C9 65 8B 58 5D")) {
        log_line("  MAG-021: slot +0x4CC RNG has an unexpected shape - refused");
        return false;
    }
    g_rng_c = read_abs(r + 2);
    log_line("  MAG-021: RNG streams gva 0x%08X 0x%08X (+0x4E8/+0x650), 0x%08X (+0x4CC) saved per window",
             rt_to_gva_exe(g_rng_a), rt_to_gva_exe(g_rng_b), rt_to_gva_exe(g_rng_c));
    return true;
}

static bool resolve_dispatch() {
    uint8_t* d = (uint8_t*)mag008_dispatch();
    if (!d || !row_MAG008.on) { log_line("  MAG-021: needs MAG-008 (its skipped dispatch is the hook point) - refused"); return false; }
    // MsBattleScene_DrawDispatch landmarks: count call, chr call, the chr state/overlay fields, current-overlay
    // call, the "current" flag set to 1 then 0, the state byte, the overlay load.
    bool ok = sig_match(d + 0x40, "E8 ?? ?? ?? ?? 85 C0") && sig_match(d + 0x51, "E8") &&
              sig_match(d + 0x56, "8A 88 FB 0D 00 00") &&                // chr state byte +0xDFB
              sig_match(d + 0x69, "8B B0 8C 0F 00 00") &&                // chr overlay +0xF8C
              sig_match(d + 0x7F, "E8") && sig_match(d + 0x8B, "C7 05") && sig_match(d + 0xAE, "C7 05") &&
              read_abs(d + 0xB0) == read_abs(d + 0x8D) &&                 // same "current" flag both times
              sig_match(d + 0xBF, "A0") && sig_match(d + 0xCC, "8B 35") &&  // load the state byte, the overlay
              call_target(d + 0xE2) && call_target(d + 0xE2) == call_target(d + 0x7F);
    if (!ok) { log_line("  MAG-021: MsBattleScene_DrawDispatch %p has an unexpected shape - refused", d); return false; }
    g_f_count  = (f_count_t)call_target(d + 0x40);
    g_f_chr    = (f_chr_t)call_target(d + 0x51);
    g_f_cur    = (f_cur_t)call_target(d + 0x7F);
    g_cur_flag = (volatile int32_t*)read_abs(d + 0x8D);
    g_st_3364  = (volatile uint8_t*)read_abs(d + 0xC0);
    g_ov_3360  = (uint8_t* volatile*)read_abs(d + 0xCE);
    log_line("  MAG-021: dispatch internals: count gva 0x%08X, chr gva 0x%08X, current gva 0x%08X, flag gva 0x%08X",
             rt_to_gva_exe((void*)g_f_count), rt_to_gva_exe((void*)g_f_chr), rt_to_gva_exe((void*)g_f_cur),
             rt_to_gva_exe((const void*)g_cur_flag));
    return true;
}

static bool install() {
    // MagicFile_Start: pushes the service table address for the overlay
    uint8_t* at = sig_find_unique("MAG-021 MagicFile_Start table push",
        "0F 94 C0 50 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? FF 15 ?? ?? ?? ?? FF 35", 0x9DA816);
    if (!at) return false;
    g_table = (uint32_t*)read_abs(at + 0x0A);
    if (!resolve_dispatch() || !resolve_rng()) return false;
    for (size_t i = 0; i < sizeof(STUBS) / sizeof(STUBS[0]); i++) g_cls[STUBS[i] / 4] = CLS_STUB;
    for (size_t i = 0; i < sizeof(STUBS_M1) / sizeof(STUBS_M1[0]); i++) g_cls[STUBS_M1[i] / 4] = CLS_STUBM1;
    g_cls[0x000 / 4] = CLS_CREATE;
    g_cls[0x3F4 / 4] = CLS_EXEC1;
    g_cls[0xB28 / 4] = CLS_B28;
    g_cls[0xB20 / 4] = CLS_B20;
    g_shadow = (uint32_t*)VirtualAlloc(nullptr, TABLE_LEN, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_thunks = (uint8_t*)exec_alloc(NSLOT * 16);
    if (!g_shadow || !g_thunks) return false;
    build_thunks();
    config_str("rows", "MAG-021-SKIP", g_skip, sizeof(g_skip), "0573");
    log_line("  MAG-021: not replayed (MAG-021-SKIP): %s", g_skip[0] ? g_skip : "none");
    QueryPerformanceFrequency(&g_qpf);
    if (!magic_mods_subscribe("MAG-021", on_load, on_unload)) return false;
    g_mag008_build_hook = build_hook;
    log_line("  MAG-021: service table gva=0x%08X; %u stubbed slots; watching family-E loads",
             rt_to_gva_exe(g_table), (unsigned)(sizeof(STUBS) / sizeof(STUBS[0])));
    return true;
}

// ---------------------------------------------------------------- per invocation
static void restore_pending();
static void pre() {
    restore_pending();                           // previous render-only window (see post())
    g_window = false;
    g_phase = 0;
    g_nnew = 0;
    bool build = pacing_kind() == INV_BUILD;
    bool any = false;
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        m.in_window = false;
        if (m.dead) { free_mod(m); continue; }
        if (!m.base) continue;
        __try {
            if (!m.scanned) scan(m);
            if (!m.famE) continue;
            if (!build) {
                if (!m.ok) find_vars(m);
                m.logic_seen++;
                continue;
            }
            if (!m.ok) continue;
            if (m.logic_seen < 2 || g_disabled || g_broken) continue;
            any = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            log_line("MAG-021: fault 0x%08lX scanning %s - module dropped", GetExceptionCode(), m.name);
            m.ok = m.famE = false;
        }
    }
    if (!any) return;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    refresh_shadow();
    g_rng_save[0] = *(uint32_t*)g_rng_a;
    g_rng_save[1] = *(uint32_t*)g_rng_b;
    g_rng_save[2] = *(uint32_t*)g_rng_c;
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        if (!m.base || !m.ok || m.logic_seen < 2) continue;
        bool ok = true;
        for (int k = 0; k < m.nsec && ok; k++) ok = safe_copy(m.sec[k].buf, m.sec[k].p, m.sec[k].len);
        if (!ok) { log_line("MAG-021: %s snapshot faulted - module dropped", m.name); m.ok = false; continue; }
        for (int v = 0; v < m.nvars; v++) *m.vars[v] = (uint32_t)(uintptr_t)g_shadow;
        m.in_window = true;
        g_window = true;
    }
    QueryPerformanceCounter(&t1);
    g_ticks_copy += t1.QuadPart - t0.QuadPart;
    if (g_window) { g_windows++; g_phase = 1; }
}

// Restored at the start of the next invocation: the frame's draw commands still point into DLL memory and
// run after Sg_MainLoop returns.
static bool g_restore_pending = false;

static void restore_pending() {
    if (!g_restore_pending) return;
    g_restore_pending = false;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < MAX_MODS; i++) {
        Mod& m = g_mods[i];
        if (!m.in_window) continue;
        m.in_window = false;
        if (!m.base) continue;                    // unloaded since the window
        bool ok = true;
        for (int k = 0; k < m.nsec; k++) ok = safe_copy(m.sec[k].p, m.sec[k].buf, m.sec[k].len) && ok;
        if (!ok) { on_fault("MAG-021 restore", 0); return; }
    }
    if (g_ncreated) {
        __try { release_created(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_ncreated = 0; log_line("MAG-021: fault 0x%08lX releasing window actors - row off", GetExceptionCode()); g_broken = true; }
    }
    g_phase = 0;
    QueryPerformanceCounter(&t1);
    g_ticks_copy += t1.QuadPart - t0.QuadPart;
}

static void post() {
    if (!g_window) return;
    *(uint32_t*)g_rng_a = g_rng_save[0];
    *(uint32_t*)g_rng_b = g_rng_save[1];
    *(uint32_t*)g_rng_c = g_rng_save[2];
    g_window = false;
    g_restore_pending = true;
    g_phase = 3;

    // first use of each slot inside a window (census for the stub list)
    for (int i = 0; i < NSLOT; i++) {
        if (g_cnt[i] && !g_cnt_logged[i]) {
            g_cnt_logged[i] = 1;
            const char* what = g_cls[i] == CLS_STUB ? "no-op" : g_cls[i] == CLS_STUBM1 ? "returns -1" : g_cls[i] == CLS_EXEC1 ? "returns 1" :
                               g_cls[i] == CLS_B28 ? "no-op, rec noted" : g_cls[i] == CLS_CREATE ? "passed, actor released at the restore" : g_cls[i] == CLS_B20 ? "step (noted recs skipped)" : "passed";
            log_line("MAG-021: render-only window: first call of slot +0x%03X (gva 0x%08X) - %s",
                     i * 4, rt_to_gva_exe((void*)(uintptr_t)g_orig[i]), what);
        }
    }
    if (g_windows == 1 || g_windows % 1800 == 0) {
        log_line("MAG-021: %ld windows, %ld DLL updates replayed, %ld fresh-record steps skipped, %ld actors released, copy avg %.0f us",
                 g_windows, g_updates, g_skipped_steps, g_released,
                 g_qpf.QuadPart ? (double)g_ticks_copy * 1e6 / (double)g_qpf.QuadPart / (double)g_windows : 0.0);
    }
}

// DIAG-DRAWSTEP: true while [base, base+size) is a module in the current window (replayed on purpose).
bool mag021_in_window(const void* base) {
    for (int i = 0; i < MAX_MODS; i++) if (g_mods[i].base == base && g_mods[i].in_window) return true;
    return false;
}

const char* mag021_state() {
    static char buf[160];
    int k = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "MAG-021 phase %d window %d:", g_phase, g_window ? 1 : 0);
    for (int i = 0; i < MAX_MODS && k > 0 && k < (int)sizeof(buf) - 30; i++)
        if (g_mods[i].base) k += _snprintf_s(buf + k, sizeof(buf) - k, _TRUNCATE, " %s%s", g_mods[i].name, g_mods[i].in_window ? "*" : "");
    return buf;
}

Row row_MAG021 = {
    "MAG-021",
    "family-E magic DLLs: render-only frames replay the DLL update + draw unpaused, then restore its .data (blink)",
    1, install, pre, post, 0
};
