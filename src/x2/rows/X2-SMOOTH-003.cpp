// SPDX-License-Identifier: Apache-2.0
// X2-SMOOTH-003 (port of FFX SMOOTH-003, minimal first build): with X2-SMOOTH-002 the root moves at 60 Hz but the
// skeleton pose still changes at 30 Hz. This row turns on the engine's half-rate motion switch and takes a second half
// step of each actor's motion on render-only invocations, so limbs animate at 60 Hz at stock speed (needs X2-SMOOTH=1;
// F7 toggles). Sub-switches as FFX: GATES, HOLD, LOOPGUARD, LOOPEND, SHORTLOOP, ENDEXTRAP (default 1), CLOTHHOLD (0 or 2,
// default 2).
// Ported since: ENDEXTRAP(-ONCE), CLOTHHOLD 3/4, SANITY and STOCK-ID(-END) (16/10). Not ported: KEYHOLD, BLEND, CLOTH, DIAG*/TRACE.
// FFX-2 changes (PORT-SMOOTH3.md §1-§7): step CALL pushes EDI (actor), the tween loop after it is inlined (cross-check
// windows sa+0x280); the walk writes no skeleton dirty flag (FFX-2 has none) and dispatches at >= 150 bones (FFX > 199);
// Ch_NoSkelPass call site has a new shape (E8 at +0x13); actor stride 0x910 with per-block field shifts (low block +0xC,
// motion block +0x10, 0x7F0+ block +0x14); hold regions re-mapped, the look-at block held whole (+0x418..+0x43B, H).
// All CALL retargets and the cloth job entry hook are written on the game thread at the first pre().
// X2-SMOOTH-003-PEEL (default 1): during a dressphere change this row runs as if off (stock motion, no half step), as
// F6 (SMOOTH-003 alone off) did: with the half step the outfit peel's two models came apart (U 18/10/2026, F6 test
// 17:18; holding X2-SMOOTH-002 over the whole change did not help, re/ledger/XFADE-BLINK.md). The change is found by
// its middle DLL loading (X2-SMOOTH-003-PEEL-DLL, default "936 937 938") or by the OPU op 0x116 silhouette copy
// (FUN_0085b3d0, DLL op table = magic service +0xC24, entry +0x58); the hold lasts until that DLL and the next one
// (loaded within 120 invocations) unload, cap 1800 invocations per phase.
// Evidence: outer repo G:\Work\ffx2-fps60 re/ledger/PORT-SMOOTH3.md (S unless tagged H), sigs tools/sigs_x2_smooth3.txt.
#include "x2_smooth.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

