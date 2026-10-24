// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-PPPOPS (diagnostic, off, observe only): which particle draw ops run while particles are paused.
//
// Each particle draw op (the pppDraw* / pppKe* functions) is hooked and counted with the particle pause flag set
// (render-only steps, X2-MAG-006) and clear; the ops that ran are logged every 10 s. A blinking effect's op tells
// which pause test needs X2-FX-010-DRAWOPS's treatment. The ops are found in the game's particle-op table (0x28-byte
// records: name, group / calc / draw op, ...), starting from the record named "pppDrawShapeCamera". Every hook calls
// the original. Also: the step notes and the UNPAUSE-OPS test X2-MAG-006 offers (pppops_prog_marked).
// Evidence: re/ledger/DRAWCAM.md.
#include "../x2.h"
#include <string.h>
#include <stdio.h>

namespace x2 {

extern Row row_DIAGPPPOPS;

enum { MAXOPS = 128 };
typedef void(__cdecl* op_t)(void*, void*, void*);    // (particle, keydata, opdata), cdecl, plain RET (DRAWCAM D3 shape)
static op_t        g_orig[MAXOPS];
static uint8_t*    g_fn[MAXOPS];
static const char* g_name[MAXOPS];
static int         g_n = 0;
static volatile int32_t* g_pause = nullptr;
static volatile LONG g_paused[MAXOPS], g_live[MAXOPS], g_last_paused[MAXOPS], g_last_live[MAXOPS];

// X2-DIAG-PPPOPS-CALLERS (default "pppKeShpTail3X pppKeLnsFls"): for these ops, distinct call chains (5 return
// addresses as gva) with counts and the pause state, logged with the 10 s counts (U 17/10/2026 Full Throttle glow:
// these ops are never reached on paused steps; which dispatcher calls them).
static bool g_trace[MAXOPS];
struct Chain { uint32_t op; uint32_t f[9]; int paused; LONG n; };
enum { MAXCH = 32 };
static Chain g_ch[MAXCH];
static int g_nch = 0;
static void note_chain(int i) {
    void* fr[10] = {};
    USHORT k = RtlCaptureStackBackTrace(2, 9, fr, nullptr);
    Chain c = {};
    c.op = (uint32_t)i;
    c.paused = *g_pause ? 1 : 0;
    for (int j = 0; j < k && j < 9; j++) c.f[j] = in_image(fr[j]) ? rt_to_gva(fr[j]) : (uint32_t)(uintptr_t)fr[j];
    for (int j = 0; j < g_nch; j++)
        if (g_ch[j].op == c.op && g_ch[j].paused == c.paused && !memcmp(g_ch[j].f, c.f, sizeof(c.f))) { g_ch[j].n++; return; }
    if (g_nch < MAXCH) { c.n = 1; g_ch[g_nch++] = c; }
}
// Per (program, caller class, op) counts by INVOCATION KIND (logic / render-only), not by the pause flag. The program
// and caller class come from X2-MAG-006's Ppp_ProgramStep hook (the pointer the step was called with; cls 1 DLL caller,
// 2 EXE caller, 3 field step, 0 = op called outside any step, e.g. deferred callback lists). Per program also: steps,
// particles on +0x30 before / after the step, step ends, per kind. Logged every 10 s for programs whose render-only side
// is below 80 % of the logic side (steps, particles or an op). U 17/10/2026: Full Throttle (magic_0681) light-ball glow
// blinks, F11 (DLL steps unpaused) fixes it; which 0681 program draws less on render-only invocations.
enum { HOP = 4096, HST = 1024 };
struct OpC { void* prog; int op; int cls; LONG n[2]; LONG np; };          // np: render-only calls with the flag set
struct StC { void* prog; int cls; LONG n[2]; LONG pb[2]; LONG pa[2]; LONG end[2]; LONG pz[2]; };  // pz: steps with 0 after
static OpC g_oc[HOP];
static StC g_sc[HST];
static bool g_full = false;
static inline uint32_t hmix(uint32_t h) { h ^= h >> 16; h *= 0x7FEB352D; h ^= h >> 15; return h; }
static OpC* op_slot(void* prog, int op, int cls) {
    uint32_t h = hmix((uint32_t)(uintptr_t)prog ^ (uint32_t)(op * 0x9E3779B1) ^ (uint32_t)cls << 28);
    for (int k = 0; k < 64; k++) {
        OpC& e = g_oc[(h + k) & (HOP - 1)];
        if (e.n[0] == 0 && e.n[1] == 0 && !e.prog && !e.op && !e.cls) { e.prog = prog; e.op = op; e.cls = cls; return &e; }
        if (e.prog == prog && e.op == op && e.cls == cls) return &e;
    }
    g_full = true;
    return nullptr;
}
static StC* st_slot(void* prog, int cls) {
    uint32_t h = hmix((uint32_t)(uintptr_t)prog ^ (uint32_t)cls << 28);
    for (int k = 0; k < 64; k++) {
        StC& e = g_sc[(h + k) & (HST - 1)];
        if (!e.prog) { e.prog = prog; e.cls = cls; return &e; }
        if (e.prog == prog && e.cls == cls) return &e;
    }
    g_full = true;
    return nullptr;
}
static void note_pp(int i) {
    int cls = 0;
    void* pr = mag006_cur_step(&cls);
    OpC* e = op_slot(pr, i + 1, cls);                       // op index + 1: slot 0 marks empty
    if (!e) return;
    int k = render_only() ? 1 : 0;
    e->n[k]++;
    if (k && *g_pause) e->np++;
}
static void module_of(void* p, char* mod, size_t cb, uint32_t* off) {
    strncpy_s(mod, cb, "?", _TRUNCATE); *off = 0;
    HMODULE hm = nullptr;
    if (p && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &hm) && hm) {
        char path[MAX_PATH]; GetModuleFileNameA(hm, path, MAX_PATH);
        const char* b = strrchr(path, 0x5C); strncpy_s(mod, cb, b ? b + 1 : path, _TRUNCATE);
        *off = (uint32_t)((uint8_t*)p - (uint8_t*)hm);
    }
}
// X2-DIAG-PPPOPS-TRACE (default "pppKeShpTail2X"): one line per step of a DLL-called program that ever called a listed
// op: invocation kind, particles before / after, the change since its previous step ended (particles made outside its
// steps), return value, and every op called in that step with its count (U 17/10/2026: magic_0681 slot 0 calls
// pppKeShpTail2X 75 times on logic and 50 on render-only steps and gains ~87 particles between steps). Max 4000 lines.
static bool  g_tr_op[MAXOPS];
static LONG  g_snap[4][MAXOPS];
static int   g_depth = 0;
struct TrP { void* prog; int last; };
static TrP   g_trp[16];
static int   g_ntrp = 0;
static LONG  g_trlines = 0;
static LONG  g_inv = 0;
static void step_trace(void* prog, int cls, int before, int after, int ret, const LONG* snap) {
    if (cls != 1) return;
    TrP* t = nullptr;
    for (int i = 0; i < g_ntrp; i++) if (g_trp[i].prog == prog) t = &g_trp[i];
    if (!t) {
        bool hit = false;
        for (int i = 0; i < g_n && !hit; i++) if (g_tr_op[i] && g_paused[i] + g_live[i] != snap[i]) hit = true;
        if (!hit || g_ntrp >= 16) return;
        t = &g_trp[g_ntrp++];
        t->prog = prog; t->last = before;
    }
    if (g_trlines >= 4000) return;
    g_trlines++;
    char buf[1024]; int o = 0;
    for (int i = 0; i < g_n && o < 900; i++) {
        LONG d = g_paused[i] + g_live[i] - snap[i];
        if (d) o += _snprintf_s(buf + o, sizeof(buf) - o, _TRUNCATE, " %s %ld", g_name[i] + 3, d);
    }
    buf[o] = 0;
    char mod[64]; uint32_t off;
    module_of(prog, mod, sizeof(mod), &off);
    log_line("PPPTRACE #%ld %s %s+0x%X parts %d -> %d (gap %+d) ret %d |%s", g_inv, render_only() ? "R" : "L", mod, off,
             before, after, before - t->last, ret, buf);
    t->last = after;
}
// X2-MAG-006-UNPAUSE-OPS (test, default ""): programs whose last step called one of these ops are marked; X2-MAG-006 then
// steps them unpaused on render-only invocations (F11 for those programs only). Which program blinks (U 17/10/2026:
// Full Throttle later-phase glow; the only glow ops then are pppKeLnsLp / pppKeLnsArnd).
static bool  g_mark_op[MAXOPS];
static bool  g_mark_any = false;
enum { HMK = 512 };
static void* g_marked[HMK];
static void mark_prog(void* prog) {
    uint32_t h = hmix((uint32_t)(uintptr_t)prog);
    for (int k = 0; k < 32; k++) {
        void*& e = g_marked[(h + k) & (HMK - 1)];
        if (e == prog) return;
        if (!e) { e = prog; return; }
    }
}
bool pppops_prog_marked(void* prog) {
    if (!g_mark_any) return false;
    uint32_t h = hmix((uint32_t)(uintptr_t)prog);
    for (int k = 0; k < 32; k++) {
        void* e = g_marked[(h + k) & (HMK - 1)];
        if (e == prog) return true;
        if (!e) return false;
    }
    return false;
}
static void step_note(void* prog, int cls, int before, int after, int ret) {
    if (after < 0) {
        if (g_depth < 4) for (int i = 0; i < g_n; i++) g_snap[g_depth][i] = g_paused[i] + g_live[i];
        g_depth++;
        return;
    }
    g_depth--;
    if (g_depth >= 0 && g_depth < 4) {
        if (g_mark_any && cls == 1)
            for (int i = 0; i < g_n; i++)
                if (g_mark_op[i] && g_paused[i] + g_live[i] != g_snap[g_depth][i]) { mark_prog(prog); break; }
        step_trace(prog, cls, before, after, ret, g_snap[g_depth]);
    }
    if (g_depth < 0) g_depth = 0;
    StC* e = st_slot(prog, cls);
    if (!e) return;
    int k = render_only() ? 1 : 0;
    e->n[k]++; e->pb[k] += before; e->pa[k] += after;
    if (ret) e->end[k]++;
    if (!after) e->pz[k]++;
}
static bool low(LONG r, LONG l) { return l >= 20 && r * 5 < l * 4; }
static const char* CLS[4] = { "none", "DLL", "EXE", "field" };
static void report_pp() {
    for (int j = 0; j < HST; j++) {
        StC& s = g_sc[j];
        if (!s.prog) continue;
        bool any = low(s.n[1], s.n[0]) || (s.n[0] >= 20 && s.n[1] && low(s.pa[1] * s.n[0] / s.n[1], s.pa[0]));
        for (int i = 0; i < HOP && !any; i++)
            if (g_oc[i].prog == s.prog && g_oc[i].cls == s.cls && low(g_oc[i].n[1], g_oc[i].n[0])) any = true;
        if (!any) continue;
        char mod[64]; uint32_t off;
        module_of(s.prog, mod, sizeof(mod), &off);
        log_line("PPPOPS prog %p (%s+0x%X) %s: steps L %ld R %ld | parts before L %ld R %ld after L %ld R %ld | empty-after L %ld R %ld | ends L %ld R %ld",
                 s.prog, mod, off, CLS[s.cls & 3], s.n[0], s.n[1], s.pb[0], s.pb[1], s.pa[0], s.pa[1], s.pz[0], s.pz[1], s.end[0], s.end[1]);
        for (int i = 0; i < HOP; i++) {
            OpC& o = g_oc[i];
            if (o.prog == s.prog && o.cls == s.cls && (o.n[0] || o.n[1]))
                log_line("PPPOPS   op %-24s L %ld R %ld (R flag set %ld)%s", g_name[o.op - 1], o.n[0], o.n[1], o.np,
                         low(o.n[1], o.n[0]) ? "  <-- low on render-only" : "");
        }
    }
    for (int i = 0; i < HOP; i++) {                          // ops outside any step
        OpC& o = g_oc[i];
        if (o.op && !o.prog && low(o.n[1], o.n[0]))
            log_line("PPPOPS no-step op %-24s L %ld R %ld  <-- low on render-only", g_name[o.op - 1], o.n[0], o.n[1]);
    }
    if (g_full) log_line("PPPOPS: per-program table full, some programs not counted");
    memset(g_oc, 0, sizeof(g_oc));
    memset(g_sc, 0, sizeof(g_sc));
    g_full = false;
}
// X2-DIAG-PPPOPS-DUMP (default ""): ops whose calls are dumped (kind, flag, program, particle, age, keydata, op data
// block 0xB0 bytes, program +0x50..+0xDF) when the running step's program lies in a module whose name contains
// X2-DIAG-PPPOPS-DUMP-MOD (default "magic_0681"). Max 1200 lines. U 17/10/2026: Full Throttle later glow stops blinking
// when only the pppKeLnsLp / pppKeLnsArnd program steps unpaused on render-only; what differs at draw time.
static bool  g_dump_op[MAXOPS];
static char  g_dump_mod[64] = "";
static LONG  g_dump_lines = 0;
static void hexs(char* o, size_t cb, const uint8_t* p, int n) {
    static const char hx[] = "0123456789ABCDEF";
    size_t k = 0;
    __try {
        for (int i = 0; i < n && k + 3 < cb; i++) {
            if (i && !(i & 3)) o[k++] = ' ';
            o[k++] = hx[p[i] >> 4]; o[k++] = hx[p[i] & 15];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    o[k] = 0;
}
// Compact mode (X2-DIAG-PPPOPS-DUMP-MODE=1, default): per invocation one line per dumped op: calls, particles at age 0,
// min / max age, and calls whose data dword at +0xA8 (KeLnsLp size) / +0x0 (KeLnsArnd texture) is 0 or the null record.
struct Sum { LONG n, age0, amin, amax, z8f, za8, z0, z4; float i_sum, i_min, i_max; LONG i_zero; };
static Sum   g_sum[2][MAXOPS];
static int   g_dump_mode = 1;
static uint8_t* g_null_rec = nullptr;             // 0x18E49B0, the empty texture record (FUN_00855830 test)
static void sum_call(int i, uint8_t* part, uint8_t*, uint8_t* opd) {
    int cls = 0;
    uint8_t* prog = (uint8_t*)mag006_cur_step(&cls);
    if (!prog) return;
    char mod[64]; uint32_t off;
    module_of(prog, mod, sizeof(mod), &off);
    if (!strstr(mod, g_dump_mod)) return;
    Sum& s = g_sum[render_only() ? 1 : 0][i];
    __try {
        uint8_t* d = part + 0xA0 + **(int**)(opd + 0xC);
        int age = *(int*)(part + 0xC);
        if (!s.n || age < s.amin) s.amin = age;
        if (!s.n || age > s.amax) s.amax = age;
        s.n++;
        if (!age) s.age0++;
        if (!d[0x8F]) s.z8f++;
        if (!*(int*)(d + 0xA8)) s.za8++;
        if (!*(void**)d || *(uint8_t**)d == g_null_rec) s.z0++;
        if (!*(int*)(d + 4)) s.z4++;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// after the op ran: KeLnsLp intensity d+0x7C (FUN_00856910 writes it; KeLnsArnd draws with it through FUN_00855830)
static void sum_after(int i, uint8_t* part, uint8_t* opd) {
    int cls = 0;
    uint8_t* prog = (uint8_t*)mag006_cur_step(&cls);
    if (!prog) return;
    char mod[64]; uint32_t off;
    module_of(prog, mod, sizeof(mod), &off);
    if (!strstr(mod, g_dump_mod)) return;
    Sum& s = g_sum[render_only() ? 1 : 0][i];
    __try {
        float v = *(float*)(part + 0xA0 + **(int**)(opd + 0xC) + 0x7C);
        if (s.n == 1 || v < s.i_min) s.i_min = v;
        if (s.n == 1 || v > s.i_max) s.i_max = v;
        s.i_sum += v;
        if (v == 0.0f) s.i_zero++;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// HD VFX instance calls inside FUN_00855830 (KeLnsArnd's submit): lookup FUN_004563d0 (0x855A46, 0 = no instance),
// submit FUN_00456a00 (0x855A72), create FUN_007fdde0 (0x855AFB). Counted per invocation kind for dumped programs
// (U 17/10/2026: the lens particle data and intensity are the same on logic and render-only frames, it still blinks).
typedef int(__cdecl* look_t)(uint32_t, int);
typedef void(__cdecl* sub_t)(uint32_t, int);
typedef void(__cdecl* cre_t)(uint32_t, int, void*, int, uint32_t, uint32_t);
static look_t g_look = nullptr; static sub_t g_sub = nullptr; static cre_t g_cre = nullptr;
static uint8_t* g_vfx_site[3] = {};
static LONG g_vfx[2][4];                          // lookups, lookups 0, submits, creates
static bool in_dump_prog() {
    int cls = 0;
    uint8_t* prog = (uint8_t*)mag006_cur_step(&cls);
    if (!prog) return false;
    char mod[64]; uint32_t off;
    module_of(prog, mod, sizeof(mod), &off);
    return strstr(mod, g_dump_mod) != nullptr;
}
static int __cdecl h_look(uint32_t a, int b) {
    int r = g_look(a, b);
    if (in_dump_prog()) { int k = render_only() ? 1 : 0; g_vfx[k][0]++; if (!r) g_vfx[k][1]++; }
    return r;
}
static void __cdecl h_sub(uint32_t a, int b) {
    if (in_dump_prog()) g_vfx[render_only() ? 1 : 0][2]++;
    g_sub(a, b);
}
static void __cdecl h_cre(uint32_t a, int b, void* c, int d, uint32_t e, uint32_t f) {
    if (in_dump_prog()) g_vfx[render_only() ? 1 : 0][3]++;
    g_cre(a, b, c, d, e, f);
}
static void sum_flush() {
    if (g_dump_lines >= 3000) return;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < g_n; i++) {
            Sum& s = g_sum[k][i];
            if (!s.n) continue;
            g_dump_lines++;
            if (!strcmp(g_name[i], "pppKeLnsArnd"))
                log_line("PPPVFX #%ld %s lookups %ld missing %ld submits %ld creates %ld", g_inv, k ? "R" : "L",
                         g_vfx[k][0], g_vfx[k][1], g_vfx[k][2], g_vfx[k][3]);
            log_line("PPPSUM #%ld %s %-12s n %ld age0 %ld age %X..%X | 8F=0 %ld A8=0 %ld d0=0/null %ld d4=0 %ld | 7C after sum %.3f min %.3f max %.3f zero %ld",
                     g_inv, k ? "R" : "L", g_name[i] + 3, s.n, s.age0, s.amin, s.amax, s.z8f, s.za8, s.z0, s.z4,
                     (double)s.i_sum, (double)s.i_min, (double)s.i_max, s.i_zero);
            s = Sum{};
        }
    memset(g_vfx, 0, sizeof(g_vfx));
}
static void dump_call(int i, uint8_t* part, uint8_t* key, uint8_t* opd) {
    if (g_dump_mode == 1) { sum_call(i, part, key, opd); return; }
    if (g_dump_lines >= 1200) return;
    int cls = 0;
    uint8_t* prog = (uint8_t*)mag006_cur_step(&cls);
    if (!prog) return;
    char mod[64]; uint32_t off;
    module_of(prog, mod, sizeof(mod), &off);
    if (!strstr(mod, g_dump_mod)) return;
    g_dump_lines++;
    int doff = 0, age = 0, kt = 0;
    __try { doff = **(int**)(opd + 0xC); age = *(int*)(part + 0xC); kt = *(int*)key; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    static char d[512], pr[512];
    hexs(d, sizeof(d), part + 0xA0 + doff, 0xB0);
    hexs(pr, sizeof(pr), prog + 0x50, 0x90);
    log_line("PPPDUMP #%ld %s f%d %s+0x%X %s part %p age %X key %X | data %s | prog50 %s", g_inv, render_only() ? "R" : "L",
             *g_pause != 0, mod, off, g_name[i] + 3, part, age, kt, d, pr);
}
// X2-DIAG-PPPOPS-HIDE (test, default ""): these draw ops are not called when the running step's program lies in the
// X2-DIAG-PPPOPS-DUMP-MOD module (user 23/10/2026: Ultima flare halos brighter on logic frames, frame dump 03:40; hide
// the lens ops and dump again to see whether the halos are theirs).
static bool g_hide_op[MAXOPS];
template <int I> static void __cdecl h_op(void* a, void* b, void* c) {
    if (g_dump_op[I]) dump_call(I, (uint8_t*)a, (uint8_t*)b, (uint8_t*)c);
    InterlockedIncrement(*g_pause ? &g_paused[I] : &g_live[I]);
    note_pp(I);
    if (g_trace[I]) note_chain(I);
    if (g_hide_op[I] && in_dump_prog()) return;
    g_orig[I](a, b, c);
    if (g_dump_op[I] && g_dump_mode == 1) sum_after(I, (uint8_t*)a, (uint8_t*)c);
}
template <int... I> struct Tbl { static constexpr void* v[sizeof...(I)] = { (void*)&h_op<I>... }; };
template <int N, int... I> struct Make : Make<N - 1, N - 1, I...> {};
template <int... I> struct Make<0, I...> { using type = Tbl<I...>; };
static void* const* const DETOURS = Make<MAXOPS>::type::v;

static bool image(uint8_t** lo, uint32_t* size) {
    uint8_t* b = exe_base();
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    *lo = b;
    *size = nt->OptionalHeader.SizeOfImage;
    return true;
}

static bool is_name(uint8_t* lo, uint32_t size, uint32_t p) {
    if (p < (uint32_t)lo || p - (uint32_t)lo > size - 4) return false;
    const char* s = (const char*)p;
    for (int i = 0; i < 3; i++) if (s[i] < 0x21 || s[i] > 0x7E) return false;
    return true;
}

// Only ops inside an executable section are hooked (one table record points its draw slot into .data: "STOP").
static bool in_code(const uint8_t* p) {
    uint8_t* b = exe_base();
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const uint8_t* lo = b + sec[i].VirtualAddress;
        if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) && p >= lo && p < lo + sec[i].Misc.VirtualSize) return true;
    }
    return false;
}

static bool install() {
    uint8_t* lo; uint32_t size;
    image(&lo, &size);
    static const char key[] = "pppDrawShapeCamera";
    uint8_t* str = nullptr;
    for (uint8_t* a = lo; a + sizeof(key) <= lo + size; a++)
        if (a[0] == 'p' && !memcmp(a, key, sizeof(key))) { str = a; break; }     // sizeof includes the NUL
    if (!str) { log_line("  X2-DIAG-PPPOPS: op name not found - off"); return false; }
    uint8_t* rec = nullptr;
    for (uint8_t* a = lo; a + 4 <= lo + size; a += 4)
        if (*(uint32_t*)a == (uint32_t)str) { rec = a; break; }
    if (!rec) { log_line("  X2-DIAG-PPPOPS: op record not found - off"); return false; }
    while (rec - 0x28 >= lo && is_name(lo, size, *(uint32_t*)(rec - 0x28))) rec -= 0x28;
    int recs = 0;
    for (; rec + 0x28 <= lo + size && is_name(lo, size, *(uint32_t*)rec); rec += 0x28, recs++) {
        uint8_t* fn = *(uint8_t**)(rec + 0xC);
        if (!fn || !in_code(fn)) continue;
        bool seen = false;
        for (int i = 0; i < g_n; i++) if (g_fn[i] == fn) { seen = true; break; }
        if (seen || g_n >= MAXOPS) continue;
        g_fn[g_n] = fn;
        g_name[g_n] = *(const char**)rec;
        g_n++;
    }
    // pause flag: the CMP operand at pppDrawShapeCamera +0x224 (DRAWCAM D2)
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_name[i], key) && sig_match(g_fn[i] + 0x224, "83 3D ?? ?? ?? ?? 00 75"))
            g_pause = (volatile int32_t*)abs_operand("particle pause flag", g_fn[i] + 0x226);
    if (!g_pause) { log_line("  X2-DIAG-PPPOPS: pause flag not found - off"); return false; }
    {
        char tq[256];
        config_str("rows", "X2-DIAG-PPPOPS-TRACE", tq, sizeof(tq), "pppKeShpTail2X");
        for (int i = 0; i < g_n; i++) {
            const char* hit = strstr(tq, g_name[i]);
            size_t L = strlen(g_name[i]);
            while (hit && ((hit > tq && hit[-1] != ' ' && hit[-1] != ',') || (hit[L] && hit[L] != ' ' && hit[L] != ','))) hit = strstr(hit + 1, g_name[i]);
            g_tr_op[i] = hit != nullptr;
        }
        config_str("rows", "X2-DIAG-PPPOPS-DUMP", tq, sizeof(tq), "");
        config_str("rows", "X2-DIAG-PPPOPS-DUMP-MOD", g_dump_mod, sizeof(g_dump_mod), "magic_0681");
        g_dump_mode = config_row("X2-DIAG-PPPOPS-DUMP-MODE", 1);
        g_null_rec = exe_base() + (0x18E49B0 - 0x400000);   // compared only, never dereferenced
        for (int i = 0; i < g_n; i++) {
            const char* hit = strstr(tq, g_name[i]);
            size_t L = strlen(g_name[i]);
            while (hit && ((hit > tq && hit[-1] != ' ' && hit[-1] != ',') || (hit[L] && hit[L] != ' ' && hit[L] != ','))) hit = strstr(hit + 1, g_name[i]);
            g_dump_op[i] = hit != nullptr;
        }
        config_str("rows", "X2-DIAG-PPPOPS-HIDE", tq, sizeof(tq), "");
        for (int i = 0; i < g_n; i++) {
            const char* hit = strstr(tq, g_name[i]);
            size_t L = strlen(g_name[i]);
            while (hit && ((hit > tq && hit[-1] != ' ' && hit[-1] != ',') || (hit[L] && hit[L] != ' ' && hit[L] != ','))) hit = strstr(hit + 1, g_name[i]);
            g_hide_op[i] = hit != nullptr;
            if (hit) log_line("  X2-DIAG-PPPOPS-HIDE: %s not drawn for %s programs", g_name[i], g_dump_mod);
        }
        config_str("rows", "X2-MAG-006-UNPAUSE-OPS", tq, sizeof(tq), "");
        for (int i = 0; i < g_n; i++) {
            const char* hit = strstr(tq, g_name[i]);
            size_t L = strlen(g_name[i]);
            while (hit && ((hit > tq && hit[-1] != ' ' && hit[-1] != ',') || (hit[L] && hit[L] != ' ' && hit[L] != ','))) hit = strstr(hit + 1, g_name[i]);
            g_mark_op[i] = hit != nullptr;
            if (hit) { g_mark_any = true; log_line("  X2-MAG-006-UNPAUSE-OPS: programs calling %s step unpaused on render-only", g_name[i]); }
        }
    }
    // VFX sites: pppKeLnsArnd draw +0xCE = CALL FUN_00855830; its CALLs at +0x216 / +0x242 / +0x2CB
    for (int i = 0; i < g_n; i++) {
        if (strcmp(g_name[i], "pppKeLnsArnd") || !g_dump_op[i]) continue;
        uint8_t* sub = (g_fn[i][0xCE] == 0xE8) ? call_target(g_fn[i] + 0xCE) : nullptr;
        if (!sub || !in_image(sub) || !sig_match(sub, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04")) { log_line("  X2-DIAG-PPPOPS: FUN_00855830 not found - VFX counts off"); break; }
        static const int offs[3] = { 0x216, 0x242, 0x2CB };
        bool ok = true;
        for (int k = 0; k < 3; k++) { g_vfx_site[k] = sub + offs[k]; if (g_vfx_site[k][0] != 0xE8 || !in_image(call_target(g_vfx_site[k]))) ok = false; }
        // callee shapes (prologues read from FFX-2.exe 0x4563D0 / 0x456A00 / 0x7FDDE0)
        if (ok && (!sig_match(call_target(g_vfx_site[0]), "55 8B EC 8B 4D 08 8B 55 0C 8B C1 0B C2 74") ||
                   !sig_match(call_target(g_vfx_site[1]), "55 8B EC 83 3D ?? ?? ?? ?? 00 0F 85") ||
                   !sig_match(call_target(g_vfx_site[2]), "55 8B EC 83 EC 08 53 8B 5D 0C 57 FF 75 1C 8B 7D 08"))) ok = false;
        if (!ok) { log_line("  X2-DIAG-PPPOPS: FUN_00855830 CALL sites not as proven - VFX counts off"); g_vfx_site[0] = nullptr; break; }
        g_look = (look_t)call_target(g_vfx_site[0]); g_sub = (sub_t)call_target(g_vfx_site[1]); g_cre = (cre_t)call_target(g_vfx_site[2]);
        log_line("  X2-DIAG-PPPOPS: FUN_00855830 gva=0x%08X, lookup 0x%08X submit 0x%08X create 0x%08X (expected 0x004563D0 / 0x00456A00 / 0x007FDDE0)",
                 rt_to_gva(sub), rt_to_gva((void*)g_look), rt_to_gva((void*)g_sub), rt_to_gva((void*)g_cre));
        break;
    }
    char tr[256];
    config_str("rows", "X2-DIAG-PPPOPS-CALLERS", tr, sizeof(tr), "pppKeShpTail3X pppKeLnsFls");
    for (int i = 0; i < g_n; i++) {
        const char* hit = strstr(tr, g_name[i]);
        size_t L = strlen(g_name[i]);
        while (hit && ((hit > tr && hit[-1] != ' ' && hit[-1] != ',') || (hit[L] && hit[L] != ' ' && hit[L] != ','))) hit = strstr(hit + 1, g_name[i]);
        g_trace[i] = hit != nullptr;
    }
    log_line("  X2-DIAG-PPPOPS: %d records, %d distinct draw ops; pause flag gva=0x%08X; hooks at the first pre()", recs, g_n,
             rt_to_gva((const void*)g_pause));
    return g_n > 0;
}

static void pre() {
    static bool done = false;
    if (done) return;
    done = true;
    int hooked = 0;                              // game thread, between invocations: no op is running
    for (int i = 0; i < g_n; i++)
        if (hook_entry("X2-DIAG-PPPOPS", g_fn[i], DETOURS[i], (void**)&g_orig[i])) hooked++;
        else log_line("X2-DIAG-PPPOPS: %s gva=0x%08X not hooked", g_name[i], rt_to_gva(g_fn[i]));
    log_line("X2-DIAG-PPPOPS: %d of %d draw ops hooked", hooked, g_n);
    if (g_vfx_site[0]) {
        bool ok = patch_call_rel32(g_vfx_site[0], (void*)g_look, (void*)&h_look) &&
                  patch_call_rel32(g_vfx_site[1], (void*)g_sub, (void*)&h_sub) &&
                  patch_call_rel32(g_vfx_site[2], (void*)g_cre, (void*)&h_cre);
        log_line("X2-DIAG-PPPOPS: FUN_00855830 VFX call sites %s", ok ? "patched (counting)" : "patch failed");
    }
    mag006_set_step_note(&step_note);
}

static void post() {
    if (g_dump_mode == 1) sum_flush();
    g_inv++;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 10000) return;
    last = now;
    for (int i = 0; i < g_n; i++) {
        LONG p = g_paused[i] - g_last_paused[i], l = g_live[i] - g_last_live[i];
        if (!p && !l) continue;
        g_last_paused[i] = g_paused[i];
        g_last_live[i] = g_live[i];
        log_line("X2-DIAG-PPPOPS: %-30s gva=0x%08X  paused %ld  live %ld (10 s)", g_name[i], rt_to_gva(g_fn[i]), p, l);
    }
    report_pp();
    for (int j = 0; j < g_nch; j++) {
        Chain& c = g_ch[j];
        if (!c.n) continue;
        log_line("X2-DIAG-PPPOPS caller: %s %s x%ld <- %08X <- %08X <- %08X <- %08X <- %08X <- %08X <- %08X <- %08X <- %08X",
                 g_name[c.op], c.paused ? "PAUSED" : "live", c.n, c.f[0], c.f[1], c.f[2], c.f[3], c.f[4], c.f[5], c.f[6], c.f[7], c.f[8]);
        c.n = 0;
    }
}

Row row_DIAGPPPOPS = {
    "X2-DIAG-PPPOPS",
    "diagnostic: per particle draw op, calls with the pause flag set / clear (which op blinks)",
    0, install, pre, post, 0
};

} // namespace x2