namespace x2 {

extern Row row_SMOOTH003;

typedef void(__cdecl* step_t)(uint8_t* actor, int full);
typedef void(__cdecl* actor_fn_t)(uint8_t* actor);
typedef void(__cdecl* void_fn_t)(void);
typedef void(__cdecl* dispatch_t)(uint8_t* start, int mode, int span);
typedef void(__cdecl* cjob_t)(uint8_t** list, int* count);

static const int      MAXA = 512;
// Actor fields (PORT-SMOOTH3.md §5, all S).
static const uint32_t STRIDE   = 0x910;   // FFX 0x880
static const uint32_t ACTIVE   = 0x2;     // unchanged
static const uint32_t RANGE_A  = 0x18F;   // FFX 0x180: range-skip byte
static const uint32_t RANGE_B  = 0x190;   // FFX 0x181
static const uint32_t FLAGS    = 0x1A0;   // FFX 0x194 (bits 0x40, 0x1000000 unchanged)
static const uint32_t PARENT   = 0x1A8;   // FFX 0x19C
static const uint32_t HDR      = 0x1CC;   // FFX 0x1C0 (bone count u16 at +0xA)
static const uint32_t HDR_BONES = 0xA;    // unchanged
// Keyframe clock fields (set by the motion start, wrapped by Ch_MotionLoopWrap, read by the sampler).
static const uint32_t M_SEQ    = 0x724;   // FFX 0x714
static const uint32_t M_START  = 0x728;   // FFX 0x718
static const uint32_t M_END    = 0x72C;   // FFX 0x71C
static const uint32_t M_LEN    = 0x730;   // FFX 0x720
static const uint32_t M_LOOPS  = 0x734;   // FFX 0x724 (s16)
static const uint32_t M_CLOCK  = 0x750;   // FFX 0x740
static const int      BATCH_BONES = 0x96; // FFX-2 dispatches at >= 150 bones (FFX > 199)
static const uint32_t F_SCRIPT = 0x40, F_STEP = 0x1000000;

static step_t     g_step = nullptr;
static actor_fn_t g_seq = nullptr, g_motev = nullptr;
static void_fn_t  g_noskel = nullptr, g_sync = nullptr;
static dispatch_t g_dispatch = nullptr;
static volatile int32_t*  g_half = nullptr;     // engine half-rate motion switch 0x0116E3D4
static volatile int8_t*   g_freeze = nullptr;   // actor freeze flag 0x00DF67F3
static uint8_t* volatile* g_table = nullptr;    // pointer to the actor array 0x01CC9604
static volatile int32_t*  g_count = nullptr;    // actor count 0x01CC9600

static uint8_t* g_site_step = nullptr, *g_site_noskel = nullptr, *g_site_seq = nullptr, *g_site_motev = nullptr;
static bool g_patched = false;

static int  g_gates = 1, g_hold = 1, g_loopguard = 1, g_shortloop = 1, g_loopend = 1, g_endextrap = 1, g_endextrap_once = 1;
static int  g_kind = INV_STOCK;
static bool g_half_on = false;                   // we wrote 1 into the switch
static volatile bool g_in_step = false;          // inside one of OUR Ch_StepMotion calls
static volatile bool g_walked_inv = false;

static uint8_t* g_debt_base = nullptr;
static uint8_t  g_debt[MAXA];
static uint16_t g_debt_id[MAXA];
static volatile uint8_t g_walked[MAXA];

// Hold buffer: per slot, common regions (all walked / range actors) then the face regions (parents).
struct Region { uint32_t off, len; };
static constexpr Region COMMON[] = {
    { 0x15C, 4 },      // tilt ease, unchanged (S)
    { 0x180, 8 },      // new store in FUN_006e3970 (H)
    { 0x520, 0x1C4 },  // reference points, FFX 0x510 (start S, end +0x6E3 H)
};
static constexpr Region FACE[] = {
    { 0x418, 0x24 },   // look-at block re-laid out, held whole +0x418..+0x43B (H; FFX {0x418,8})
    { 0x4CC, 4 },      // FFX 0x4BC (H)
    { 0x4E0, 4 },      // FFX 0x4D0 (S)
    { 0x4EC, 8 },      // mouth/blink, FFX 0x4DC (S)
};
template <size_t N> static constexpr uint32_t region_len(const Region (&r)[N]) {
    uint32_t s = 0;
    for (size_t i = 0; i < N; i++) s += r[i].len;
    return s;
}
static constexpr uint32_t HOLD_SZ = region_len(COMMON) + region_len(FACE);
static_assert(HOLD_SZ == 0x204, "hold layout");
static uint8_t* g_hold_buf = nullptr;            // MAXA * HOLD_SZ
static uint8_t  g_held[MAXA];                    // 0 = none, 1 = common, 2 = common + face
static uint8_t* g_hold_base = nullptr;
static int      g_hold_n = 0;

static LONG g_loopend_holds = 0, g_stock_steps = 0, g_guard_wrap = 0, g_guard_clamp = 0, g_guard_hold = 0;
static LONG g_walks = 0, g_stepped = 0, g_pays = 0, g_batches = 0, g_gated_seq = 0, g_gated_motev = 0;

// CLOTHHOLD: the cloth job Ch_ClothJob 0x6DB500 (cdecl(actor**, int* count), worker threads) runs for walked actors on
// render-only invocations too, so hair and cloth would swing 2x. Mode 2: skipped on render-only invocations (cloth 30 Hz,
// one tick behind the 60 Hz body: U 13/10/2026 "ghost character when walking left/right", "maybe cloth is not
// smooth"). Mode 4 (default, port of FFX's default): after each logic job the mesh is redeformed at the midpoint of
// the last two job results, and a render-only invocation deforms the latest one, so cloth trails the body by half a
// tick at 60 Hz, like everything else. Mode 3: extrapolation (FFX: overshoots). FFX-2 offsets (PORT-SMOOTH3.md, S):
// mesh entries +0x1C8 (stride 0x38, desc +0 (vertex count u16 at +4), vertex array +0x08, +0x0C/+0x2C/+0x34 as FFX),
// model header +0x1CC (entry count u16 at +6), reset counter +0x824, reset flag bit 0x800000 at +0x1A0, deform gate
// CMP [0xDF6440],0 at job +0x27 and deform Ch_ClothDeform 0x6DAE00 at job +0x31.
static int      g_clothhold = 4;
static uint8_t* g_cjob_fn = nullptr;
static cjob_t   g_cjob_tramp = nullptr;
static volatile LONG g_cloth_held = 0;

static void fault(const char* where, DWORD code) {
    log_line("X2-SMOOTH-003: fault 0x%08lX in %s - row off", code, where);
    row_SMOOTH003.on = 0;
}

static int slot_of(const uint8_t* a, uint8_t* base, int n) {
    if (!base || a < base) return -1;
    uintptr_t d = (uintptr_t)(a - base);
    if (d % STRIDE) return -1;
    int i = (int)(d / STRIDE);
    return (i < n && i < MAXA) ? i : -1;
}

// ---------------------------------------------------------------- hold (save / restore)
static void hold_save(int i, uint8_t* a, bool face) {
    uint8_t* p = g_hold_buf + i * HOLD_SZ;
    for (const Region& r : COMMON) { memcpy(p, a + r.off, r.len); p += r.len; }
    if (face) for (const Region& r : FACE) { memcpy(p, a + r.off, r.len); p += r.len; }
    g_held[i] = face ? 2 : 1;
}

static void hold_restore() {
    if (!g_hold_base) return;
    if (*g_table == g_hold_base) {
        int n = *g_count;
        if (n > g_hold_n) n = g_hold_n;
        for (int i = 0; i < n; i++) {
            if (!g_held[i]) continue;
            uint8_t* a = g_hold_base + i * STRIDE;
            uint8_t* p = g_hold_buf + i * HOLD_SZ;
            for (const Region& r : COMMON) { memcpy(a + r.off, p, r.len); p += r.len; }
            if (g_held[i] == 2) for (const Region& r : FACE) { memcpy(a + r.off, p, r.len); p += r.len; }
        }
    }
    memset(g_held, 0, sizeof(g_held));
    g_hold_base = nullptr;
}

// ---------------------------------------------------------------- our calls into game code (SEH)
static bool safe_step(uint8_t* a, int full) {
    g_in_step = true;
    bool ok = true;
    __try { g_step(a, full); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; fault("Ch_StepMotion", GetExceptionCode()); }
    g_in_step = false;
    return ok;
}

static bool safe_dispatch(uint8_t* start, int span) {
    __try { g_dispatch(start, 0, span); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("skeleton dispatch", GetExceptionCode()); return false; }
    InterlockedIncrement(&g_batches);
    return true;
}

static bool safe_sync() {
    __try { g_sync(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("job sync", GetExceptionCode()); return false; }
    return true;
}

// ---------------------------------------------------------------- end extrapolation (X2-SMOOTH-003-ENDEXTRAP)
// Port of FFX SMOOTH-003-ENDEXTRAP (U 11/10/2026 FFX: Tidus run/idle loop-end stutter). LOOPEND holds a loop's last
// frame for one render-only frame, which repeats a pose once per loop cycle (U 13/10/2026 FFX-2: "character walk feel
// stuttery"; D fps60x2.log 23:46: 55 LOOPEND holds per minute). Instead each bone rotation continues half a tick past
// tick k from its last two logic values: k + (k - (k-1)) / 2. A channel that moved more than ENDX_MAX in the last
// tick (flip, cut) keeps tick k. Play-once clamps are not extrapolated (FFX ENDEXTRAP-ONCE default 0: the next motion
// is unknown). FFX-2 layout (PORT-SMOOTH3.md, S): bones ptr +0x334, bone stride 0x160, angles +0xE4/+0xE8/+0xEC,
// mirror +0x30.
static const uint32_t BONES = 0x334, BONE_STRIDE = 0x160;
static const uint32_t BANG[3] = { 0xE4, 0xE8, 0xEC };
static const int      MAXB = 1024;
static const float    INV_2PI = 0.15915494f, ENDX_MAX = 0.08f;
static float*   g_prev_ang = nullptr;            // MAXA * MAXB * 3, in turns: tick k-1
static float*   g_cur_ang = nullptr;             // tick k
static uint16_t g_ang_id[MAXA];
static uint8_t  g_ang_n[MAXA];
static volatile LONG g_endx = 0;

static int bone_count(const uint8_t* a) {
    const uint8_t* hdr = *(uint8_t* const*)(a + HDR);
    if (!hdr) return 0;
    int n = *(const uint16_t*)(hdr + HDR_BONES);
    return (n > 0 && n <= MAXB) ? n : 0;
}

// After a logic step: tick k-1 <- tick k <- this pose.
static void record_angles(int i, const uint8_t* a) {
    if (!g_cur_ang || i < 0 || i >= MAXA) return;
    int nb = bone_count(a);
    const uint8_t* bones = *(uint8_t* const*)(a + BONES);
    if (!nb || !bones) { g_ang_n[i] = 0; return; }
    uint16_t id = *(const uint16_t*)a;
    if (g_ang_id[i] != id) { g_ang_n[i] = 0; g_ang_id[i] = id; }
    float* prev = g_prev_ang + (size_t)i * MAXB * 3;
    float* cur = g_cur_ang + (size_t)i * MAXB * 3;
    memcpy(prev, cur, sizeof(float) * nb * 3);
    for (int b = 0; b < nb; b++)
        for (int c = 0; c < 3; c++) cur[b * 3 + c] = *(const float*)(bones + b * BONE_STRIDE + BANG[c]) * INV_2PI;
    if (g_ang_n[i] < 2) g_ang_n[i]++;
}

static void end_extrapolate(int i, uint8_t* a) {
    if (!g_cur_ang || i < 0 || i >= MAXA || g_ang_n[i] < 2 || g_ang_id[i] != *(const uint16_t*)a) return;
    int nb = bone_count(a);
    uint8_t* bones = *(uint8_t**)(a + BONES);
    if (!nb || !bones) return;
    const float* prev = g_prev_ang + (size_t)i * MAXB * 3;
    const float* cur = g_cur_ang + (size_t)i * MAXB * 3;
    for (int b = 0; b < nb; b++)
        for (int c = 0; c < 3; c++) {
            float d = cur[b * 3 + c] - prev[b * 3 + c];
            d -= (float)(int)d;                          // shortest signed difference in turns
            if (d > 0.5f) d -= 1.0f;
            if (d < -0.5f) d += 1.0f;
            if (d > ENDX_MAX || d < -ENDX_MAX) continue;
            float v = (cur[b * 3 + c] + 0.5f * d) * 6.2831853f;
            *(float*)(bones + b * BONE_STRIDE + BANG[c]) = v;
            *(float*)(bones + b * BONE_STRIDE + BANG[c] + 0x30) = v;
        }
    if (InterlockedIncrement(&g_endx) == 1)
        log_line("X2-SMOOTH-003: first loop-end half-tick extrapolation (ENDEXTRAP), id 0x%04X", *(const uint16_t*)a);
}

// ---------------------------------------------------------------- pose sanity (X2-SMOOTH-003-SANITY)
// Port of FFX SMOOTH-003-SANITY (U 10/10/2026 FFX: twisted enemies; U 16/10/2026 FFX-2: one monster's limbs twist).
// Keyframe pairs far apart (e.g. ~180 deg) that stock never samples between give a wrong half-frame sample. Per bone
// channel: a render-only angle that moved more than max(0.06 turn, 3x its last tick delta + 0.01) from tick k keeps
// the tick-k value. Same bone layout as ENDEXTRAP (PORT-SMOOTH3.md, S).
static int      g_sanity = 1;
static uint8_t  g_bone_save[MAXB * BONE_STRIDE];
static volatile LONG g_sanity_reverts = 0, g_sanity_lines = 0;

static inline float turn_diff(float x, float y) {
    float d = x - y;
    d -= (float)(int)d;                                   // fractional part, sign kept
    if (d > 0.5f) d -= 1.0f;
    if (d < -0.5f) d += 1.0f;
    return d < 0 ? -d : d;
}

// Before the render-only step: save the bones. Returns the bone count (0 = cannot check).
static int sanity_save(int i, const uint8_t* a) {
    if (!g_sanity || !g_cur_ang || i < 0 || i >= MAXA) return 0;
    int nb = bone_count(a);
    const uint8_t* bones = *(uint8_t* const*)(a + BONES);
    if (!nb || !bones) return 0;
    memcpy(g_bone_save, bones, (size_t)nb * BONE_STRIDE);
    return nb;
}

static void sanity_check(int i, uint8_t* a, int nb, int32_t c0) {
    if (!nb || g_ang_n[i] < 2 || g_ang_id[i] != *(const uint16_t*)a) return;
    uint8_t* bones = *(uint8_t**)(a + BONES);
    if (!bones) return;
    const float* prev = g_prev_ang + (size_t)i * MAXB * 3;
    const float* cur = g_cur_ang + (size_t)i * MAXB * 3;
    for (int b = 0; b < nb; b++) {
        for (int c = 0; c < 3; c++) {
            const uint8_t* kb = g_bone_save + b * BONE_STRIDE;
            uint8_t* hb = bones + b * BONE_STRIDE;
            float vk = *(const float*)(kb + BANG[c]) * INV_2PI;
            float vh = *(const float*)(hb + BANG[c]) * INV_2PI;
            float dh = turn_diff(vh, vk), dt = turn_diff(cur[b * 3 + c], prev[b * 3 + c]);
            if (dh <= 0.06f || dh <= 3.0f * dt + 0.01f) continue;   // NaN falls through to the restore
            *(float*)(hb + BANG[c]) = *(const float*)(kb + BANG[c]);
            *(float*)(hb + BANG[c] + 0x30) = *(const float*)(kb + BANG[c] + 0x30);
            InterlockedIncrement(&g_sanity_reverts);
            if (g_sanity_lines < 40) {
                InterlockedIncrement(&g_sanity_lines);
                log_line("X2-SMOOTH-003 sanity: slot=%d id=0x%04X bone %d ch %d: k-1 %.4f k %.4f half %.4f (d %.4f vs tick %.4f) "
                         "clock %d start=%d end=%d len=%d loops=%d fl=%08X - channel held",
                         i, *(const uint16_t*)a, b, c, prev[b * 3 + c], vk, vh, dh, dt, c0, *(int32_t*)(a + M_START),
                         *(int32_t*)(a + M_END), *(int32_t*)(a + M_LEN), (int)*(int16_t*)(a + M_LOOPS), *(uint32_t*)(a + FLAGS));
            }
        }
    }
}

// ---------------------------------------------------------------- key hold (X2-SMOOTH-003-KEYHOLD)
// Port of FFX SMOOTH-003-KEYHOLD (FFX ledger SMOOTH-003-KEYDECODE.md). U 18/10/2026: Rikku's legs alternate between two
// poses on consecutive frames in a cutscene (map 0xB3); D (FREEZEDIAG trace 05:49:37.65): 16 bones swing exactly
// 0.785 rad between every logic and render-only frame for 4 frames at a motion start. The half-frame sample lerps each
// rotation channel between the stream keys prev (+0x18) and value (+0x16) (shortest path, masked with res-1); cutscene
// data can store one rotation as two Euler forms (or near gimbal lock) in consecutive keys, which stock never samples
// between: the lerp is a wrong pose. A bone whose keys differ by more than KEYHOLD-D (scaled to the motion's key
// resolution) on any rotation channel keeps the pre-step pose: nodes +0x1C/+0x20 and bone +0xE4..+0xEC / +0x114..+0x11C.
// Stream cursors are untouched (the next logic decode is unchanged). FFX-2 layout (outer repo re/ledger/KEYHOLD-X2.md,
// S): pose [a+0x758] = 9 nodes of 0x2C per entry (stride 0x18C), rotation nodes have byte +5 set, linked list head
// [a+0x75C] (read fresh: 0x6E89D0 relocates it), key resolution res = [a+0x7F8] (0x1000..0x8000, one turn), sampled
// clock +0x754, pose entry -> bone remap [a+0x768] (count u16 +2, bone u16 at +8+2c, 0xFFFF none; null = identity).
static const uint32_t P_POSE = 0x758, P_LIST = 0x75C, P_SCLK = 0x754, P_REMAP = 0x768, P_RES = 0x7F8, NODE = 0x2C, ENTRY = 0x18C;
static int      g_keyhold = 1, g_keyhold_d = 0x300;
static uint8_t  g_kh_pose[MAXB * ENTRY];
static uint8_t  g_kh_bones[MAXB * BONE_STRIDE];
static uint8_t  g_kh_mark[MAXB];
static volatile LONG g_keyholds = 0, g_keyhold_lines = 0;

// Before the render-only step: save pose entries and bones. Returns the entry count (0 = no check).
static int keyhold_save(const uint8_t* a) {
    if (!g_keyhold) return 0;
    int nb = bone_count(a);
    const uint8_t* pose = *(uint8_t* const*)(a + P_POSE);
    const uint8_t* bones = *(uint8_t* const*)(a + BONES);
    if (!nb || !pose || !bones) return 0;
    memcpy(g_kh_pose, pose, (size_t)nb * ENTRY);
    memcpy(g_kh_bones, bones, (size_t)nb * BONE_STRIDE);
    return nb;
}

static void keyhold_apply(int i, uint8_t* a, int nb) {
    if ((*(const int32_t*)(a + P_SCLK) & 0xFF) == 0) return;          // whole frame: no lerp between keys
    int32_t res = *(const int32_t*)(a + P_RES);
    if (res != 0x1000 && res != 0x2000 && res != 0x4000 && res != 0x8000) return;
    uint8_t* pose = *(uint8_t**)(a + P_POSE);
    uint8_t* bones = *(uint8_t**)(a + BONES);
    if (!pose || !bones) return;
    int mask = res - 1, half = res / 2, thr = g_keyhold_d * (res / 0x1000);
    memset(g_kh_mark, 0, (size_t)nb);
    int marked = 0, guard = nb * 9;
    for (uint8_t* n = *(uint8_t**)(a + P_LIST); n && guard-- > 0; n = *(uint8_t**)n) {
        if (n < pose || n >= pose + (size_t)nb * ENTRY || (n - pose) % NODE) break;   // not a node of this pose: stop
        if (!n[5]) continue;                                                       // linear channel
        int d = (int)(uint16_t)(*(const int16_t*)(n + 0x16) - *(const int16_t*)(n + 0x18)) & mask;
        if (d > half) d = res - d;
        if (d <= thr) continue;
        int e = (int)((n - pose) / ENTRY);
        if (!g_kh_mark[e]) { g_kh_mark[e] = 1; marked++; }
    }
    if (!marked) return;
    const uint8_t* remap = *(const uint8_t* const*)(a + P_REMAP);
    int rcount = remap ? *(const uint16_t*)(remap + 2) : 0;
    for (int e = 0; e < nb; e++) {
        if (!g_kh_mark[e]) continue;
        for (int c = 0; c < 3; c++) {                                  // rotation channels: output +0x1C, current +0x20
            uint8_t* dst = pose + (size_t)e * ENTRY + c * NODE;
            const uint8_t* src = g_kh_pose + (size_t)e * ENTRY + c * NODE;
            memcpy(dst + 0x1C, src + 0x1C, 8);
        }
        int b = e;
        if (remap) { if (e >= rcount) continue; b = *(const uint16_t*)(remap + 8 + 2 * e); if (b == 0xFFFF || b >= nb) continue; }
        uint8_t* hb = bones + (size_t)b * BONE_STRIDE;
        const uint8_t* kb = g_kh_bones + (size_t)b * BONE_STRIDE;
        memcpy(hb + 0xE4, kb + 0xE4, 12);
        memcpy(hb + 0x114, kb + 0x114, 12);
        LONG c = InterlockedIncrement(&g_keyholds);
        if (g_keyhold_lines < 30) {
            InterlockedIncrement(&g_keyhold_lines);
            log_line("X2-SMOOTH-003-KEYHOLD: slot %d id 0x%04X pose entry %d bone %d held (keys > 0x%X of res 0x%X), %ld so far",
                     i, *(const uint16_t*)a, e, b, thr, res, c);
        }
    }
}

// X2-SMOOTH-003-STOCK-ID / -STOCK-ID-END (decimal, inclusive; bisect switch, port of FFX): actors in this id range
// take whole stock steps (no half step), so which actor's half step causes a glitch can be found by halving.
static int g_stock_id = 0, g_stock_id_end = 0;
static bool stock_id(const uint8_t* a) {
    if (!g_stock_id) return false;
    uint16_t id = *(const uint16_t*)a;
    return id >= (uint16_t)g_stock_id && id <= (uint16_t)(g_stock_id_end ? g_stock_id_end : g_stock_id);
}

// SHORTLOOP: a looping motion of at most one frame (e.g. an enemy hold pose) is only ever sampled at whole frames at
// stock; a half-frame sample twists the body, so such actors run exactly as stock.
static bool short_loop(const uint8_t* a) {
    uint32_t fl = *(const uint32_t*)(a + FLAGS);
    if ((fl & F_SCRIPT) != 0 || *(const uint32_t*)(a + M_SEQ) == 0) return false;
    return *(const int16_t*)(a + M_LOOPS) != 1 && *(const int32_t*)(a + M_LEN) <= 0x100;
}

// Logic step stub (E8 0x6E1394 -> Ch_StepMotion 0x6E78F0, cdecl(actor, full)).
static void __cdecl h_step(uint8_t* a, int full) {
    if (g_disabled || !row_SMOOTH003.on) {
        if (g_half_on) { *g_half = 0; g_half_on = false; }
        g_step(a, full);
        return;
    }
    uint8_t* base = *g_table;
    int n = *g_count;
    if (base != g_debt_base) { memset(g_debt, 0, sizeof(g_debt)); g_debt_base = base; }
    int i = slot_of(a, base, n);
    if (i >= 0 && g_debt[i]) {
        g_debt[i] = 0;
        if (g_debt_id[i] == *(uint16_t*)a) {
            // Pay the half step the render-only walk did not take: clock only, switch forced on.
            int32_t old = *g_half;
            *g_half = 1;
            bool ok = safe_step(a, 0);
            *g_half = old;
            if (!ok) { *g_half = 0; g_half_on = false; g_step(a, full); return; }
            InterlockedIncrement(&g_pays);
        }
    }
    if (*g_half != 0 && ((g_shortloop && short_loop(a)) || stock_id(a))) {
        *g_half = 0;                                    // whole stock step: no half step, no debt
        g_step(a, full);
        *g_half = 1;
        InterlockedIncrement(&g_stock_steps);
        if (full && (g_endextrap || g_sanity)) __try { record_angles(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
    g_step(a, full);
    if (full && (g_endextrap || g_sanity)) __try { record_angles(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (i >= 0 && *g_half != 0) { g_debt[i] = 1; g_debt_id[i] = *(uint16_t*)a; }
}

// ---------------------------------------------------------------- gates (X2-SMOOTH-003-GATES)
// Ch_SeqFrame is a per-call script VM and Ch_MotevDispatch fires motion events: neither may run twice per tick.
static void __cdecl h_seq(uint8_t* a) {
    if (g_in_step && g_gates && !g_disabled) { InterlockedIncrement(&g_gated_seq); return; }
    g_seq(a);
}

static void __cdecl h_motev(uint8_t* a) {
    if (g_walked_inv && g_gates && !g_disabled && row_SMOOTH003.on) {
        int i = slot_of(a, *g_table, *g_count);
        if (i >= 0 && g_walked[i]) { InterlockedIncrement(&g_gated_motev); return; }
    }
    g_motev(a);
}

// ---------------------------------------------------------------- loop guard (X2-SMOOTH-003-LOOPGUARD)
// Returns the clock the stock logic step would sample at for clock c (the loop-wrap arithmetic of Ch_MotionLoopWrap
// 0x6E8310, read-only; no loop count is decremented, the new FFX-2 wrap counter +0x803 is not touched). kind:
// 0 = unchanged, 1 = wrapped, 2 = clamped (play once), 3 = no stock equivalent (hold).
static int32_t guard_clock(const uint8_t* a, int32_t c, int* kind) {
    *kind = 0;
    int32_t end = *(const int32_t*)(a + M_END);
    int32_t len = *(const int32_t*)(a + M_LEN);
    int16_t loops = *(const int16_t*)(a + M_LOOPS);
    if (c < 0) { *kind = 1; return 0; }
    if (loops == 1) {
        int32_t last = end & ~0xFF;                     // play once: stock never samples past the last whole frame
        if (c > last) { *kind = 2; return last; }
        return c;
    }
    if (c <= end) return c;
    if (len <= 0) {
        if (loops == 0 && len == 0) { *kind = 1; return *(const int32_t*)(a + M_START); }
        *kind = 3;
        return c;
    }
    int n = 64;
    while (c > end && n-- > 0) c -= len;
    if (c > end) { *kind = 3; return c; }
    *kind = 1;
    return c;
}

// The render-only full step of one actor (bit 24 already set). Returns false on a fault (the walk aborts).
static bool walk_step(int i, uint8_t* a, uint32_t fl) {
    int32_t c0 = *(int32_t*)(a + M_CLOCK);
    int kind = 0;
    int32_t cs = c0;
    bool keyed = (fl & F_SCRIPT) == 0 && *(uint32_t*)(a + M_SEQ) != 0;   // the script paths wrap on every call
    if (g_loopguard && keyed) cs = guard_clock(a, c0, &kind);
    // LOOPEND: a half-frame sample inside the last frame of a loop reads past the loop end (bad pose); stock samples
    // there only at whole frames, so hold the pose for that one frame.
    if (g_loopend && kind <= 1 && keyed && (cs & 0xFF) != 0 && *(int16_t*)(a + M_LOOPS) != 1 &&
        *(int32_t*)(a + M_LEN) > 0 && (cs >> 8) >= (*(int32_t*)(a + M_END) >> 8)) {
        if (!safe_step(a, 0)) return false;               // clock only: pose stays at tick k
        InterlockedIncrement(&g_loopend_holds);
        if (g_endextrap) __try { end_extrapolate(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return true;
    }
    if (kind == 3) {
        if (!safe_step(a, 0)) return false;               // clock only: pose stays at tick k
        InterlockedIncrement(&g_guard_hold);
        return true;
    }
    if (kind) *(int32_t*)(a + M_CLOCK) = cs;
    int nb = 0, kh = 0;
    __try { nb = sanity_save(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) { nb = 0; }
    __try { kh = keyhold_save(a); } __except (EXCEPTION_EXECUTE_HANDLER) { kh = 0; }
    bool ok = safe_step(a, 1);
    if (ok && kh) __try { keyhold_apply(i, a, kh); } __except (EXCEPTION_EXECUTE_HANDLER) { fault("keyhold", GetExceptionCode()); }
    if (ok && nb) __try { sanity_check(i, a, nb, c0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // X2-SMOOTH-003-ENDEXTRAP-ONCE (default 1 in FFX-2): a play-once motion clamped at its last frame (kind 2) repeats
    // that pose on the render-only frame; walks/runs are chains of play-once strides (FFX: Tidus's run, 28 frames),
    // so every stride end hitched (U 13/10/2026 "walk still stuttery" with LOOPEND extrapolation alone; D fps60x2.log
    // 23:46: clamp=190 vs loopend 55 per 2 min). Extrapolate only when the logic tick already showed the last frame.
    // FFX turned this off (Bahamut's hit-motion end guessed wrong): set 0 here if a motion end overshoots.
    if (ok && kind == 2 && g_endextrap && g_endextrap_once && c0 >= (*(int32_t*)(a + M_END) & ~0xFF))
        __try { end_extrapolate(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (kind) {
        // Unwrapped clock + the half step just added: the next logic tick wraps / counts down exactly as stock.
        *(int32_t*)(a + M_CLOCK) = c0 + (*(int32_t*)(a + M_CLOCK) - cs);
        InterlockedIncrement(kind == 2 ? &g_guard_clamp : &g_guard_wrap);
    }
    return ok;
}

// ---------------------------------------------------------------- render-only walk
// Mirrors the logic walk of Ch_UpdateAllActors 0x6E10A0 for actors that owe a half step, then dispatches skeleton
// jobs. FFX-2: no dirty-flag store, batch at >= 150 bones, next batch starts at a + 0x910.
static void walk() {
    uint8_t* base = *g_table;
    int n = *g_count;
    if (!base || n <= 0 || base != g_debt_base) return;
    if (n > MAXA) n = MAXA;
    if (!safe_sync()) return;
    if (g_hold) {
        g_hold_base = base;
        g_hold_n = n;
        for (int i = 0; i < n; i++) {
            uint8_t* a = base + i * STRIDE;
            if (a[ACTIVE] == 0 || (a[RANGE_A] != 0 && a[RANGE_B] == 0)) continue;
            bool child = *(uint32_t*)(a + PARENT) != 0;
            if (!child) hold_save(i, a, true);                  // mode-0 jobs touch every parent in range
            else if (g_debt[i]) hold_save(i, a, false);         // children: only the rebuild state
        }
    }
    uint8_t* cursor = base;
    int span = 0, bones = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * STRIDE;
        span++;
        if (a[ACTIVE] != 0 && (a[RANGE_A] == 0 || a[RANGE_B] != 0) && g_debt[i]) {
            g_debt[i] = 0;
            uint32_t fl = *(uint32_t*)(a + FLAGS);
            if (g_debt_id[i] != *(uint16_t*)a) goto next;                     // slot reused: drop the stale debt
            if (g_shortloop && short_loop(a)) { g_debt[i] = 1; goto next; }  // stock from the next logic step
            if (stock_id(a)) { g_debt[i] = 1; goto next; }                    // bisect switch, stock from the next logic step
            *(uint32_t*)(a + FLAGS) = fl | F_STEP;
            if (!walk_step(i, a, fl)) return;
            g_walked[i] = 1;
            g_walked_inv = true;
            InterlockedIncrement(&g_stepped);
            if (*(uint32_t*)(a + PARENT) == 0) {
                uint8_t* hdr = *(uint8_t**)(a + HDR);
                if (hdr) bones += *(uint16_t*)(hdr + HDR_BONES);
            }
        }
    next:
        if (bones >= BATCH_BONES) {
            if (!safe_dispatch(cursor, span)) return;
            cursor = a + STRIDE;
            span = 0;
            bones = 0;
        }
    }
    if (bones != 0) safe_dispatch(cursor, span);
}

// ---------------------------------------------------------------- X2-SMOOTH-003-PEEL
static int g_peel = 1;
static volatile uint32_t g_inv = 0;
static volatile LONG g_peel_phase = 0;           // 0 none, 1 change DLL / copy seen, 2 its DLL unloaded, 3 next DLL
static volatile uint32_t g_peel_phase_inv = 0;
static uint8_t* volatile g_peel_tail = nullptr;
static int g_peel_dll[8], g_npeel_dll = 0;
typedef void* (__cdecl* op_t)(uint8_t* scr, void* pc);
static op_t g_op116 = nullptr;
static volatile LONG g_peel_invs = 0;

static int magic_number(const char* name) {      // "magic_0937.dll" -> 937
    const char* p = strstr(name, "magic_");
    return p ? atoi(p + 6) : -1;
}

static void* __cdecl h_op116(uint8_t* scr, void* pc) {
    if (g_peel) {
        if (g_peel_phase != 1) {
            g_peel_phase = 1;
            log_line("X2-SMOOTH-003-PEEL: op 0x116 silhouette copy - limbs at stock rate for the change");
        }
        g_peel_phase_inv = g_inv;
    }
    return g_op116(scr, pc);
}

static void peel_on_load(uint8_t* base, uint32_t, const char* name) {
    int num = magic_number(name);
    for (int k = 0; k < g_npeel_dll; k++) {
        if (g_peel_dll[k] != num) continue;
        g_peel_phase_inv = g_inv;
        g_peel_phase = 1;
        log_line("X2-SMOOTH-003-PEEL: %s loaded - limbs at stock rate for the change", name);
        return;
    }
    if (g_peel_phase == 2 && g_inv - g_peel_phase_inv <= 120) {
        g_peel_tail = base;
        g_peel_phase_inv = g_inv;
        g_peel_phase = 3;
        log_line("X2-SMOOTH-003-PEEL: %s follows - limbs at stock rate until it unloads", name);
    }
}

static void peel_on_unload(uint8_t* base) {
    if (g_peel_phase == 1) { g_peel_phase_inv = g_inv; g_peel_phase = 2; }
    else if (g_peel_phase == 3 && base == g_peel_tail) {
        g_peel_tail = nullptr;
        g_peel_phase = 0;
        log_line("X2-SMOOTH-003-PEEL: change over - 60 Hz limbs again (%ld invocations held)", (LONG)g_peel_invs);
    }
}

static bool peel_hold() {
    if (!g_peel) return false;
    LONG ph = g_peel_phase;
    if (ph == 0) return false;
    uint32_t cap = ph == 2 ? 120 : 1800;
    if (g_inv - g_peel_phase_inv <= cap) return true;
    g_peel_phase = 0;
    return false;
}

static bool live3() { return smooth_live(3) && !peel_hold(); }

static void install_peel() {
    g_peel = config_row("X2-SMOOTH-003-PEEL", 1);
    if (!g_peel) { log_line("  X2-SMOOTH-003-PEEL: off (ini)"); return; }
    char buf[128];
    config_str("rows", "X2-SMOOTH-003-PEEL-DLL", buf, sizeof(buf), "936 937 938");
    for (char* q = buf; *q && g_npeel_dll < 8;) {
        char* e = nullptr; long v = strtol(q, &e, 10);
        if (e == q) { q++; continue; }
        g_peel_dll[g_npeel_dll++] = (int)v; q = e;
    }
    if (!magic_mods_subscribe("X2-SMOOTH-003-PEEL", peel_on_load, peel_on_unload))
        log_line("  X2-SMOOTH-003-PEEL: magic load hook not available - op 0x116 trigger only");
    uint8_t* at = sig_find("X2-SMOOTH-003-PEEL MagicFile_Start table push",
        "0F 94 C0 50 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 01 00 00 00 FF 15", 0x8578D1);
    uint8_t* svc = at ? abs_operand("service table", at + 0x0A) : nullptr;
    uint8_t* tab = svc ? (uint8_t*)(uintptr_t)*(uint32_t*)(svc + 0xC24) : nullptr;
    uint32_t* entry = tab && in_image(tab) ? (uint32_t*)(tab + 0x16 * 4) : nullptr;
    uint8_t* fn = entry ? (uint8_t*)(uintptr_t)*entry : nullptr;
    // FUN_0085b3d0: PUSH EBP; MOV EBP,ESP; SUB ESP,0xE4; cookie; FLDZ; PUSH EBX/ESI/EDI; EDI=scr; ESI=pc; EBX=[scr+0x220]
    if (!fn || !in_image(fn) || !sig_match(fn, "55 8B EC 81 EC E4 00 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC D9 EE 53 56 57 "
                                               "8B 7D 08 8B 75 0C 8B 9F 20 02 00 00")) {
        log_line("  X2-SMOOTH-003-PEEL: op 0x116 entry not proven (%p) - DLL trigger only", fn);
    } else {
        g_op116 = (op_t)fn;
        if (!patch_u32(entry, (uint32_t)(uintptr_t)&h_op116)) g_op116 = nullptr;
    }
    char line[64]; int k = 0; line[0] = 0;
    for (int i = 0; i < g_npeel_dll; i++) k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %04d", g_peel_dll[i]);
    log_line("  X2-SMOOTH-003-PEEL: change DLLs:%s; op 0x116 entry gva=0x%08X (0x00D6F810) -> %s", line,
             entry ? rt_to_gva(entry) : 0, g_op116 ? "stub" : "untouched");
}

// Sg_MainLoop stub (E8 0x60586C -> Ch_NoSkelPass 0x6E1610, cdecl(void)).
static void __cdecl h_noskel() {
    if (row_SMOOTH003.on && !g_disabled && g_kind == INV_BUILD && live3() && *g_freeze == 0) {
        g_walks++;
        LONGLONG t0 = perf_on() ? perf_now() : 0;
        __try { walk(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { fault("walk", GetExceptionCode()); }
        if (t0) perf_walk(perf_now() - t0);
    }
    g_noskel();
}

// ---------------------------------------------------------------- CLOTHHOLD modes 2/3/4 (worker threads)
static const uint32_t C_ENTS = 0x1C8, C_MDL = 0x1CC, C_RESET = 0x824, C_ENT_STRIDE = 0x38;
static void (__cdecl* g_deform)(uint8_t*) = nullptr;
static volatile int32_t* g_deform_gate = nullptr;
static int g_extrap_pct = 50;                    // X2-SMOOTH-003-CLOTHEXTRAP-PCT (mode 3)
static volatile LONG g_step_n = 1;               // logic invocations (game thread writes, jobs read)
static volatile LONG g_extrap_n = 0, g_extrap_miss = 0;

struct ClothSnap { float* vtex; uint8_t* actor; uint32_t n; LONG ncur, nprev; float* cur; float* prev; };
enum { NSNAP = 128 };
static ClothSnap g_snap[NSNAP];
static SRWLOCK g_snap_lock = SRWLOCK_INIT;

// Jobs run on several worker threads at once: a new entry is stamped with the current step; only empty slots or
// entries unused for 4+ logic steps are taken (FFX: heap corruption when a fresh entry was evicted mid-copy).
static ClothSnap* snap_get(float* vtex, uint8_t* actor, uint32_t n, bool create) {
    AcquireSRWLockExclusive(&g_snap_lock);
    ClothSnap* s = nullptr; ClothSnap* old = nullptr;
    LONG now = g_step_n;
    for (int i = 0; i < NSNAP; i++) {
        ClothSnap* e = &g_snap[i];
        if (e->vtex == vtex) { s = e; break; }
        if (!e->vtex) { if (!old || old->vtex) old = e; }
        else if (e->ncur < now - 4 && (!old || (old->vtex && e->ncur < old->ncur))) old = e;
    }
    if (!s && create && old) { s = old; s->vtex = vtex; s->ncur = now; s->nprev = 0; s->actor = nullptr; }
    if (s && create && (s->n != n || s->actor != actor)) {
        free(s->cur); free(s->prev);
        s->cur = (float*)malloc(n * 16); s->prev = (float*)malloc(n * 16);
        s->n = (s->cur && s->prev) ? n : 0; s->actor = actor; s->ncur = s->nprev = 0;
    }
    ReleaseSRWLockExclusive(&g_snap_lock);
    return s;
}

// Calls fn(entry, vertex array, vertex count) for each dynamic mesh of the actor.
template <class F> static void for_dyn_mesh(uint8_t* a, F fn) {
    uint8_t* ents = *(uint8_t**)(a + C_ENTS);
    uint8_t* mdl = *(uint8_t**)(a + C_MDL);
    if (!ents || !mdl) return;
    uint32_t ne = *(uint16_t*)(mdl + 6);
    for (uint32_t e = 0; e < ne; e++) {
        uint8_t* en = ents + e * C_ENT_STRIDE;
        uint8_t* desc = *(uint8_t**)en;
        float* vtex = *(float**)(en + 8);
        if (!desc || !vtex || (!*(void**)(en + 0x2C) && !*(void**)(en + 0x0C))) continue;
        uint32_t n = *(uint16_t*)(desc + 4);
        if (n) fn(en, vtex, n);
    }
}

static void cloth_snap_after_step(uint8_t* a, bool reset) {
    for_dyn_mesh(a, [&](uint8_t*, float* vtex, uint32_t n) {
        ClothSnap* s = snap_get(vtex, a, n, true);
        if (!s || !s->n) return;
        float* t = s->prev; s->prev = s->cur; s->cur = t;
        s->nprev = reset ? 0 : s->ncur;
        memcpy(s->cur, vtex, n * 16);
        s->ncur = g_step_n;
    });
}

// True when every dynamic mesh had fresh job results (and the previous tick's, if need_prev); then the mesh is
// deformed from cur + k * (cur - prev) and the vertex arrays are put back.
static bool cloth_extrap(uint8_t* a, float k, bool need_prev) {
    LONG now = g_step_n;
    bool ok = true;
    for_dyn_mesh(a, [&](uint8_t*, float* vtex, uint32_t n) {
        ClothSnap* s = snap_get(vtex, a, n, false);
        if (!s || s->n != n || s->ncur != now || (need_prev && s->nprev != now - 1)) ok = false;
    });
    if (!ok) return false;
    for_dyn_mesh(a, [&](uint8_t*, float* vtex, uint32_t n) {
        ClothSnap* s = snap_get(vtex, a, n, false);
        for (uint32_t v = 0; v < n * 4; v += 4)
            for (int c = 0; c < 3; c++) vtex[v + c] = s->cur[v + c] + k * (s->cur[v + c] - s->prev[v + c]);
    });
    if (*g_deform_gate == 0) g_deform(a);
    for_dyn_mesh(a, [&](uint8_t*, float* vtex, uint32_t n) {
        ClothSnap* s = snap_get(vtex, a, n, false);
        memcpy(vtex, s->cur, n * 16);
    });
    return true;
}

static volatile LONG g_cjob_calls[MAXA];         // FREEZEDIAG: cloth job entries per slot (worker threads)
static bool g_freezediag_on = false;

static void __cdecl h_cjob(uint8_t** list, int* count) {
    bool live = !g_disabled && row_SMOOTH003.on && live3();
    if (g_freezediag_on) __try {
        for (int k = 0; k < *count; k++) {
            int i = slot_of(list[k], *g_table, *g_count);
            if (i >= 0 && i < MAXA) InterlockedIncrement(&g_cjob_calls[i]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_clothhold >= 2 && g_walked_inv && live) {
        bool all = true;
        __try {
            for (int k = 0; k < *count && all; k++) {
                int i = slot_of(list[k], *g_table, *g_count);
                all = i >= 0 && g_walked[i];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { all = false; }
        if (all) {
            if (g_clothhold >= 3) {
                __try {
                    for (int k = 0; k < *count; k++) {
                        if (g_clothhold == 3 ? cloth_extrap(list[k], g_extrap_pct / 100.0f, true) : cloth_extrap(list[k], 0.0f, false)) {
                            LONG c = InterlockedIncrement(&g_extrap_n);
                            if (c == 1) log_line("X2-SMOOTH-003-CLOTHHOLD: first cloth mesh drawn from the job results on a render-only invocation (mode %d, id 0x%04X)",
                                                 g_clothhold, *(uint16_t*)list[k]);
                        } else InterlockedIncrement(&g_extrap_miss);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { fault("cloth render-only", GetExceptionCode()); }
            } else if (InterlockedIncrement(&g_cloth_held) == 1)
                log_line("X2-SMOOTH-003-CLOTHHOLD: first cloth job skipped on a render-only invocation (mode 2)");
            return;
        }
    }
    if (g_clothhold < 3) { g_cjob_tramp(list, count); return; }
    // Reset/teleport (+0x824 counter, +0x1A0 bit 0x800000) or an uninitialised mesh: no blend from it.
    bool reset[8] = {};
    int n = *count < 8 ? *count : 8;
    __try {
        for (int k = 0; k < n; k++) {
            uint8_t* a = list[k];
            reset[k] = *(uint16_t*)(a + C_RESET) != 0 || (*(uint32_t*)(a + FLAGS) & 0x800000) != 0;
            for_dyn_mesh(a, [&](uint8_t* en, float*, uint32_t) { if (*(void**)(en + 0x0C) && !*(int*)(en + 0x34)) reset[k] = true; });
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_cjob_tramp(list, count);
    __try {
        for (int k = 0; k < n; k++) {
            cloth_snap_after_step(list[k], reset[k]);
            if (g_clothhold == 4 && !reset[k] && live) cloth_extrap(list[k], -0.5f, true);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("cloth snap", GetExceptionCode()); }
}

// ---------------------------------------------------------------- per invocation
// Game thread, between invocations: the CALL retargets and the cloth entry hook.
static bool apply_patches() {
    uint8_t* sites[4] = { g_site_step, g_site_noskel, g_site_seq, g_site_motev };
    void* olds[4] = { (void*)g_step, (void*)g_noskel, (void*)g_seq, (void*)g_motev };
    void* news[4] = { (void*)&h_step, (void*)&h_noskel, (void*)&h_seq, (void*)&h_motev };
    int count = g_gates ? 4 : 2;
    for (int i = 0; i < count; i++) {
        if (!patch_call_rel32(sites[i], olds[i], news[i])) {
            for (int j = 0; j < i; j++) patch_call_rel32(sites[j], news[j], olds[j]);
            log_line("X2-SMOOTH-003: CALL patch %d failed - row off", i);
            return false;
        }
    }
    if (g_clothhold >= 2) {
        if (hook_entry("X2-SMOOTH-003-CLOTHHOLD", g_cjob_fn, (void*)&h_cjob, (void**)&g_cjob_tramp))
            log_line("X2-SMOOTH-003-CLOTHHOLD: Ch_ClothJob gva=0x%08X hooked (mode %d: %s)", rt_to_gva(g_cjob_fn), g_clothhold,
                     g_clothhold == 4 ? "interpolated half a tick behind" : g_clothhold == 3 ? "extrapolated" : "held, 30 Hz");
        else { g_clothhold = 0; log_line("X2-SMOOTH-003-CLOTHHOLD: hook failed - off"); }
    }
    log_line("X2-SMOOTH-003: %d CALLs retargeted on the game thread", count);
    return true;
}

// X2-SMOOTH-003-FREEZEDIAG (diagnostic, default 0): U 18/10/2026 Seymour's model holds one pose for ~0.5 s at the start
// of Tidus's Blitz Ace (clip 00:15:34, 0.4-0.9 s), then moves again. Per invocation (at post) each active parent
// actor's bone angles, cloth vertex arrays and root are hashed; when none of them changed for FREEZE_RUN invocations,
// this is logged once with the motion fields and this row's state for the slot, plus the last HIST invocations (kind,
// clock, flags, walked/debt/held, which of bones/cloth/root changed, cloth job entries), and again when it moves.
// D 18/10 first build: Seymour (id 0x113F) has a static skeleton in this battle (clock 0x100, no step bit), so what
// moves him is cloth and root. Observe only.
static int g_freezediag = 0;
enum { FREEZE_RUN = 12, HIST = 16 };
struct FreezeHist { char kind; uint8_t walked, debt, held, chg, jobs; int32_t clock; uint32_t flags; };
struct FreezeState { uint16_t id; uint8_t logged; uint8_t pos; uint32_t hb, hc, hr; int run; LONG jobs; FreezeHist h[HIST]; };
static FreezeState* g_fs = nullptr;              // MAXA
static LONG g_freeze_inv = 0;
static const uint32_t ROOT = 0x1DC;              // root matrix (X2-SMOOTH-002, S)
// FREEZEDIAG-ID / -ID-END (decimal actor ids): every invocation of these actors is traced while a magic DLL is loaded.
// U 18/10: the freeze comes during Blitz Ace (magic_0749) three times; Seymour 0x113F = 4415.
static int g_trace_id = 0, g_trace_id_end = 0;
// FREEZEDIAG-ALWAYS (default 0): trace those ids on every invocation, not only while a magic DLL is loaded (U 18/10/2026:
// Rikku's legs jump between two consecutive frames in a cutscene). Each trace line also gives the largest bone angle
// change since the slot's previous traced invocation (radians, wrapped) and how many channels changed by > 0.5 rad.
static int g_trace_always = 0;
static float* g_trace_prev[MAXA];
static volatile LONG g_magic_loaded = 0;
static void fd_on_load(uint8_t*, uint32_t, const char*) { InterlockedIncrement(&g_magic_loaded); }
static void fd_on_unload(uint8_t*) { InterlockedDecrement(&g_magic_loaded); }

static uint32_t fnv(uint32_t h, const void* p, size_t n) {
    const uint32_t* w = (const uint32_t*)p;
    for (size_t i = 0; i < n / 4; i++) { h ^= w[i]; h *= 16777619u; }
    return h;
}

static void freeze_diag() {
    uint8_t* base = *g_table;
    int n = *g_count;
    if (!base || n <= 0) return;
    if (n > MAXA) n = MAXA;
    char kind = "LRBS"[g_kind & 3];
    InterlockedIncrement(&g_freeze_inv);
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * STRIDE;
        FreezeState& s = g_fs[i];
        if (a[ACTIVE] == 0 || *(uint32_t*)(a + PARENT) != 0) { s.id = 0; continue; }
        int nb = bone_count(a);
        const uint8_t* bones = *(uint8_t* const*)(a + BONES);
        if (!nb || !bones) { s.id = 0; continue; }
        uint32_t hb = 2166136261u, hc = 2166136261u, hr = fnv(2166136261u, a + ROOT, 64);
        for (int b = 0; b < nb; b++) hb = fnv(hb, bones + b * BONE_STRIDE + BANG[0], 12);
        for_dyn_mesh(a, [&](uint8_t*, float* vtex, uint32_t nv) { hc = fnv(hc, vtex, (nv < 512 ? nv : 512) * 16); });
        LONG jobs = g_cjob_calls[i];
        uint16_t id = *(const uint16_t*)a;
        uint8_t chg = (hb != s.hb ? 1 : 0) | (hc != s.hc ? 2 : 0) | (hr != s.hr ? 4 : 0);
        FreezeHist& e = s.h[s.pos++ % HIST];
        e.kind = kind; e.walked = g_walked[i]; e.debt = g_debt[i]; e.held = g_held[i];
        e.clock = *(int32_t*)(a + M_CLOCK); e.flags = *(uint32_t*)(a + FLAGS);
        e.chg = chg; e.jobs = (uint8_t)(jobs - s.jobs);
        s.jobs = jobs; s.hb = hb; s.hc = hc; s.hr = hr;
        if ((g_magic_loaded > 0 || g_trace_always) && id >= g_trace_id && id <= g_trace_id_end) {
            const float* r = (const float*)(a + ROOT);
            float maxd = 0; int maxb = -1, maxc = 0, big = 0;
            if (!g_trace_prev[i]) g_trace_prev[i] = (float*)calloc((size_t)MAXB * 3 + 1, sizeof(float));
            float* pv = g_trace_prev[i];
            if (pv) {
                bool have = pv[MAXB * 3] == (float)id;
                for (int b = 0; b < nb; b++)
                    for (int c = 0; c < 3; c++) {
                        float v = *(const float*)(bones + b * BONE_STRIDE + BANG[c]);
                        if (have) {
                            float d = v - pv[b * 3 + c];
                            while (d > 3.14159265f) d -= 6.2831853f;
                            while (d < -3.14159265f) d += 6.2831853f;
                            if (d < 0) d = -d;
                            if (d > maxd) { maxd = d; maxb = b; maxc = c; }
                            if (d > 0.5f) big++;
                        }
                        pv[b * 3 + c] = v;
                    }
                pv[MAXB * 3] = (float)id;
            }
            log_line("X2-SMOOTH-003-FREEZEDIAG trace: inv %ld %c slot %d id 0x%04X maxd %.3f bone %d ch %d big %d | chg %d jobs %d wdh %d%d%d clock 0x%X seq 0x%X flags 0x%08X "
                     "root (%.2f, %.2f, %.2f) fwd (%.3f, %.3f) start 0x%X end 0x%X len 0x%X loops %d short %d | stock %ld lend %ld wrap %ld clamp %ld ghold %ld",
                     g_freeze_inv, kind, i, id, maxd, maxb, maxc, big, chg, e.jobs, e.walked, e.debt, e.held, e.clock, *(uint32_t*)(a + M_SEQ), e.flags,
                     r[12], r[13], r[14], r[8], r[10], *(int32_t*)(a + M_START), *(int32_t*)(a + M_END), *(int32_t*)(a + M_LEN),
                     (int)*(int16_t*)(a + M_LOOPS), short_loop(a), g_stock_steps, g_loopend_holds, g_guard_wrap, g_guard_clamp, g_guard_hold);
        }
        if (s.id != id) { s.id = id; s.run = 0; s.logged = 0; continue; }
        if (chg == 0) {
            if (++s.run == FREEZE_RUN && !s.logged) {
                s.logged = 1;
                log_line("X2-SMOOTH-003-FREEZEDIAG: pose still %d inv (inv %ld %c) slot %d id 0x%04X bones %d | clock 0x%X seq 0x%X "
                         "start 0x%X end 0x%X len 0x%X loops %d flags 0x%08X | shortloop %d stockid %d freeze %d half %d live %d",
                         s.run, g_freeze_inv, kind, i, id, nb, *(int32_t*)(a + M_CLOCK), *(uint32_t*)(a + M_SEQ),
                         *(int32_t*)(a + M_START), *(int32_t*)(a + M_END), *(int32_t*)(a + M_LEN), (int)*(int16_t*)(a + M_LOOPS),
                         *(uint32_t*)(a + FLAGS), short_loop(a), stock_id(a), (int)*g_freeze, *g_half, smooth_live(3));
                char line[900];
                int k = _snprintf_s(line, sizeof(line), _TRUNCATE,
                                    "X2-SMOOTH-003-FREEZEDIAG:   last %d (kind clock flags walked/debt/held changed(b1 c2 r4) jobs):", HIST);
                for (int j = 0; j < HIST && k > 0; j++) {
                    const FreezeHist& f = s.h[(s.pos + j) % HIST];
                    int w = _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %c %X %08X %d%d%d c%d j%d;", f.kind ? f.kind : '-', f.clock,
                                        f.flags, f.walked, f.debt, f.held, f.chg, f.jobs);
                    if (w < 0) break;
                    k += w;
                }
                log_line("%s", line);
            }
        } else {
            if (s.logged)
                log_line("X2-SMOOTH-003-FREEZEDIAG: pose moves after %d inv (inv %ld %c, changed %d, jobs %d) slot %d id 0x%04X | clock 0x%X "
                         "seq 0x%X start 0x%X end 0x%X len 0x%X loops %d flags 0x%08X",
                         s.run, g_freeze_inv, kind, chg, e.jobs, i, id, *(int32_t*)(a + M_CLOCK), *(uint32_t*)(a + M_SEQ),
                         *(int32_t*)(a + M_START), *(int32_t*)(a + M_END), *(int32_t*)(a + M_LEN), (int)*(int16_t*)(a + M_LOOPS),
                         *(uint32_t*)(a + FLAGS));
            s.run = 0; s.logged = 0;
        }
    }
}

static void pre() {
    g_inv++;
    if (!g_patched) {
        g_patched = true;
        if (!apply_patches()) { row_SMOOTH003.on = 0; return; }
    }
    smooth_poll_hotkey();
    if (g_hold_base) {
        __try { hold_restore(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_hold_base = nullptr; fault("hold restore", GetExceptionCode()); }
    }
    g_kind = g_disabled ? INV_STOCK : pacing_kind();
    if (g_kind != INV_BUILD) InterlockedIncrement(&g_step_n);
    bool want = !g_disabled && row_SMOOTH003.on && live3() && (g_kind == INV_LOGIC || g_kind == INV_BUILD);
    if (peel_hold()) InterlockedIncrement(&g_peel_invs);
    *g_half = want ? 1 : 0;
    g_half_on = want;
    g_walked_inv = false;
    memset((void*)g_walked, 0, sizeof(g_walked));
}

static void post() {
    if (g_fs) __try { freeze_diag(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_fs = nullptr; log_line("X2-SMOOTH-003-FREEZEDIAG: fault - off"); }
    static LONG last = 0;
    if (g_walks - last >= 1800) {
        last = g_walks;
        log_line("X2-SMOOTH-003: walks=%ld stepped=%ld batches=%ld pays=%ld gated(seq=%ld motev=%ld) "
                 "loopguard(wrap=%ld clamp=%ld hold=%ld) loopend_holds=%ld endx=%ld sanity=%ld stock_steps=%ld cloth_held=%ld cloth_drawn=%ld cloth_miss=%ld half=%d live=%d",
                 g_walks, g_stepped, g_batches, g_pays, g_gated_seq, g_gated_motev, g_guard_wrap, g_guard_clamp,
                 g_guard_hold, g_loopend_holds, (LONG)g_endx, (LONG)g_sanity_reverts, g_stock_steps, (LONG)g_cloth_held, (LONG)g_extrap_n, (LONG)g_extrap_miss, *g_half, smooth_live(3));
    }
    static LONG reported_s = 0;
    if (!reported_s && g_stock_steps > 0) {
        reported_s = 1;
        log_line("X2-SMOOTH-003: first stock logic step for a short-loop actor (no half step)");
    }
    static LONG reported = 0;
    if (!reported && g_stepped > 0) {
        reported = 1;
        log_line("X2-SMOOTH-003: first half-step motion walk on a render-only invocation");
    }
}

// ---------------------------------------------------------------- install (resolve + check only)
static bool install() {
    if (!smooth_master()) { log_line("  X2-SMOOTH-003: master X2-SMOOTH=0 - off"); return false; }
    // Ch_UpdateAllActors: sets the motion-step flag (bit 24), PUSH full; PUSH EDI (actor); CALL Ch_StepMotion (+0x18)
    uint8_t* sa = sig_find("X2-SMOOTH-003 logic step (Ch_UpdateAllActors)",
        "81 8E 5C FE FF FF 00 00 00 01 8B 86 5C FE FF FF C1 E8 18 83 E0 01 50 57 E8 ?? ?? ?? ?? D9 46 F8", 0x6E137C);
    // Ch_UpdateAllActors: parent test, CALL Ch_IsFrozen (+0x09), bone count of +0x1CC (no dirty store in FFX-2)
    uint8_t* sc = sig_find("X2-SMOOTH-003 skeleton bones + freeze (Ch_UpdateAllActors)",
        "83 BE 64 FE FF FF 00 75 15 E8 ?? ?? ?? ?? 85 C0 75 0C 8B 86 88 FE FF FF 0F B7 40 0A 03 D8", 0x6E156D);
    // Ch_UpdateAllActors: dispatches a skeleton job batch at >= 150 bones (E8 at +0x11)
    uint8_t* sb = sig_find("X2-SMOOTH-003 skeleton dispatch (Ch_UpdateAllActors)",
        "81 FB 96 00 00 00 7C 20 8B 45 F8 50 6A 00 FF 75 FC E8 ?? ?? ?? ?? 8D 96 CC 05 00 00", 0x6E158B);
    // Ch_StepMotion: CALL Ch_SeqFrame (+0x08), then halves the step if the half-rate switch (+0x12) is on
    uint8_t* sd = sig_find("X2-SMOOTH-003 SeqFrame call + half switch (Ch_StepMotion)",
        "56 66 89 BE 66 07 00 00 E8 ?? ?? ?? ?? 83 C4 04 83 3D ?? ?? ?? ?? 00 74 09 8B C7 99 2B C2 8B F8 D1 FF 83 7D 0C 00", 0x6E7A5D);
    // Sg_MainLoop: CMP gate; MOV [timing],0 (new in FFX-2); JZ; CALL Ch_NoSkelPass (+0x13)
    uint8_t* se = sig_find("X2-SMOOTH-003 Ch_NoSkelPass call (Sg_MainLoop)",
        "80 3D ?? ?? ?? ?? 00 C7 05 ?? ?? ?? ?? 00 00 00 00 74 20 E8 ?? ?? ?? ?? A1", 0x605859);
    // Ch_NoSkelPass: for a stepped actor, CALL Ch_MotevDispatch (+0x0E) and clear the pose-hold bit 0x1000
    uint8_t* sf = sig_find("X2-SMOOTH-003 Motev call (Ch_NoSkelPass)",
        "8B 86 A0 01 00 00 A9 00 00 00 01 74 15 56 E8 ?? ?? ?? ?? 83 C4 04 81 A6 A0 01 00 00 FF EF FF FF", 0x6E1787);
    // Ch_DecodeKeys: MOV EBX,[EDI+M_CLOCK]; MOV EAX,[EDI+M_END] - checks the motion-block shift (+0x10)
    uint8_t* sk = sig_find("X2-SMOOTH-003 sample clock (Ch_DecodeKeys)",
        "8B 9F 50 07 00 00 8B 87 2C 07 00 00 ?? 8D B3 FF 01 00 00", 0x6E841C);
    if (!sa || !sb || !sc || !sd || !se || !sf || !sk) return false;

    g_site_step = sa + 0x18; g_site_seq = sd + 0x08; g_site_noskel = se + 0x13; g_site_motev = sf + 0x0E;
    uint8_t* step = call_target(g_site_step);
    uint8_t* seq = call_target(g_site_seq);
    uint8_t* noskel = call_target(g_site_noskel);
    uint8_t* motev = call_target(g_site_motev);
    uint8_t* disp = call_target(sb + 0x11);
    uint8_t* frz = call_target(sc + 0x09);
    // Cross-checks at the resolved targets: Ch_StepMotion prologue; Ch_NoSkelPass loads the actor table, calls the job
    // sync, loads the count; job sync; skeleton dispatch (pushes its job fn); freeze getter. Windows widened for the
    // inlined tween loop (sb - sa = 0x20F, sc - sa = 0x1F1).
    static const char P_STEP[] = "55 8B EC";
    static const char P_NOSKEL[] = "55 8B EC 83 EC 08 A1 ?? ?? ?? ?? 53 89 45 FC A1 ?? ?? ?? ?? 56 57 33 DB 33 FF 89 45 F8 E8 ?? ?? ?? ?? A1";
    static const char P_SYNC[] = "8B 0D ?? ?? ?? ?? 6A 00 E8 ?? ?? ?? ?? C3";
    static const char P_DISP[] = "55 8B EC 8B 0D ?? ?? ?? ?? 6A 00 FF 75 10 FF 75 0C FF 75 08 68";
    static const char P_FRZ[] = "0F BE 05 ?? ?? ?? ?? C3";
    uint8_t* sync = (noskel && sig_match(noskel, P_NOSKEL)) ? call_target(noskel + 0x1D) : nullptr;
    bool ok = step && sig_match(step, P_STEP) && seq && motev && sync && sig_match(sync, P_SYNC) &&
              disp && sig_match(disp, P_DISP) && frz && sig_match(frz, P_FRZ) &&
              sd > step && sd < step + 0x300 && sb > sa && sb < sa + 0x280 && sc > sa && sc < sa + 0x280 && sc < sb &&
              sf > noskel && sf < noskel + 0x200 &&
              *(const uint32_t*)(sk + 2) == M_CLOCK && *(const uint32_t*)(sk + 8) == M_END;
    if (!ok) {
        log_line("  X2-SMOOTH-003: cross-check failed (step %p seq %p noskel %p sync %p motev %p disp %p freeze %p) - refused",
                 step, seq, noskel, sync, motev, disp, frz);
        return false;
    }
    g_table = (uint8_t* volatile*)abs_operand("actor table", noskel + 0x07);
    g_count = (volatile int32_t*)abs_operand("actor count", noskel + 0x23);
    g_half = (volatile int32_t*)abs_operand("half-rate switch", sd + 0x12);
    g_freeze = (volatile int8_t*)abs_operand("freeze flag", frz + 0x03);
    if (!g_table || !g_count || !g_half || !g_freeze) { log_line("  X2-SMOOTH-003: global operand outside FFX-2.exe - refused"); return false; }
    if (*g_half != 0) log_line("  X2-SMOOTH-003: note: half-rate switch was %d at install (debug menu?)", *g_half);
    g_step = (step_t)step;
    g_seq = (actor_fn_t)seq;
    g_motev = (actor_fn_t)motev;
    g_noskel = (void_fn_t)noskel;
    g_sync = (void_fn_t)sync;
    g_dispatch = (dispatch_t)disp;
    g_gates = config_row("X2-SMOOTH-003-GATES", 1);
    g_hold = config_row("X2-SMOOTH-003-HOLD", 1);
    g_loopguard = config_row("X2-SMOOTH-003-LOOPGUARD", 1);
    g_loopend = config_row("X2-SMOOTH-003-LOOPEND", 1);
    g_endextrap = config_row("X2-SMOOTH-003-ENDEXTRAP", 1);
    g_endextrap_once = config_row("X2-SMOOTH-003-ENDEXTRAP-ONCE", 1);
    g_sanity = config_row("X2-SMOOTH-003-SANITY", 1);
    g_keyhold = config_row("X2-SMOOTH-003-KEYHOLD", 1);
    g_keyhold_d = config_row("X2-SMOOTH-003-KEYHOLD-D", 0x300);
    if (g_keyhold_d < 0x80 || g_keyhold_d > 0x800) g_keyhold_d = 0x300;
    if (g_keyhold) {
        // Cross-checks of the layout KEYHOLD reads (KEYHOLD-X2.md, each 1 hit in the export):
        // Ch_DecodeKeys: MOV EAX,[EDI+0x7F8] (res) .. MOV EDI,[EDI+0x75C] (node list)
        uint8_t* k1 = sig_find("X2-SMOOTH-003-KEYHOLD decode res/list",
            "8B 87 F8 07 00 00 A3 ?? ?? ?? ?? A1 ?? ?? ?? ?? 48 A3 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B BF 5C 07 00 00", 0x6E848F);
        // rotation flag +5, lerp(prev +0x18, value +0x16) -> output +0x1C
        uint8_t* k2 = sig_find("X2-SMOOTH-003-KEYHOLD decode lerp",
            "80 7F 05 00 74 23 0F BF 47 16 DD D8 53 50 0F BF 47 18 50 E8 ?? ?? ?? ?? 83 C4 0C D9 5F 1C", 0x6E84D7);
        // Ch_SamplePose: res +0x7F8, pose +0x758
        uint8_t* k3 = sig_find("X2-SMOOTH-003-KEYHOLD sample pose",
            "8B 87 F8 07 00 00 8B B7 58 07 00 00 99 2B C2 D1 F8 89 45 08 8B 87 D0 01 00 00", 0x6E7AF3);
        // Ch_SamplePose: remap +0x768, bone u16 at +8+2c, 0xFFFF = none
        uint8_t* k4 = sig_find("X2-SMOOTH-003-KEYHOLD sample remap",
            "8B 87 68 07 00 00 85 C0 74 16 3B 55 F4 0F 8D ?? ?? ?? ?? 0F B7 54 50 08 81 FA FF FF 00 00", 0x6E80C1);
        if (!k1 || !k2 || !k3 || !k4) { g_keyhold = 0; log_line("  X2-SMOOTH-003-KEYHOLD: layout cross-check failed - off"); }
        else log_line("  X2-SMOOTH-003-KEYHOLD: on, threshold 0x%X per 0x1000 key units (pose +0x758, list +0x75C, res +0x7F8)", g_keyhold_d);
    }
    g_stock_id = config_row("X2-SMOOTH-003-STOCK-ID", 0);
    g_stock_id_end = config_row("X2-SMOOTH-003-STOCK-ID-END", 0);
    if (g_endextrap || g_sanity) {
        g_prev_ang = (float*)calloc((size_t)MAXA * MAXB * 3, sizeof(float));
        g_cur_ang = (float*)calloc((size_t)MAXA * MAXB * 3, sizeof(float));
        if (!g_prev_ang || !g_cur_ang) { g_endextrap = 0; g_sanity = 0; log_line("  X2-SMOOTH-003: ENDEXTRAP/SANITY off (out of memory)"); }
    }
    if (g_stock_id)
        log_line("  X2-SMOOTH-003-STOCK-ID: actors %d..%d (0x%04X..0x%04X) take whole stock steps", g_stock_id,
                 g_stock_id_end ? g_stock_id_end : g_stock_id, g_stock_id, g_stock_id_end ? g_stock_id_end : g_stock_id);
    g_shortloop = config_row("X2-SMOOTH-003-SHORTLOOP", 1);
    g_freezediag = config_row("X2-SMOOTH-003-FREEZEDIAG", 0);
    if (g_freezediag) {
        g_fs = (FreezeState*)calloc(MAXA, sizeof(FreezeState));
        g_freezediag_on = g_fs != nullptr;
        g_trace_id = config_row("X2-SMOOTH-003-FREEZEDIAG-ID", 0);
        g_trace_id_end = config_row("X2-SMOOTH-003-FREEZEDIAG-ID-END", 0);
        g_trace_always = config_row("X2-SMOOTH-003-FREEZEDIAG-ALWAYS", 0);
        if (!g_trace_id_end) g_trace_id_end = g_trace_id;
        if (g_trace_id && g_fs && magic_mods_subscribe("X2-SMOOTH-003-FREEZEDIAG", fd_on_load, fd_on_unload))
            log_line("  X2-SMOOTH-003-FREEZEDIAG: trace ids 0x%04X..0x%04X while a magic DLL is loaded", g_trace_id, g_trace_id_end);
        else g_trace_id = 0x10000;
        log_line("  X2-SMOOTH-003-FREEZEDIAG: %s (pose unchanged for %d invocations is logged)", g_fs ? "on" : "out of memory - off", FREEZE_RUN);
    }
    g_clothhold = config_row("X2-SMOOTH-003-CLOTHHOLD", 4);
    g_extrap_pct = config_row("X2-SMOOTH-003-CLOTHEXTRAP-PCT", 50);
    if (g_clothhold != 0 && (g_clothhold < 2 || g_clothhold > 4)) {
        log_line("  X2-SMOOTH-003-CLOTHHOLD: mode %d not available - using mode 4", g_clothhold);
        g_clothhold = 4;
    }
    if (g_clothhold) {
        // entry through MOV ESI,[EBP+8]; MOV EAX,[ESI+0x1A0]; TEST EAX,0x200000 (cloth actor bit)
        uint8_t* cloth = sig_find("X2-SMOOTH-003-CLOTHHOLD Ch_ClothSimulate",
            "55 8B EC 81 EC B8 02 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 56 8B 75 08 89 B5 78 FD FF FF 8B 86 A0 01 00 00 A9 00 00 20 00",
            0x6F4C90);
        // Ch_ClothJob: loop over count actors: CALL attach (+0x19); CALL Ch_ClothSimulate (+0x1F); deform gate; deform
        g_cjob_fn = sig_find("X2-SMOOTH-003-CLOTHHOLD cloth job (Ch_ClothJob)",
            "55 8B EC 53 8B 5D 0C 56 33 F6 39 33 7E ?? 57 90 8B 45 08 6A 00 8B 3C B0 57 E8 ?? ?? ?? ?? 57 E8 ?? ?? ?? ?? 83 C4 0C "
            "83 3D ?? ?? ?? ?? 00 75 09 57 E8", 0x6DB500);
        if (!cloth || !g_cjob_fn || call_target(g_cjob_fn + 0x1F) != cloth) {
            g_clothhold = 0;
            g_cjob_fn = nullptr;
            log_line("  X2-SMOOTH-003-CLOTHHOLD: cloth job not found or does not call Ch_ClothSimulate - off");
        }
    }
    if (g_clothhold >= 3) {
        // job +0x27: CMP dword [gate],0 / JNZ +9 / PUSH EDI / CALL Ch_ClothDeform (verified by the job signature)
        g_deform_gate = (volatile int32_t*)abs_operand("cloth deform gate", g_cjob_fn + 0x29);
        g_deform = (void (__cdecl*)(uint8_t*))call_target(g_cjob_fn + 0x31);
        if (!g_deform_gate || !g_deform || !in_image((void*)g_deform)) {
            g_clothhold = 2;
            log_line("  X2-SMOOTH-003-CLOTHHOLD: deform call not at job +0x31 - modes 3/4 unavailable, mode 2");
        } else {
            log_line("  X2-SMOOTH-003-CLOTHHOLD: mode %d, deform gva=0x%08X (expected 0x006DAE00), gate gva=0x%08X (0x00DF6440)",
                     g_clothhold, rt_to_gva((void*)g_deform), rt_to_gva((const void*)g_deform_gate));
        }
    }
    g_hold_buf = (uint8_t*)calloc(MAXA, HOLD_SZ);
    if (!g_hold_buf) { log_line("  X2-SMOOTH-003: out of memory - refused"); return false; }
    install_peel();

    log_line("  X2-SMOOTH-003: Ch_StepMotion gva=0x%08X (0x006E78F0) call 0x%08X (0x006E1394); Ch_NoSkelPass gva=0x%08X (0x006E1610) "
             "call 0x%08X (0x0060586C); SeqFrame gva=0x%08X (0x006D2720) call 0x%08X (0x006E7A65); Motev gva=0x%08X (0x006E27F0) "
             "call 0x%08X (0x006E1795); sync 0x%08X (0x00B520D0) dispatch 0x%08X (0x00B520E0); half gva=0x%08X (0x0116E3D4) "
             "freeze gva=0x%08X (0x00DF67F3); actors 0x%08X (0x01CC9604) count 0x%08X (0x01CC9600) stride 0x910; "
             "gates=%d hold=%d loopguard=%d loopend=%d shortloop=%d clothhold=%d; patches at the first pre(); F7 toggles",
             rt_to_gva(step), rt_to_gva(g_site_step), rt_to_gva(noskel), rt_to_gva(g_site_noskel),
             rt_to_gva(seq), rt_to_gva(g_site_seq), rt_to_gva(motev), rt_to_gva(g_site_motev),
             rt_to_gva(sync), rt_to_gva(disp), rt_to_gva((const void*)g_half), rt_to_gva((const void*)g_freeze),
             rt_to_gva((const void*)g_table), rt_to_gva((const void*)g_count),
             g_gates, g_hold, g_loopguard, g_loopend, g_shortloop, g_clothhold);
    return true;
}

Row row_SMOOTH003 = {
    "X2-SMOOTH-003",
    "60 Hz limbs: engine half-rate motion + a half step on render-only frames (needs X2-SMOOTH=1)",
    1, install, pre, post, 0
};

} // namespace x2
