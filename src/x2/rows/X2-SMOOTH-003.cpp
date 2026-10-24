// SPDX-License-Identifier: Apache-2.0
// X2-SMOOTH-003: arms, legs and faces animate at 60 fps.
//
// The problem. At 60 fps the game logic still runs 30 times a second (logic frame, then a render-only frame that
// draws the same state again; see x2.h). X2-SMOOTH-002 already moves each character's body at 60 Hz, but the pose of
// its skeleton is only computed on logic frames, so limbs would still move in 30 Hz steps.
//
// The idea. The engine has a built-in "half-rate" switch: while it is on, each call of Ch_StepMotion (the function
// that advances one actor's animation) moves the animation clock forward by half a frame instead of a whole one.
// We turn it on, so the logic frame advances every actor by half a frame. On the render-only frame we take the
// other half step ourselves (render_only_walk), so each logic tick still advances a whole frame (stock speed) but
// the pose is computed twice as often. An actor that took a half step on a logic frame "owes" the second half
// (g_owes_half_step); if the render-only walk does not run for it, the next logic step pays the debt first.
//
// Taking extra steps is not free: the step also runs things that must happen exactly once per tick, and sampling an
// animation half way between frames goes wrong in a few places the stock game never samples. Each safety net below
// is its own ini switch ([rows] X2-SMOOTH-003-<name>, all on by default unless noted):
//   GATES       the per-actor script (Ch_SeqFrame) and motion events (Ch_MotevDispatch) are skipped on our extra steps
//   HOLD        actor fields the extra step writes but game logic reads (look-at, face, reference points) are saved
//               before the walk and put back at the start of the next frame
//   LOOPGUARD   a half step past the end of a looping animation is wrapped the way the logic step would wrap it
//   LOOPEND     ...and a half-frame sample inside the last frame of a loop holds the pose (it would read past the end)
//   ENDEXTRAP   instead of holding, continue each bone's rotation half a tick (ENDEXTRAP-ONCE: also at the end of
//               play-once animations, e.g. each stride of a run)
//   SHORTLOOP   animations one frame long (hold poses) run exactly as stock: a half-frame sample twists them
//   SANITY      a bone that jumps much further on the render-only frame than it moved over the last tick is put back
//   DRAWCHECK   diagnostic (on while in progress): after every invocation the drawn bone angles are kept for 3 frames;
//               a bone that is the odd one out against the frames on both sides is logged with the frame kind
//   MIDCHECK    every render-only pose is checked one tick later against the true half-way point between the two
//               logic ticks around it; a bone drawn far from it is logged and drawn at its tick pose for a while
//   KEYHOLD     a bone whose two animation keys are far apart (cutscene data can store one rotation two ways) keeps
//               its pose: the half-way blend between them is a wrong pose
//   CLOTHHOLD   hair and cloth (mode 4: drawn half a tick behind from the last two results; 2: 30 Hz; 3: extrapolated)
//               (MIXED: render-only frames never step the cloth simulation, also when a batch holds unwalked actors)
//   DAMPHOLD    bones the game smooths (hair) get their angles back after the render-only step, so the next logic
//               step starts from the stock filter state
//   PHASE       logic frames keep sampling whole animation frames (as stock) when a motion restarts after the logic
//               step: that actor takes no half step on the next render-only frame
//   PEEL        during a dressphere change everything runs as stock, or the outfit's two models come apart
//               (PEEL-SKEL: still rebuild the skeletons on render-only frames, with the held pose)
//   STOCK-ID    bisect switch: actor ids in a range take whole stock steps (find which actor causes a glitch)
//   FREEZEDIAG  diagnostic, off: logs actors whose pose stops changing
//
// Needs X2-SMOOTH=1; F7 turns all SMOOTH rows off and on. The CALL patches and the cloth hook are written on the game
// thread at the first pre(). Port of the FFX mod's SMOOTH-003 with FFX-2's actor layout (0x910 bytes per actor).
// History, measurements and evidence: re/ledger/PORT-SMOOTH3.md (it also keeps this file's earlier header),
// XFADE-BLINK.md (PEEL), KEYHOLD-X2.md; signatures tools/sigs_x2_smooth3.txt (ffx2-fps60 repo).
#include "x2_smooth.h"
#include <math.h>
#include <share.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

namespace x2 {

extern Row row_SMOOTH003;

typedef void(__cdecl* step_motion_fn)(uint8_t* actor, int full);
typedef void(__cdecl* actor_fn)(uint8_t* actor);
typedef void(__cdecl* void_fn)(void);
typedef void(__cdecl* dispatch_fn)(uint8_t* start, int mode, int span);
typedef void(__cdecl* cloth_job_fn)(uint8_t** list, int* count);

// --- The game's actors ----------------------------------------------------------------------------------------------
// Actors (characters, monsters, props) live in one array of fixed-size records; "slot" = index in that array.
// Offsets inside an actor record (re/ledger/PORT-SMOOTH3.md §5; the FFX value in brackets where it differs).
static const int      MAX_ACTORS = 512;
static const uint32_t ACTOR_SIZE              = 0x910;  // bytes per actor (FFX 0x880)
static const uint32_t OFF_ACTIVE              = 0x2;    // nonzero = slot in use
static const uint32_t OFF_RANGE_SKIP          = 0x18F;  // the game skips an actor with RANGE_SKIP set and RANGE_KEEP not
static const uint32_t OFF_RANGE_KEEP          = 0x190;
static const uint32_t OFF_FLAGS               = 0x1A0;  // flag bits below (FFX 0x194)
static const uint32_t OFF_PARENT              = 0x1A8;  // 0 = a root actor, else attached to another (FFX 0x19C)
static const uint32_t OFF_MODEL_HEADER        = 0x1CC;  // pointer to the model header (FFX 0x1C0)
static const uint32_t MODEL_HEADER_BONE_COUNT = 0xA;    // u16 in the model header
// The animation ("motion") clock, in 1/256 frames: set when a motion starts, wrapped at the loop end by the game's
// Ch_MotionLoopWrap, read by the pose sampler.
static const uint32_t OFF_MOTION_SEQ      = 0x724;      // current motion, 0 = none (FFX 0x714)
static const uint32_t OFF_MOTION_START    = 0x728;
static const uint32_t OFF_MOTION_END      = 0x72C;
static const uint32_t OFF_MOTION_LOOP_LEN = 0x730;
static const uint32_t OFF_MOTION_LOOPS    = 0x734;      // s16: 1 = play once, 0 = loop forever, n = loop n times
static const uint32_t OFF_MOTION_CLOCK    = 0x750;      // FFX 0x740
static const uint32_t OFF_STEP_VALUE      = 0x766;      // u16, the clock step per call (FFX 0x756)
// The game rebuilds skeletons in batches on worker threads, one batch per 150 bones (FFX: 200).
static const int      SKELETON_BATCH_BONES = 0x96;
// Flag bits: FLAG_SCRIPTED = motion driven by the actor's script, not by keyframes; FLAG_STEP_MOTION = "step this
// actor's motion and rebuild its skeleton" (set by the game's actor update, cleared every frame)
static const uint32_t FLAG_SCRIPTED = 0x40, FLAG_STEP_MOTION = 0x1000000;

// Game functions and globals (found by signature in install()).
static step_motion_fn g_step_motion = nullptr;          // Ch_StepMotion(actor, full): advance one actor's animation
static actor_fn g_seq_frame = nullptr, g_motion_events = nullptr;   // per-actor script step; motion event dispatch
static void_fn  g_noskel_pass = nullptr, g_job_sync = nullptr;      // Ch_NoSkelPass (main loop); wait for worker jobs
static dispatch_fn g_skeleton_dispatch = nullptr;       // start one skeleton batch on the workers
static volatile int32_t*  g_half_rate_switch = nullptr; // the engine's half-rate motion switch (gva 0x0116E3D4)
static volatile int8_t*   g_actors_frozen = nullptr;    // nonzero while the game freezes all actors (gva 0x00DF67F3)
static uint8_t* volatile* g_actor_table = nullptr;      // pointer to the actor array (gva 0x01CC9604)
static volatile int32_t*  g_actor_count = nullptr;      // number of slots in use (gva 0x01CC9600)

// The four CALL instructions we point at our hooks (see apply_patches).
static uint8_t* g_callsite_step = nullptr, *g_callsite_noskel = nullptr, *g_callsite_seq = nullptr, *g_callsite_motev = nullptr;
static bool g_calls_patched = false;

// ini switches (see the header)
static int  g_opt_gates = 1, g_opt_hold = 1, g_opt_loopguard = 1, g_opt_shortloop = 1, g_opt_loopend = 1, g_opt_endextrap = 1, g_opt_endextrap_once = 1;

// --- State ----------------------------------------------------------------------------------------------------------
static int  g_inv_kind = INV_STOCK;                     // kind of the current invocation (pacing_kind at pre)
static bool g_we_set_half_rate = false;                 // we wrote 1 into the half-rate switch
static volatile bool g_in_our_step = false;             // inside one of OUR Ch_StepMotion calls (GATES)
static volatile bool g_walk_ran = false;                // the render-only walk stepped at least one actor this frame

// Per slot: the actor took a half step on the logic frame and owes the second half (with the id it had then, so a
// slot reused by another actor drops the debt). g_owed_table = actor array the debts belong to.
static uint8_t* g_owed_table = nullptr;
static uint8_t  g_owes_half_step[MAX_ACTORS];
static uint16_t g_owed_actor_id[MAX_ACTORS];
static volatile uint8_t g_walked_now[MAX_ACTORS];      // stepped by the render-only walk this frame

// --- HOLD: fields saved before the render-only walk and restored at the next pre() -----------------------------------
// Per slot, the "always" regions (every stepped actor) and then, for root actors, the face regions.
struct Region { uint32_t off, len; };
static constexpr Region HOLD_ALWAYS[] = {
    { 0x15C, 4 },      // tilt ease, unchanged (S)
    { 0x180, 8 },      // new store in FUN_006e3970 (H)
    { 0x520, 0x1C4 },  // reference points, FFX 0x510 (start S, end +0x6E3 H)
};
static constexpr Region HOLD_FACE[] = {
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
static constexpr uint32_t HOLD_BYTES = region_len(HOLD_ALWAYS) + region_len(HOLD_FACE);
static_assert(HOLD_BYTES == 0x204, "hold layout");
static uint8_t* g_held_bytes = nullptr;                 // MAX_ACTORS * HOLD_BYTES
static uint8_t  g_held_kind[MAX_ACTORS];                // 0 = nothing held, 1 = always regions, 2 = always + face
static uint8_t* g_held_table = nullptr;                 // actor array the held bytes belong to (null = none held)
static int      g_held_count = 0;

// Counters for the periodic log line in post()
static LONG g_loopend_holds = 0, g_stock_steps = 0, g_guard_wrap = 0, g_guard_clamp = 0, g_guard_hold = 0;
static LONG g_walks = 0, g_stepped = 0, g_pays = 0, g_batches = 0, g_gated_seq = 0, g_gated_motev = 0;

// CLOTHHOLD: hair and cloth are simulated by the game's cloth job (Ch_ClothJob, on worker threads) for every stepped
// actor, so the render-only walk would swing them twice per tick. Modes:
//   4 (default) after each logic job, redeform the mesh at the midpoint of the last two results; on render-only
//     frames deform the latest result: cloth moves at 60 Hz, half a tick behind, like everything else
//   3 extrapolate past the latest result (overshoots in FFX)
//   2 skip the job on render-only frames: cloth at 30 Hz, one tick behind the body (looked like a ghost image)
// Cloth layout (PORT-SMOOTH3.md): mesh entries at actor +0x1C8 (0x38 bytes each: descriptor with the vertex count u16
// at +4, vertex array at +0x08), entry count u16 at model header +6, reset counter +0x824, reset flag 0x800000 in
// the flags; the job only deforms while [0xDF6440] is 0 (job +0x27) and calls Ch_ClothDeform at job +0x31.
static int      g_opt_cloth = 4;
static uint8_t* g_cloth_job = nullptr;
static cloth_job_fn   g_cloth_job_orig = nullptr;
static volatile LONG g_cloth_skipped = 0;

static void fault(const char* where, DWORD code) {
    log_line("X2-SMOOTH-003: fault 0x%08lX in %s - row off", code, where);
    row_SMOOTH003.on = 0;
}

// Slot index of actor a in the array at base with n slots, or -1 if a is not an actor record of that array.
static int slot_of(const uint8_t* a, uint8_t* base, int n) {
    if (!base || a < base) return -1;
    uintptr_t d = (uintptr_t)(a - base);
    if (d % ACTOR_SIZE) return -1;
    int i = (int)(d / ACTOR_SIZE);
    return (i < n && i < MAX_ACTORS) ? i : -1;
}

static void damp_save(int i, const uint8_t* a);           // DAMPHOLD, below
static void damp_restore(int i, uint8_t* a);
static uint16_t g_damp_n[MAX_ACTORS];                      // DAMPHOLD: damped bones saved per actor slot

// --- HOLD: save before the walk, restore at the next pre() ------------------------------------------------------------
static void hold_save(int i, uint8_t* a, bool face) {
    uint8_t* p = g_held_bytes + i * HOLD_BYTES;
    for (const Region& r : HOLD_ALWAYS) { memcpy(p, a + r.off, r.len); p += r.len; }
    if (face) for (const Region& r : HOLD_FACE) { memcpy(p, a + r.off, r.len); p += r.len; }
    g_held_kind[i] = face ? 2 : 1;
}

// Puts the held bytes back, unless the game has moved to another actor array since (then they are dropped).
static void hold_restore() {
    if (!g_held_table) return;
    if (*g_actor_table == g_held_table) {
        int n = *g_actor_count;
        if (n > g_held_count) n = g_held_count;
        for (int i = 0; i < n; i++) {
            if (!g_held_kind[i]) continue;
            uint8_t* a = g_held_table + i * ACTOR_SIZE;
            uint8_t* p = g_held_bytes + i * HOLD_BYTES;
            for (const Region& r : HOLD_ALWAYS) { memcpy(a + r.off, p, r.len); p += r.len; }
            if (g_held_kind[i] == 2) for (const Region& r : HOLD_FACE) { memcpy(a + r.off, p, r.len); p += r.len; }
        }
        for (int i = 0; i < n; i++) if (g_damp_n[i]) damp_restore(i, g_held_table + i * ACTOR_SIZE);
    }
    memset(g_held_kind, 0, sizeof(g_held_kind));
    memset(g_damp_n, 0, sizeof(g_damp_n));
    g_held_table = nullptr;
}

// --- Our calls into game code -----------------------------------------------------------------------------------------
// Each catches a crash inside the game function and turns the row off instead of taking the game down.
static bool safe_step(uint8_t* a, int full) {
    g_in_our_step = true;
    bool ok = true;
    __try { g_step_motion(a, full); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; fault("Ch_StepMotion", GetExceptionCode()); }
    g_in_our_step = false;
    return ok;
}

static bool safe_dispatch(uint8_t* start, int span) {
    __try { g_skeleton_dispatch(start, 0, span); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("skeleton dispatch", GetExceptionCode()); return false; }
    InterlockedIncrement(&g_batches);
    return true;
}

static bool safe_sync() {
    __try { g_job_sync(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("job sync", GetExceptionCode()); return false; }
    return true;
}

// --- ENDEXTRAP: continue the motion at a loop end instead of holding it --------------------------------------------
// LOOPEND holds a loop's last frame for one render-only frame, which repeats a pose once per loop cycle (a visible
// hitch in walk cycles). Instead each bone rotation continues half a tick past the latest logic tick k from its last
// two values: k + (k - (k-1)) / 2. A rotation that moved more than ENDX_MAX_TURN in the last tick (a flip or a cut)
// keeps tick k. Angles are recorded after every logic step (record_angles) and kept in turns (1 turn = 360 degrees).
// Bone layout: bone array pointer at actor +0x334, 0x160 bytes per bone, three rotation angles (radians) at
// +0xE4/+0xE8/+0xEC and a mirror copy of each 0x30 bytes later.
static const uint32_t OFF_BONES = 0x334, BONE_SIZE = 0x160;
static const uint32_t BONE_ANGLE[3] = { 0xE4, 0xE8, 0xEC };
static const int      MAX_BONES = 1024;
static const float    TURNS_PER_RADIAN = 0.15915494f, ENDX_MAX_TURN = 0.08f;
static float*   g_prev_ang = nullptr;            // MAX_ACTORS * MAX_BONES * 3 angles, in turns: tick k-1
static float*   g_cur_ang = nullptr;             // the same for tick k
static uint16_t g_angles_actor_id[MAX_ACTORS];   // actor id the recorded angles belong to
static uint8_t  g_angles_ticks[MAX_ACTORS];      // ticks recorded for that actor so far (2 = both k-1 and k valid)
static volatile LONG g_endx = 0;

// Bones in the actor's skeleton (0 = no model, or more than we keep room for).
static int bone_count(const uint8_t* a) {
    const uint8_t* hdr = *(uint8_t* const*)(a + OFF_MODEL_HEADER);
    if (!hdr) return 0;
    int n = *(const uint16_t*)(hdr + MODEL_HEADER_BONE_COUNT);
    return (n > 0 && n <= MAX_BONES) ? n : 0;
}

static void midcheck_judge(int i, const uint8_t* a);    // MIDCHECK, below

// --- DAMPHOLD: damped bones start every logic step from their stock state ------------------------------------------
// Some bones (bone byte +0x144 set, e.g. hair) are smoothed by the game: each pose write sets the target (+0x114..)
// and moves the drawn angles (+0xE4..) only part of the way toward it (0x6E8ED0, weight on the old angle
// 0.7 - 10 x change in radians). Our render-only step writes the pose too, so the filter took one extra step per tick
// and the next logic frame started from a different old angle than stock (U 24/10/2026: Rikku's fringe pulled up for
// one tick at 60 fps, not at stock). The damped bones' angles and targets are saved before the render-only step and
// put back at the next pre(), so render-only frames draw the half step and logic frames match stock.
static int g_opt_damphold = 1;
struct DampSave { uint16_t bone; float angle[3]; float target[3]; };
static const int  DAMP_MAX_BONES = 128;                    // damped bones kept per actor
static const uint32_t BONE_DAMPED = 0x144, BONE_TARGET = 0x114;
static DampSave*  g_damp_save = nullptr;                   // MAX_ACTORS * DAMP_MAX_BONES
static uint8_t*   g_damp_bones[MAX_ACTORS];                // bone array the saved values belong to
static volatile LONG g_damp_restored = 0;

static void damp_save(int i, const uint8_t* a) {
    g_damp_n[i] = 0;
    uint8_t* bones = *(uint8_t* const*)(a + OFF_BONES);
    int nb = bone_count(a);
    if (!g_damp_save || !bones || !nb) return;
    DampSave* out = g_damp_save + (size_t)i * DAMP_MAX_BONES;
    int k = 0;
    for (int b = 0; b < nb && k < DAMP_MAX_BONES; b++) {
        const uint8_t* hb = bones + b * BONE_SIZE;
        if (!hb[BONE_DAMPED]) continue;
        out[k].bone = (uint16_t)b;
        memcpy(out[k].angle, hb + BONE_ANGLE[0], 12);
        memcpy(out[k].target, hb + BONE_TARGET, 12);
        k++;
    }
    g_damp_bones[i] = bones;
    g_damp_n[i] = (uint16_t)k;
}

static void damp_restore(int i, uint8_t* a) {
    int k = g_damp_n[i];
    g_damp_n[i] = 0;
    uint8_t* bones = *(uint8_t**)(a + OFF_BONES);
    if (!k || bones != g_damp_bones[i]) return;
    const DampSave* in = g_damp_save + (size_t)i * DAMP_MAX_BONES;
    for (int j = 0; j < k; j++) {
        uint8_t* hb = bones + in[j].bone * BONE_SIZE;
        memcpy(hb + BONE_ANGLE[0], in[j].angle, 12);
        memcpy(hb + BONE_TARGET, in[j].target, 12);
    }
    if (InterlockedIncrement(&g_damp_restored) == 1)
        log_line("X2-SMOOTH-003-DAMPHOLD: first damped bones put back before a logic step (id 0x%04X, %d bones)", *(uint16_t*)a, k);
}


// After a logic step: tick k-1 <- tick k <- this pose.
static void record_angles(int i, const uint8_t* a) {
    if (!g_cur_ang || i < 0 || i >= MAX_ACTORS) return;
    int nb = bone_count(a);
    const uint8_t* bones = *(uint8_t* const*)(a + OFF_BONES);
    if (!nb || !bones) { g_angles_ticks[i] = 0; return; }
    uint16_t id = *(const uint16_t*)a;
    if (g_angles_actor_id[i] != id) { g_angles_ticks[i] = 0; g_angles_actor_id[i] = id; }
    float* prev = g_prev_ang + (size_t)i * MAX_BONES * 3;
    float* cur = g_cur_ang + (size_t)i * MAX_BONES * 3;
    memcpy(prev, cur, sizeof(float) * nb * 3);
    for (int b = 0; b < nb; b++)
        for (int c = 0; c < 3; c++) cur[b * 3 + c] = *(const float*)(bones + b * BONE_SIZE + BONE_ANGLE[c]) * TURNS_PER_RADIAN;
    if (g_angles_ticks[i] < 2) g_angles_ticks[i]++;
}

// On a render-only frame whose pose was held at tick k: write k + half the last tick's change into every bone.
static void end_extrapolate(int i, uint8_t* a) {
    if (!g_cur_ang || i < 0 || i >= MAX_ACTORS || g_angles_ticks[i] < 2 || g_angles_actor_id[i] != *(const uint16_t*)a) return;
    int nb = bone_count(a);
    uint8_t* bones = *(uint8_t**)(a + OFF_BONES);
    if (!nb || !bones) return;
    const float* prev = g_prev_ang + (size_t)i * MAX_BONES * 3;
    const float* cur = g_cur_ang + (size_t)i * MAX_BONES * 3;
    for (int b = 0; b < nb; b++)
        for (int c = 0; c < 3; c++) {
            float d = cur[b * 3 + c] - prev[b * 3 + c];
            d -= (float)(int)d;                          // shortest signed difference in turns
            if (d > 0.5f) d -= 1.0f;
            if (d < -0.5f) d += 1.0f;
            if (d > ENDX_MAX_TURN || d < -ENDX_MAX_TURN) continue;
            float v = (cur[b * 3 + c] + 0.5f * d) * 6.2831853f;
            *(float*)(bones + b * BONE_SIZE + BONE_ANGLE[c]) = v;
            *(float*)(bones + b * BONE_SIZE + BONE_ANGLE[c] + 0x30) = v;
        }
    if (InterlockedIncrement(&g_endx) == 1)
        log_line("X2-SMOOTH-003: first loop-end half-tick extrapolation (ENDEXTRAP), id 0x%04X", *(const uint16_t*)a);
}

// --- SANITY: undo a bone that jumps on the render-only frame --------------------------------------------------------
// Two keyframes far apart (e.g. ~180 degrees) that the stock game never samples between give a wrong pose when we
// sample half way (seen as twisted monster limbs). Per bone: if the render-only step moved any of its three angles more
// than max(0.06 turn, 3 x that angle's change over the last tick + 0.01) away from tick k, the bone goes back to its
// tick-k angles (all three).
static int      g_opt_sanity = 1;
static uint8_t  g_sanity_bone_save[MAX_BONES * BONE_SIZE];   // the bones before the render-only step (tick k)
static volatile LONG g_sanity_reverts = 0, g_sanity_lines = 0;

// Size of the shortest turn between two angles given in turns (0 .. 0.5).
static inline float turn_diff(float x, float y) {
    float d = x - y;
    d -= (float)(int)d;                                   // fractional part, sign kept
    if (d > 0.5f) d -= 1.0f;
    if (d < -0.5f) d += 1.0f;
    return d < 0 ? -d : d;
}

// Before the render-only step: save the bones. Returns the bone count (0 = cannot check).
static int sanity_save(int i, const uint8_t* a) {
    if (!g_opt_sanity || !g_cur_ang || i < 0 || i >= MAX_ACTORS) return 0;
    int nb = bone_count(a);
    const uint8_t* bones = *(uint8_t* const*)(a + OFF_BONES);
    if (!nb || !bones) return 0;
    memcpy(g_sanity_bone_save, bones, (size_t)nb * BONE_SIZE);
    return nb;
}

static void sanity_check(int i, uint8_t* a, int nb, int32_t c0) {
    if (!nb || g_angles_ticks[i] < 2 || g_angles_actor_id[i] != *(const uint16_t*)a) return;
    uint8_t* bones = *(uint8_t**)(a + OFF_BONES);
    if (!bones) return;
    const float* prev = g_prev_ang + (size_t)i * MAX_BONES * 3;
    const float* cur = g_cur_ang + (size_t)i * MAX_BONES * 3;
    for (int b = 0; b < nb; b++) {
        const uint8_t* kb = g_sanity_bone_save + b * BONE_SIZE;
        uint8_t* hb = bones + b * BONE_SIZE;
        int jumped = -1;
        float vk = 0, vh = 0, dh = 0, dt = 0;
        for (int c = 0; c < 3 && jumped < 0; c++) {
            vk = *(const float*)(kb + BONE_ANGLE[c]) * TURNS_PER_RADIAN;
            vh = *(const float*)(hb + BONE_ANGLE[c]) * TURNS_PER_RADIAN;
            dh = turn_diff(vh, vk);
            dt = turn_diff(cur[b * 3 + c], prev[b * 3 + c]);
            if (!(dh <= 0.06f || dh <= 3.0f * dt + 0.01f)) jumped = c;     // NaN counts as a jump
        }
        if (jumped < 0) continue;
        // The whole bone goes back: the three angles are one rotation, and a bone whose keys switch to the other
        // equivalent angle triple changes all three at once; restoring only the channel that jumped mixes the two
        // triples into a wrong pose (Rikku's braids, map 0xB5).
        for (int c = 0; c < 3; c++) {
            *(float*)(hb + BONE_ANGLE[c]) = *(const float*)(kb + BONE_ANGLE[c]);
            *(float*)(hb + BONE_ANGLE[c] + 0x30) = *(const float*)(kb + BONE_ANGLE[c] + 0x30);
        }
        InterlockedIncrement(&g_sanity_reverts);
        if (g_sanity_lines < 40) {
            InterlockedIncrement(&g_sanity_lines);
            log_line("X2-SMOOTH-003 sanity: slot=%d id=0x%04X bone %d ch %d: k-1 %.4f k %.4f half %.4f (d %.4f vs tick %.4f) "
                     "clock %d start=%d end=%d len=%d loops=%d fl=%08X - bone held",
                     i, *(const uint16_t*)a, b, jumped, prev[b * 3 + jumped], vk, vh, dh, dt, c0, *(int32_t*)(a + OFF_MOTION_START),
                     *(int32_t*)(a + OFF_MOTION_END), *(int32_t*)(a + OFF_MOTION_LOOP_LEN), (int)*(int16_t*)(a + OFF_MOTION_LOOPS), *(uint32_t*)(a + OFF_FLAGS));
        }
    }
}

// --- KEYHOLD: keep the pose where two animation keys are far apart --------------------------------------------------
// A half-frame sample blends each rotation between two stored keys. Cutscene data sometimes stores one rotation in
// two different forms in consecutive keys (or sits near gimbal lock); the stock game never samples between them, and
// the blend is a wrong pose (seen as Rikku's legs flicking between two poses at a motion start). A bone whose keys
// differ by more than X2-SMOOTH-003-KEYHOLD-D on any rotation (scaled to the motion's key resolution) keeps its
// pre-step pose. The key streams are not touched, so the next logic step decodes exactly as stock.
// Pose layout (re/ledger/KEYHOLD-X2.md): the pose at [actor+0x758] has one entry of 0x18C bytes per bone, made of 9
// channel nodes of 0x2C bytes (rotation nodes have byte +5 set; keys at +0x16 (next) and +0x18 (previous); output
// +0x1C, current +0x20). The nodes in use form a linked list from [actor+0x75C]. Key resolution [actor+0x7F8]
// (0x1000..0x8000 = one turn); clock the pose was sampled at +0x754; pose entry -> bone remap [actor+0x768] (count u16
// at +2, bone u16 at +8 + 2 * entry, 0xFFFF = none; null = entry n is bone n).
static const uint32_t OFF_POSE = 0x758, OFF_POSE_LIST = 0x75C, OFF_SAMPLED_CLOCK = 0x754, OFF_POSE_REMAP = 0x768, OFF_KEY_RES = 0x7F8, POSE_NODE_SIZE = 0x2C, POSE_ENTRY_SIZE = 0x18C;
static int      g_opt_keyhold = 1, g_opt_keyhold_limit = 0x300;            // limit = KEYHOLD-D, per 0x1000 key units
static uint8_t  g_keyhold_pose_save[MAX_BONES * POSE_ENTRY_SIZE];          // pose and bones before the render-only step
static uint8_t  g_keyhold_bone_save[MAX_BONES * BONE_SIZE];
static uint8_t  g_keyhold_marked[MAX_BONES];                               // pose entries to put back
static volatile LONG g_keyholds = 0, g_keyhold_lines = 0;

// Before the render-only step: save pose entries and bones. Returns the entry count (0 = no check).
static int keyhold_save(const uint8_t* a) {
    if (!g_opt_keyhold) return 0;
    int nb = bone_count(a);
    const uint8_t* pose = *(uint8_t* const*)(a + OFF_POSE);
    const uint8_t* bones = *(uint8_t* const*)(a + OFF_BONES);
    if (!nb || !pose || !bones) return 0;
    memcpy(g_keyhold_pose_save, pose, (size_t)nb * POSE_ENTRY_SIZE);
    memcpy(g_keyhold_bone_save, bones, (size_t)nb * BONE_SIZE);
    return nb;
}

// After the render-only step: find rotation nodes whose two keys are too far apart and put their entries' pose and
// bones back to the saved (pre-step) values.
static void keyhold_apply(int i, uint8_t* a, int nb) {
    if ((*(const int32_t*)(a + OFF_SAMPLED_CLOCK) & 0xFF) == 0) return;          // whole frame: no lerp between keys
    int32_t res = *(const int32_t*)(a + OFF_KEY_RES);
    if (res != 0x1000 && res != 0x2000 && res != 0x4000 && res != 0x8000) return;
    uint8_t* pose = *(uint8_t**)(a + OFF_POSE);
    uint8_t* bones = *(uint8_t**)(a + OFF_BONES);
    if (!pose || !bones) return;
    int mask = res - 1, half = res / 2, thr = g_opt_keyhold_limit * (res / 0x1000);
    memset(g_keyhold_marked, 0, (size_t)nb);
    int marked = 0, guard = nb * 9;
    for (uint8_t* n = *(uint8_t**)(a + OFF_POSE_LIST); n && guard-- > 0; n = *(uint8_t**)n) {
        if (n < pose || n >= pose + (size_t)nb * POSE_ENTRY_SIZE || (n - pose) % POSE_NODE_SIZE) break;   // not a node of this pose: stop
        if (!n[5]) continue;                                                       // linear channel
        int d = (int)(uint16_t)(*(const int16_t*)(n + 0x16) - *(const int16_t*)(n + 0x18)) & mask;
        if (d > half) d = res - d;
        if (d <= thr) continue;
        int e = (int)((n - pose) / POSE_ENTRY_SIZE);
        if (!g_keyhold_marked[e]) { g_keyhold_marked[e] = 1; marked++; }
    }
    if (!marked) return;
    const uint8_t* remap = *(const uint8_t* const*)(a + OFF_POSE_REMAP);
    int rcount = remap ? *(const uint16_t*)(remap + 2) : 0;
    for (int e = 0; e < nb; e++) {
        if (!g_keyhold_marked[e]) continue;
        for (int c = 0; c < 3; c++) {                                  // rotation channels: output +0x1C, current +0x20
            uint8_t* dst = pose + (size_t)e * POSE_ENTRY_SIZE + c * POSE_NODE_SIZE;
            const uint8_t* src = g_keyhold_pose_save + (size_t)e * POSE_ENTRY_SIZE + c * POSE_NODE_SIZE;
            memcpy(dst + 0x1C, src + 0x1C, 8);
        }
        int b = e;
        if (remap) { if (e >= rcount) continue; b = *(const uint16_t*)(remap + 8 + 2 * e); if (b == 0xFFFF || b >= nb) continue; }
        uint8_t* hb = bones + (size_t)b * BONE_SIZE;
        const uint8_t* kb = g_keyhold_bone_save + (size_t)b * BONE_SIZE;
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

// --- Actors that run as stock -----------------------------------------------------------------------------------------
// STOCK-ID / STOCK-ID-END (decimal actor ids, inclusive; a bisect switch): actors in this range take whole stock steps,
// so the actor whose half step causes a glitch can be found by halving the range.
static int g_stock_id = 0, g_stock_id_end = 0;
static bool stock_id(const uint8_t* a) {
    if (!g_stock_id) return false;
    uint16_t id = *(const uint16_t*)a;
    return id >= (uint16_t)g_stock_id && id <= (uint16_t)(g_stock_id_end ? g_stock_id_end : g_stock_id);
}

// SHORTLOOP: a looping motion at most one frame long (e.g. an enemy's hold pose) is only ever sampled at whole frames
// by the stock game; a half-frame sample twists the body, so such actors run exactly as stock.
static bool short_loop(const uint8_t* a) {
    uint32_t fl = *(const uint32_t*)(a + OFF_FLAGS);
    if ((fl & FLAG_SCRIPTED) != 0 || *(const uint32_t*)(a + OFF_MOTION_SEQ) == 0) return false;
    return *(const int16_t*)(a + OFF_MOTION_LOOPS) != 1 && *(const int32_t*)(a + OFF_MOTION_LOOP_LEN) <= 0x100;
}

// --- PHASE: logic frames sample whole animation frames, as stock ------------------------------------------------------
// With the half step, logic frames sample a motion at whole frames (clock % 256 == 0, where stock samples) and
// render-only frames half way. When the actor's script starts a new motion after the logic step (clock reset to a
// whole frame), the render-only step would sample its whole frame and every later logic frame would land half way
// between keys: logic frames then drew the half-frame blends KEYHOLD/SANITY only guard on render-only frames (U/D
// 24/10/2026: Rikku's fringe pulled up for a tick, only in runs where logic samples were at clock % 256 == 128).
// So the clock is noted after each logic step; an actor whose clock changed by the render-only walk takes no half
// step that frame (it keeps its pose; the next logic step samples the new motion's first frame, as stock).
static int g_opt_phase = 1;
static int32_t g_clock_after_step[MAX_ACTORS];
static volatile LONG g_phase_skips = 0, g_half_logic_samples = 0;

// Logic step about to sample half way between frames (diagnostic: the phase got flipped somewhere).
static void phase_check_logic(int i, const uint8_t* a) {
    int32_t clock = *(const int32_t*)(a + OFF_MOTION_CLOCK);
    if ((clock & 0xFF) != 0x80) return;
    LONG c = InterlockedIncrement(&g_half_logic_samples);
    if (c <= 8)
        log_line("X2-SMOOTH-003-PHASE: logic step of slot=%d id=0x%04X samples half way (clock %d, step value %u)", i,
                 *(const uint16_t*)a, clock, *(const uint16_t*)(a + OFF_STEP_VALUE));
}

// --- The logic frame's step ---------------------------------------------------------------------------------------------
// Replaces the game's call of Ch_StepMotion in its actor update (CALL at gva 0x6E1394 -> 0x6E78F0). With the half-rate
// switch on, the game's own step moves the actor half a frame; the actor then owes the other half to the render-only
// walk. If it still owes one from the last frame (the walk skipped it), that half is paid first, clock only.
static void __cdecl hook_step_motion(uint8_t* a, int full) {
    if (g_disabled || !row_SMOOTH003.on) {
        if (g_we_set_half_rate) { *g_half_rate_switch = 0; g_we_set_half_rate = false; }
        g_step_motion(a, full);
        return;
    }
    uint8_t* base = *g_actor_table;
    int n = *g_actor_count;
    if (base != g_owed_table) { memset(g_owes_half_step, 0, sizeof(g_owes_half_step)); g_owed_table = base; }
    int i = slot_of(a, base, n);
    if (i >= 0 && g_owes_half_step[i]) {
        g_owes_half_step[i] = 0;
        if (g_owed_actor_id[i] == *(uint16_t*)a) {
            // Pay the half step the render-only walk did not take: clock only, switch forced on.
            int32_t old = *g_half_rate_switch;
            *g_half_rate_switch = 1;
            bool ok = safe_step(a, 0);
            *g_half_rate_switch = old;
            if (!ok) { *g_half_rate_switch = 0; g_we_set_half_rate = false; g_step_motion(a, full); return; }
            InterlockedIncrement(&g_pays);
        }
    }
    if (*g_half_rate_switch != 0 && ((g_opt_shortloop && short_loop(a)) || stock_id(a))) {
        *g_half_rate_switch = 0;                                    // whole stock step: no half step, no debt
        g_step_motion(a, full);
        *g_half_rate_switch = 1;
        InterlockedIncrement(&g_stock_steps);
        if (full && (g_opt_endextrap || g_opt_sanity)) __try { midcheck_judge(i, a); record_angles(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
    if (i >= 0 && g_inv_kind == INV_LOGIC && *g_half_rate_switch != 0) phase_check_logic(i, a);
    g_step_motion(a, full);
    if (full && (g_opt_endextrap || g_opt_sanity)) __try { midcheck_judge(i, a); record_angles(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (i >= 0 && *g_half_rate_switch != 0) {
        g_owes_half_step[i] = 1; g_owed_actor_id[i] = *(uint16_t*)a;
        g_clock_after_step[i] = *(int32_t*)(a + OFF_MOTION_CLOCK);
    }
}

// --- GATES: things that must run once per tick -----------------------------------------------------------------------
// Ch_SeqFrame runs one step of the actor's script each time it is called, and Ch_MotevDispatch fires the motion's events
// (sounds, effects): our extra steps must not run either of them a second time.
static void __cdecl hook_seq_frame(uint8_t* a) {
    if (g_in_our_step && g_opt_gates && !g_disabled) { InterlockedIncrement(&g_gated_seq); return; }
    g_seq_frame(a);
}

static void __cdecl hook_motion_events(uint8_t* a) {
    if (g_walk_ran && g_opt_gates && !g_disabled && row_SMOOTH003.on) {
        int i = slot_of(a, *g_actor_table, *g_actor_count);
        if (i >= 0 && g_walked_now[i]) { InterlockedIncrement(&g_gated_motev); return; }
    }
    g_motion_events(a);
}

// --- LOOPGUARD / LOOPEND: the render-only step near the end of an animation ------------------------------------------
// The clock the logic step would sample at for clock c: the loop-wrap arithmetic of the game's Ch_MotionLoopWrap
// (gva 0x6E8310), computed without changing anything (no loop count decremented, the wrap counter at +0x803 untouched).
// *kind: 0 = c unchanged, 1 = wrapped back into the loop, 2 = clamped at the last frame (play-once motion),
// 3 = the stock game has no equivalent (the caller holds the pose).
static int32_t guard_clock(const uint8_t* a, int32_t c, int* kind) {
    *kind = 0;
    int32_t end = *(const int32_t*)(a + OFF_MOTION_END);
    int32_t len = *(const int32_t*)(a + OFF_MOTION_LOOP_LEN);
    int16_t loops = *(const int16_t*)(a + OFF_MOTION_LOOPS);
    if (c < 0) { *kind = 1; return 0; }
    if (loops == 1) {
        int32_t last = end & ~0xFF;                     // play once: stock never samples past the last whole frame
        if (c > last) { *kind = 2; return last; }
        return c;
    }
    if (c <= end) return c;
    if (len <= 0) {
        if (loops == 0 && len == 0) { *kind = 1; return *(const int32_t*)(a + OFF_MOTION_START); }
        *kind = 3;
        return c;
    }
    int n = 64;
    while (c > end && n-- > 0) c -= len;
    if (c > end) { *kind = 3; return c; }
    *kind = 1;
    return c;
}

// --- MIDCHECK: learn which bones the half step draws wrong, and hold them ------------------------------------------
// The render-only frame shows a pose half way between logic ticks k and k+1, but it is drawn before tick k+1 exists, so
// a bad half-frame sample can only be recognised afterwards. Each render-only step keeps the angles it produced
// ("sampled"); the next logic step compares them with the true half-way angle mid = k + (k+1 - k) / 2. A channel that
// was off by more than max(MIDCHECK_MIN_ERR, half the tick's own movement) marks its bone, and a marked bone is drawn
// at its tick-k pose on render-only frames for MIDCHECK_HOLD_TICKS logic ticks (renewed while its samples stay bad;
// they are still taken and checked while it is held). Catches what KEYHOLD and SANITY miss, e.g. a bone whose two
// keys agree but whose half sample is a quarter turn off (seen on Rikku's braids, map 0xB5).
static const float MIDCHECK_MIN_ERR = 0.04f;               // turns (about 14 degrees)
static const uint16_t MIDCHECK_HOLD_TICKS = 90;            // 3 s at 30 ticks/s
static int       g_opt_midcheck = 1;
static float*    g_sampled_ang = nullptr;                  // MAX_ACTORS * MAX_BONES * 3: last render-only sample, turns
static uint16_t* g_mid_hold = nullptr;                     // MAX_ACTORS * MAX_BONES: logic ticks the bone stays held
static uint16_t  g_sampled_id[MAX_ACTORS];                 // actor id of the sample (0xFFFF = none waiting)
static int       g_sampled_bones[MAX_ACTORS];
static volatile LONG g_mid_bad = 0, g_mid_held = 0, g_mid_lines = 0;

// The judgement, in turns: was `sampled` too far from the half-way angle between ticks k and k1?
static bool midcheck_bad(float k, float sampled, float k1, float* err_out) {
    float d = k1 - k;
    d -= (float)(int)d;
    if (d > 0.5f) d -= 1.0f;
    if (d < -0.5f) d += 1.0f;
    float mid = k + 0.5f * d, err = turn_diff(sampled, mid), move = d < 0 ? -d : d;
    if (err_out) *err_out = err;
    return !(err <= MIDCHECK_MIN_ERR || err <= 0.5f * move);       // NaN counts as bad
}

// Render-only step done (after KEYHOLD and SANITY): keep the sampled angles, then put held bones back to tick k
// (g_sanity_bone_save holds the pre-step bones).
static void midcheck_after_step(int i, uint8_t* a, int nb) {
    if (!g_opt_midcheck || !g_sampled_ang || i < 0 || i >= MAX_ACTORS) return;
    uint8_t* bones = *(uint8_t**)(a + OFF_BONES);
    if (!bones) return;
    float* sampled = g_sampled_ang + (size_t)i * MAX_BONES * 3;
    uint16_t* hold = g_mid_hold + (size_t)i * MAX_BONES;
    for (int b = 0; b < nb; b++) {
        uint8_t* hb = bones + b * BONE_SIZE;
        for (int c = 0; c < 3; c++) sampled[b * 3 + c] = *(const float*)(hb + BONE_ANGLE[c]) * TURNS_PER_RADIAN;
        if (!hold[b]) continue;
        const uint8_t* kb = g_sanity_bone_save + b * BONE_SIZE;
        for (int c = 0; c < 3; c++) {
            *(float*)(hb + BONE_ANGLE[c]) = *(const float*)(kb + BONE_ANGLE[c]);
            *(float*)(hb + BONE_ANGLE[c] + 0x30) = *(const float*)(kb + BONE_ANGLE[c] + 0x30);
        }
        InterlockedIncrement(&g_mid_held);
    }
    g_sampled_id[i] = *(const uint16_t*)a;
    g_sampled_bones[i] = nb;
}

// Logic step done, before record_angles moves tick k to k-1: judge the last render-only sample against k and k+1.
static void midcheck_judge(int i, const uint8_t* a) {
    if (!g_opt_midcheck || !g_sampled_ang || i < 0 || i >= MAX_ACTORS) return;
    uint16_t id = *(const uint16_t*)a;
    uint16_t* hold = g_mid_hold + (size_t)i * MAX_BONES;
    if (g_angles_actor_id[i] != id) { memset(hold, 0, sizeof(uint16_t) * MAX_BONES); g_sampled_id[i] = 0xFFFF; return; }
    for (int b = 0; b < MAX_BONES; b++) if (hold[b]) hold[b]--;
    int nb = bone_count(a);
    const uint8_t* bones = *(uint8_t* const*)(a + OFF_BONES);
    bool judge = g_sampled_id[i] == id && g_angles_ticks[i] >= 1 && nb && bones && g_sampled_bones[i] == nb;
    g_sampled_id[i] = 0xFFFF;
    if (!judge) return;
    const float* cur = g_cur_ang + (size_t)i * MAX_BONES * 3;            // tick k
    const float* sampled = g_sampled_ang + (size_t)i * MAX_BONES * 3;
    for (int b = 0; b < nb; b++)
        for (int c = 0; c < 3; c++) {
            float k1 = *(const float*)(bones + b * BONE_SIZE + BONE_ANGLE[c]) * TURNS_PER_RADIAN, err = 0;
            if (!midcheck_bad(cur[b * 3 + c], sampled[b * 3 + c], k1, &err)) continue;
            bool was_held = hold[b] != 0;
            hold[b] = MIDCHECK_HOLD_TICKS;
            InterlockedIncrement(&g_mid_bad);
            if (!was_held && g_mid_lines < 60) {
                InterlockedIncrement(&g_mid_lines);
                log_line("X2-SMOOTH-003-MIDCHECK: slot=%d id=0x%04X bone %d ch %d: k %.4f sampled %.4f k+1 %.4f (off %.4f turn) "
                         "res 0x%X clock %d - bone held on render-only frames",
                         i, id, b, c, cur[b * 3 + c], sampled[b * 3 + c], k1, err, *(const int32_t*)(a + OFF_KEY_RES),
                         *(const int32_t*)(a + OFF_MOTION_CLOCK));
            }
            break;
        }
}

// --- The render-only frame's step -------------------------------------------------------------------------------------
// The owed half step of one actor (FLAG_STEP_MOTION already set), with every safety net around it. "Clock only" below
// = Ch_StepMotion(actor, 0): the clock advances but the pose stays at tick k. Returns false on a crash (walk stops).
static bool render_only_step(int i, uint8_t* a, uint32_t fl) {
    int32_t c0 = *(int32_t*)(a + OFF_MOTION_CLOCK);
    int kind = 0;
    int32_t cs = c0;
    bool keyed = (fl & FLAG_SCRIPTED) == 0 && *(uint32_t*)(a + OFF_MOTION_SEQ) != 0;   // the script paths wrap on every call
    if (g_opt_loopguard && keyed) cs = guard_clock(a, c0, &kind);
    // LOOPEND: a half-frame sample inside the last frame of a loop reads past the loop end (bad pose); stock samples
    // there only at whole frames, so hold the pose for that one frame.
    if (g_opt_loopend && kind <= 1 && keyed && (cs & 0xFF) != 0 && *(int16_t*)(a + OFF_MOTION_LOOPS) != 1 &&
        *(int32_t*)(a + OFF_MOTION_LOOP_LEN) > 0 && (cs >> 8) >= (*(int32_t*)(a + OFF_MOTION_END) >> 8)) {
        if (!safe_step(a, 0)) return false;               // clock only: pose stays at tick k
        InterlockedIncrement(&g_loopend_holds);
        if (g_opt_endextrap) __try { end_extrapolate(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return true;
    }
    if (kind == 3) {
        if (!safe_step(a, 0)) return false;               // clock only: pose stays at tick k
        InterlockedIncrement(&g_guard_hold);
        return true;
    }
    if (kind) *(int32_t*)(a + OFF_MOTION_CLOCK) = cs;
    int nb = 0, kh = 0;
    __try { nb = sanity_save(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) { nb = 0; }
    __try { kh = keyhold_save(a); } __except (EXCEPTION_EXECUTE_HANDLER) { kh = 0; }
    bool ok = safe_step(a, 1);
    if (ok && kh) __try { keyhold_apply(i, a, kh); } __except (EXCEPTION_EXECUTE_HANDLER) { fault("keyhold", GetExceptionCode()); }
    if (ok && nb) __try { sanity_check(i, a, nb, c0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (ok && nb) __try { midcheck_after_step(i, a, nb); } __except (EXCEPTION_EXECUTE_HANDLER) { fault("midcheck", GetExceptionCode()); }
    // ENDEXTRAP-ONCE: a play-once motion clamped at its last frame (kind 2) would repeat that pose on the render-only
    // frame. Walks and runs are chains of play-once strides, so every stride end hitched; extrapolate instead, but only
    // when the logic tick already showed the last frame. (The FFX mod has this off: one hit motion's end overshot.
    // Set X2-SMOOTH-003-ENDEXTRAP-ONCE=0 if a motion end overshoots here.)
    if (ok && kind == 2 && g_opt_endextrap && g_opt_endextrap_once && c0 >= (*(int32_t*)(a + OFF_MOTION_END) & ~0xFF))
        __try { end_extrapolate(i, a); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (kind) {
        // Unwrapped clock + the half step just added: the next logic tick wraps / counts down exactly as stock.
        *(int32_t*)(a + OFF_MOTION_CLOCK) = c0 + (*(int32_t*)(a + OFF_MOTION_CLOCK) - cs);
        InterlockedIncrement(kind == 2 ? &g_guard_clamp : &g_guard_wrap);
    }
    return ok;
}

// --- The render-only walk -----------------------------------------------------------------------------------------------
// Does for the actors that owe a half step what the game's actor update (Ch_UpdateAllActors, gva 0x6E10A0) does on a
// logic frame: step each one, then hand the skeleton rebuilds to the worker threads in batches of 150 bones.
static void render_only_walk() {
    uint8_t* base = *g_actor_table;
    int n = *g_actor_count;
    if (!base || n <= 0 || base != g_owed_table) return;
    if (n > MAX_ACTORS) n = MAX_ACTORS;
    if (!safe_sync()) return;
    if (g_opt_hold) {
        g_held_table = base;
        g_held_count = n;
        for (int i = 0; i < n; i++) {
            uint8_t* a = base + i * ACTOR_SIZE;
            if (a[OFF_ACTIVE] == 0 || (a[OFF_RANGE_SKIP] != 0 && a[OFF_RANGE_KEEP] == 0)) continue;
            bool child = *(uint32_t*)(a + OFF_PARENT) != 0;
            if (!child) hold_save(i, a, true);                  // the skeleton jobs touch every root actor in range
            else if (g_owes_half_step[i]) hold_save(i, a, false);         // attached actors: only what the step writes
            if (g_opt_damphold && g_owes_half_step[i]) damp_save(i, a);
        }
    }
    uint8_t* cursor = base;
    int span = 0, bones = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * ACTOR_SIZE;
        span++;
        if (a[OFF_ACTIVE] != 0 && (a[OFF_RANGE_SKIP] == 0 || a[OFF_RANGE_KEEP] != 0) && g_owes_half_step[i]) {
            g_owes_half_step[i] = 0;
            uint32_t fl = *(uint32_t*)(a + OFF_FLAGS);
            if (g_owed_actor_id[i] != *(uint16_t*)a) goto next;                     // slot reused: drop the stale debt
            if (g_opt_shortloop && short_loop(a)) { g_owes_half_step[i] = 1; goto next; }  // stock from the next logic step
            if (stock_id(a)) { g_owes_half_step[i] = 1; goto next; }                    // bisect switch, stock from the next logic step
            if (g_opt_phase && *(int32_t*)(a + OFF_MOTION_CLOCK) != g_clock_after_step[i]) {      // motion restarted after the step
                LONG c = InterlockedIncrement(&g_phase_skips);
                if (c <= 8)
                    log_line("X2-SMOOTH-003-PHASE: slot=%d id=0x%04X clock %d -> %d after its logic step (new motion) - no half step this frame",
                             i, *(uint16_t*)a, g_clock_after_step[i], *(int32_t*)(a + OFF_MOTION_CLOCK));
                goto next;
            }
            *(uint32_t*)(a + OFF_FLAGS) = fl | FLAG_STEP_MOTION;
            if (!render_only_step(i, a, fl)) return;
            g_walked_now[i] = 1;
            g_walk_ran = true;
            InterlockedIncrement(&g_stepped);
            if (*(uint32_t*)(a + OFF_PARENT) == 0) {
                uint8_t* hdr = *(uint8_t**)(a + OFF_MODEL_HEADER);
                if (hdr) bones += *(uint16_t*)(hdr + MODEL_HEADER_BONE_COUNT);
            }
        }
    next:
        if (bones >= SKELETON_BATCH_BONES) {
            if (!safe_dispatch(cursor, span)) return;
            cursor = a + ACTOR_SIZE;
            span = 0;
            bones = 0;
        }
    }
    if (bones != 0) safe_dispatch(cursor, span);
}

// --- PEEL: a dressphere change runs as stock --------------------------------------------------------------------------
// In a dressphere change the old outfit "peels" off the new one: two models drawn on top of each other. With our half
// step they came apart, so for the whole change this row runs as if it were off (stock motion, no half step).
// How the change is found: one of its effect DLLs loads (X2-SMOOTH-003-PEEL-DLL, default "936 937 938"), or the effect
// script runs op 0x116 (the silhouette copy). It ends when that DLL has unloaded and the one loaded right after it
// (within 120 invocations) has unloaded too; each phase gives up after 1800 invocations.
//   PEEL-SKEL (default on): with the walk off, the skeletons were no longer rebuilt on render-only frames and the
//     change's own models drew a stale copy (a blink). This rebuilds them anyway, with the pose held at tick k.
//   PEEL-DRAW (test, off): only re-sets FLAG_STEP_MOTION on render-only frames, without the rebuild.
// More: re/ledger/XFADE-BLINK.md.
static int g_opt_peel = 1;
static int g_opt_peel_draw = 0;                      // X2-SMOOTH-003-PEEL-DRAW
static int g_opt_peel_skel = 1;                      // X2-SMOOTH-003-PEEL-SKEL
static volatile uint32_t g_inv = 0;                  // invocation counter (pre)
// 0 = no change; 1 = change seen (DLL loaded or op 0x116); 2 = that DLL unloaded, waiting for the next one;
// 3 = the next DLL is loaded (g_peel_tail), stock until it unloads
static volatile LONG g_peel_phase = 0;
static volatile uint32_t g_peel_phase_inv = 0;       // invocation the current phase began at
static uint8_t* volatile g_peel_tail = nullptr;
static int g_peel_dll[8], g_npeel_dll = 0;
typedef void* (__cdecl* opu_op_fn)(uint8_t* scr, void* pc);
static opu_op_fn g_op116 = nullptr;                  // the game's op 0x116 (we sit in its slot of the op table)
static volatile LONG g_peel_invs = 0;

static int magic_number(const char* name) {      // "magic_0937.dll" -> 937
    const char* p = strstr(name, "magic_");
    return p ? atoi(p + 6) : -1;
}

static void* __cdecl hook_op116(uint8_t* scr, void* pc) {
    if (g_opt_peel) {
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

// True while a dressphere change is running (the phase ends by itself after its time limit).
static bool peel_hold() {
    if (!g_opt_peel) return false;
    LONG ph = g_peel_phase;
    if (ph == 0) return false;
    uint32_t cap = ph == 2 ? 120 : 1800;
    if (g_inv - g_peel_phase_inv <= cap) return true;
    g_peel_phase = 0;
    return false;
}

// 60 Hz limbs right now: not switched off by F6/F7 and no dressphere change running.
static bool limbs_live() { return smooth_live(3) && !peel_hold(); }

static void install_peel() {
    g_opt_peel = config_row("X2-SMOOTH-003-PEEL", 1);
    g_opt_peel_draw = config_row("X2-SMOOTH-003-PEEL-DRAW", 0);
    g_opt_peel_skel = config_row("X2-SMOOTH-003-PEEL-SKEL", 1);
    log_line("  X2-SMOOTH-003-PEEL-SKEL: %s", g_opt_peel_skel ? "on (render-only skeleton jobs, held pose)" : "off");
    log_line("  X2-SMOOTH-003-PEEL-DRAW: %s", g_opt_peel_draw ? "on (render-only loop 3 for the peel's actors)" : "off");
    if (!g_opt_peel) { log_line("  X2-SMOOTH-003-PEEL: off (ini)"); return; }
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
        g_op116 = (opu_op_fn)fn;
        if (!patch_u32(entry, (uint32_t)(uintptr_t)&hook_op116)) g_op116 = nullptr;
    }
    char line[64]; int k = 0; line[0] = 0;
    for (int i = 0; i < g_npeel_dll; i++) k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %04d", g_peel_dll[i]);
    log_line("  X2-SMOOTH-003-PEEL: change DLLs:%s; op 0x116 entry gva=0x%08X (0x00D6F810) -> %s", line,
             entry ? rt_to_gva(entry) : 0, g_op116 ? "stub" : "untouched");
}

// PEEL-SKEL / PEEL-DRAW during a change. Logic frame: remember which actors the game flagged for a step. Render-only
// frame: flag the same actors again (no step, so the pose stays at tick k) and, with PEEL-SKEL, rebuild their
// skeletons as the walk would.
static uint16_t g_step_id[MAX_ACTORS];                 // id of actors with FLAG_STEP_MOTION at the last logic Ch_NoSkelPass, 0 = none
static volatile LONG g_peel_draw_n = 0;
static void peel_draw(bool logic) {
    uint8_t* base = *g_actor_table;
    int n = *g_actor_count;
    if (!base || n <= 0) return;
    if (n > MAX_ACTORS) n = MAX_ACTORS;
    if (!logic && g_opt_peel_skel && !safe_sync()) return;
    uint8_t* cursor = base;
    int span = 0, bones = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * ACTOR_SIZE;
        uint32_t fl = *(uint32_t*)(a + OFF_FLAGS);
        if (logic) { g_step_id[i] = (a[OFF_ACTIVE] && (fl & FLAG_STEP_MOTION)) ? *(uint16_t*)a : 0; continue; }
        span++;
        if (g_step_id[i] && a[OFF_ACTIVE] && *(uint16_t*)a == g_step_id[i] && !(fl & FLAG_STEP_MOTION)) {
            *(uint32_t*)(a + OFF_FLAGS) = fl | FLAG_STEP_MOTION;
            InterlockedIncrement(&g_peel_draw_n);
            if (*(uint32_t*)(a + OFF_PARENT) == 0) {
                uint8_t* hdr = *(uint8_t**)(a + OFF_MODEL_HEADER);
                if (hdr) bones += *(uint16_t*)(hdr + MODEL_HEADER_BONE_COUNT);
            }
        }
        if (g_opt_peel_skel && bones >= SKELETON_BATCH_BONES) {
            if (!safe_dispatch(cursor, span)) return;
            cursor = a + ACTOR_SIZE;
            span = 0;
            bones = 0;
        }
    }
    if (g_opt_peel_skel && bones != 0) safe_dispatch(cursor, span);
}

// Replaces the main loop's call of Ch_NoSkelPass (CALL at gva 0x60586C -> 0x6E1610), which runs every frame after the
// actor update and before drawing: the place where the render-only walk runs.
static void __cdecl hook_noskel_pass() {
    if ((g_opt_peel_draw || g_opt_peel_skel) && row_SMOOTH003.on && !g_disabled && (g_inv_kind == INV_LOGIC || g_inv_kind == INV_RENDER_ONLY)) {
        static bool was = false;
        bool hold = peel_hold();
        if (hold) __try { peel_draw(g_inv_kind == INV_LOGIC); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (hold != was) log_line("X2-SMOOTH-003-PEEL-DRAW/SKEL: %s (%ld actor flags set so far)", hold ? "on" : "off",
                                  (LONG)g_peel_draw_n);
        was = hold;
    }
    if (row_SMOOTH003.on && !g_disabled && g_inv_kind == INV_RENDER_ONLY && limbs_live() && *g_actors_frozen == 0) {
        g_walks++;
        LONGLONG t0 = perf_on() ? perf_now() : 0;
        __try { render_only_walk(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { fault("walk", GetExceptionCode()); }
        if (t0) perf_walk(perf_now() - t0);
    }
    g_noskel_pass();
}

// --- CLOTHHOLD: hair and cloth (runs on the worker threads) --------------------------------------------------------
// Modes are described at g_opt_cloth. For modes 3 and 4 we keep, per cloth mesh, the vertex positions of the last two
// cloth job results (a "snap"), and redeform the mesh from a blend of them.
static const uint32_t OFF_MESH_ENTRIES = 0x1C8, OFF_CLOTH_MODEL = 0x1CC, OFF_RESET_COUNT = 0x824, MESH_ENTRY_SIZE = 0x38;
static void (__cdecl* g_cloth_deform)(uint8_t*) = nullptr;   // the game's Ch_ClothDeform(actor)
static volatile int32_t* g_cloth_deform_gate = nullptr;      // the game deforms only while this is 0
static int g_opt_cloth_extrap_pct = 50;                    // X2-SMOOTH-003-CLOTHEXTRAP-PCT (mode 3)
static volatile LONG g_logic_tick = 1;               // logic invocations (game thread writes, jobs read)
static volatile LONG g_extrap_n = 0, g_extrap_miss = 0;

// One cloth mesh: its vertex array in the game (vtex, n vertices of 4 floats), and our copies of the latest (cur, from
// logic tick ncur) and previous (prev, tick nprev; 0 = none) job results.
struct ClothSnap { float* vtex; uint8_t* actor; uint32_t n; LONG ncur, nprev; float* cur; float* prev;
                   float typical_move; };   // running average of the largest vertex move per tick (jump log)
static volatile LONG g_cloth_jump_lines = 0, g_cloth_jumps = 0;
enum { MAX_CLOTH_SNAPS = 128 };
static ClothSnap g_cloth_snaps[MAX_CLOTH_SNAPS];
static thread_local uint16_t tl_deform_actor_id = 0;       // MESHCHECK: actor whose cloth this thread is deforming
static thread_local char tl_deform_by = '?';               // MESHCHECK: who deforms: 'G' the game's job, 'C' ours at the
                                                           // latest result, 'H' ours half way back, 'X' ours other
static SRWLOCK g_cloth_snaps_lock = SRWLOCK_INIT;

// Finds (or, with create, makes) the snap of a mesh. Jobs run on several worker threads at once, hence the lock. A new
// snap only takes a free entry or one unused for 4+ logic ticks (in FFX, reusing a fresh one mid-copy corrupted the
// heap).
static ClothSnap* snap_get(float* vtex, uint8_t* actor, uint32_t n, bool create) {
    AcquireSRWLockExclusive(&g_cloth_snaps_lock);
    ClothSnap* s = nullptr; ClothSnap* old = nullptr;
    LONG now = g_logic_tick;
    for (int i = 0; i < MAX_CLOTH_SNAPS; i++) {
        ClothSnap* e = &g_cloth_snaps[i];
        if (e->vtex == vtex) { s = e; break; }
        if (!e->vtex) { if (!old || old->vtex) old = e; }
        else if (e->ncur < now - 4 && (!old || (old->vtex && e->ncur < old->ncur))) old = e;
    }
    if (!s && create && old) { s = old; s->vtex = vtex; s->ncur = now; s->nprev = 0; s->actor = nullptr; }
    if (s && create && (s->n != n || s->actor != actor)) {
        free(s->cur); free(s->prev);
        s->cur = (float*)malloc(n * 16); s->prev = (float*)malloc(n * 16);
        s->n = (s->cur && s->prev) ? n : 0; s->actor = actor; s->ncur = s->nprev = 0; s->typical_move = 0;
    }
    ReleaseSRWLockExclusive(&g_cloth_snaps_lock);
    return s;
}

// Calls fn(entry, vertex array, vertex count) for each dynamic mesh of the actor.
template <class F> static void for_dyn_mesh(uint8_t* a, F fn) {
    uint8_t* ents = *(uint8_t**)(a + OFF_MESH_ENTRIES);
    uint8_t* mdl = *(uint8_t**)(a + OFF_CLOTH_MODEL);
    if (!ents || !mdl) return;
    uint32_t ne = *(uint16_t*)(mdl + 6);
    for (uint32_t e = 0; e < ne; e++) {
        uint8_t* en = ents + e * MESH_ENTRY_SIZE;
        uint8_t* desc = *(uint8_t**)en;
        float* vtex = *(float**)(en + 8);
        if (!desc || !vtex || (!*(void**)(en + 0x2C) && !*(void**)(en + 0x0C))) continue;
        uint32_t n = *(uint16_t*)(desc + 4);
        if (n) fn(en, vtex, n);
    }
}

// After a logic cloth job: the job's result becomes "cur", the old cur becomes "prev" (none after a reset/teleport).
static void cloth_snap_after_step(uint8_t* a, bool reset) {
    for_dyn_mesh(a, [&](uint8_t*, float* vtex, uint32_t n) {
        ClothSnap* s = snap_get(vtex, a, n, true);
        if (!s || !s->n) return;
        float* t = s->prev; s->prev = s->cur; s->cur = t;
        s->nprev = reset ? 0 : s->ncur;
        memcpy(s->cur, vtex, n * 16);
        // Jump log (diagnostic): the largest vertex move of this tick against the usual one. A blend across a jump
        // draws the mesh half way between two far shapes.
        if (s->nprev && s->nprev == s->ncur) {
            float big = 0;
            for (uint32_t v = 0; v < n * 4; v += 4) {
                float dx = s->cur[v] - s->prev[v], dy = s->cur[v + 1] - s->prev[v + 1], dz = s->cur[v + 2] - s->prev[v + 2];
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > big) big = d2;
            }
            big = sqrtf(big);
            if (s->typical_move > 0 && big > 4.0f * s->typical_move + 1.0f) {
                InterlockedIncrement(&g_cloth_jumps);
                if (g_cloth_jump_lines < 40) {
                    InterlockedIncrement(&g_cloth_jump_lines);
                    log_line("X2-SMOOTH-003-CLOTHHOLD: id 0x%04X cloth mesh (%u vertices) jumped %.2f in one tick (usual %.2f)",
                             *(const uint16_t*)a, n, big, s->typical_move);
                }
            }
            s->typical_move = s->typical_move > 0 ? 0.9f * s->typical_move + 0.1f * big : big;
        } else s->typical_move = 0;
        s->ncur = g_logic_tick;
    });
}

// Deforms the actor's cloth from cur + k * (cur - prev) (k = -0.5: half way back to prev; 0: cur; > 0: extrapolated),
// then puts the job's real result back into the vertex arrays. Only when every mesh has a result from this logic tick
// (and from the one before, if need_prev); returns false otherwise.
static bool cloth_extrap(uint8_t* a, float k, bool need_prev) {
    LONG now = g_logic_tick;
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
    tl_deform_actor_id = *(const uint16_t*)a;
    tl_deform_by = k == 0.0f ? 'C' : k == -0.5f ? 'H' : 'X';
    if (*g_cloth_deform_gate == 0) g_cloth_deform(a);
    tl_deform_by = '?';
    for_dyn_mesh(a, [&](uint8_t*, float* vtex, uint32_t n) {
        ClothSnap* s = snap_get(vtex, a, n, false);
        memcpy(vtex, s->cur, n * 16);
    });
    return true;
}

static volatile LONG g_cloth_job_calls[MAX_ACTORS];         // FREEZEDIAG: cloth job entries per slot (worker threads)
static bool g_freezediag_running = false;
// CLOTHHOLD-MIXED (default on): a render-only cloth job whose batch holds an actor that was not walked used to run the
// game's job for the whole batch: a second simulation step in the same tick, drawn without the blend. With this on,
// render-only frames never run the job: walked actors are drawn from the snaps, the others keep their last shape.
static int g_opt_cloth_mixed = 1;
static volatile LONG g_cloth_mixed = 0, g_cloth_mixed_lines = 0;

// Replaces the entry of the game's cloth job Ch_ClothJob(actors, count) (worker threads). Render-only frame where every
// actor in the batch was walked: draw the cloth from the snaps (modes 3/4) or skip it (mode 2). Logic frame: run the
// job, record the result and, in mode 4, redeform half way back to the previous result.
static void __cdecl hook_cloth_job(uint8_t** list, int* count) {
    bool live = !g_disabled && row_SMOOTH003.on && limbs_live();
    if (g_freezediag_running) __try {
        for (int k = 0; k < *count; k++) {
            int i = slot_of(list[k], *g_actor_table, *g_actor_count);
            if (i >= 0 && i < MAX_ACTORS) InterlockedIncrement(&g_cloth_job_calls[i]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    bool render_only_frame = g_inv_kind == INV_RENDER_ONLY;
    if (g_opt_cloth >= 3 && g_opt_cloth_mixed && live && render_only_frame) {
        // Render-only: draw walked actors from the snaps, leave the others as they are; never step the simulation.
        int walked = 0, others = 0;
        uint16_t other_id = 0;
        __try {
            for (int k = 0; k < *count; k++) {
                int i = slot_of(list[k], *g_actor_table, *g_actor_count);
                if (i >= 0 && g_walked_now[i]) {
                    walked++;
                    if (cloth_extrap(list[k], 0.0f, false)) {
                        if (InterlockedIncrement(&g_extrap_n) == 1)
                            log_line("X2-SMOOTH-003-CLOTHHOLD: first cloth mesh drawn from the job results on a render-only invocation (mode %d, id 0x%04X)",
                                     g_opt_cloth, *(uint16_t*)list[k]);
                    } else InterlockedIncrement(&g_extrap_miss);
                } else { others++; other_id = *(uint16_t*)list[k]; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { fault("cloth render-only", GetExceptionCode()); }
        if (others) {
            InterlockedIncrement(&g_cloth_mixed);
            if (g_cloth_mixed_lines < 8) {
                InterlockedIncrement(&g_cloth_mixed_lines);
                log_line("X2-SMOOTH-003-CLOTHHOLD-MIXED: render-only cloth job with %d walked and %d not walked actor(s) "
                         "(e.g. id 0x%04X, walk %s) - not stepped", walked, others, other_id, g_walk_ran ? "ran" : "did not run");
            }
        }
        return;
    }
    if (g_opt_cloth >= 2 && g_walk_ran && live) {
        bool all = true;
        __try {
            for (int k = 0; k < *count && all; k++) {
                int i = slot_of(list[k], *g_actor_table, *g_actor_count);
                all = i >= 0 && g_walked_now[i];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { all = false; }
        if (all) {
            if (g_opt_cloth >= 3) {
                __try {
                    for (int k = 0; k < *count; k++) {
                        if (g_opt_cloth == 3 ? cloth_extrap(list[k], g_opt_cloth_extrap_pct / 100.0f, true) : cloth_extrap(list[k], 0.0f, false)) {
                            LONG c = InterlockedIncrement(&g_extrap_n);
                            if (c == 1) log_line("X2-SMOOTH-003-CLOTHHOLD: first cloth mesh drawn from the job results on a render-only invocation (mode %d, id 0x%04X)",
                                                 g_opt_cloth, *(uint16_t*)list[k]);
                        } else InterlockedIncrement(&g_extrap_miss);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { fault("cloth render-only", GetExceptionCode()); }
            } else if (InterlockedIncrement(&g_cloth_skipped) == 1)
                log_line("X2-SMOOTH-003-CLOTHHOLD: first cloth job skipped on a render-only invocation (mode 2)");
            return;
        }
    }
    if (g_opt_cloth < 3) { g_cloth_job_orig(list, count); return; }
    // Reset/teleport (+0x824 counter, +0x1A0 bit 0x800000) or an uninitialised mesh: no blend from it.
    bool reset[8] = {};
    int n = *count < 8 ? *count : 8;
    __try {
        for (int k = 0; k < n; k++) {
            uint8_t* a = list[k];
            reset[k] = *(uint16_t*)(a + OFF_RESET_COUNT) != 0 || (*(uint32_t*)(a + OFF_FLAGS) & 0x800000) != 0;
            for_dyn_mesh(a, [&](uint8_t* en, float*, uint32_t) { if (*(void**)(en + 0x0C) && !*(int*)(en + 0x34)) reset[k] = true; });
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    tl_deform_actor_id = *count > 0 ? *(const uint16_t*)list[0] : 0;
    tl_deform_by = 'G';
    g_cloth_job_orig(list, count);
    tl_deform_by = '?';
    __try {
        for (int k = 0; k < n; k++) {
            cloth_snap_after_step(list[k], reset[k]);
            if (g_opt_cloth == 4 && !reset[k] && live) cloth_extrap(list[k], -0.5f, true);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("cloth snap", GetExceptionCode()); }
}

static void install_meshcheck();                            // MESHCHECK, below

// --- Patching (game thread, first pre()) --------------------------------------------------------------------------------
// Points the four game CALLs at our hooks (without GATES only the step and the Ch_NoSkelPass call) and hooks the cloth
// job. If one CALL patch fails, the ones done are undone and the row stays off.
static bool apply_patches() {
    uint8_t* sites[4] = { g_callsite_step, g_callsite_noskel, g_callsite_seq, g_callsite_motev };
    void* olds[4] = { (void*)g_step_motion, (void*)g_noskel_pass, (void*)g_seq_frame, (void*)g_motion_events };
    void* news[4] = { (void*)&hook_step_motion, (void*)&hook_noskel_pass, (void*)&hook_seq_frame, (void*)&hook_motion_events };
    int count = g_opt_gates ? 4 : 2;
    for (int i = 0; i < count; i++) {
        if (!patch_call_rel32(sites[i], olds[i], news[i])) {
            for (int j = 0; j < i; j++) patch_call_rel32(sites[j], news[j], olds[j]);
            log_line("X2-SMOOTH-003: CALL patch %d failed - row off", i);
            return false;
        }
    }
    if (g_opt_cloth >= 3) install_meshcheck();
    if (g_opt_cloth >= 2) {
        if (hook_entry("X2-SMOOTH-003-CLOTHHOLD", g_cloth_job, (void*)&hook_cloth_job, (void**)&g_cloth_job_orig))
            log_line("X2-SMOOTH-003-CLOTHHOLD: Ch_ClothJob gva=0x%08X hooked (mode %d: %s)", rt_to_gva(g_cloth_job), g_opt_cloth,
                     g_opt_cloth == 4 ? "interpolated half a tick behind" : g_opt_cloth == 3 ? "extrapolated" : "held, 30 Hz");
        else { g_opt_cloth = 0; log_line("X2-SMOOTH-003-CLOTHHOLD: hook failed - off"); }
    }
    log_line("X2-SMOOTH-003: %d CALLs retargeted on the game thread", count);
    return true;
}

// --- FREEZEDIAG: diagnostic, off by default ---------------------------------------------------------------------------
// Finds actors whose pose stops changing (written for a model that froze for half a second, ledger PORT-SMOOTH3.md).
// After every invocation each active root actor's bone angles, cloth vertices and root matrix are hashed. When none of
// them changed for FREEZE_RUN invocations, one line is logged with the actor's motion fields and this row's state,
// plus the last FREEZE_HISTORY invocations; and again when it moves. Observe only.
static int g_opt_freezediag = 0;
enum { FREEZE_RUN = 12, FREEZE_HISTORY = 16 };
// One invocation of one actor: kind (L/R/B/S), walked/owed/held, which hashes changed (bones 1, cloth 2, root 4) and
// how many cloth jobs ran for it.
struct FreezeHist { char kind; uint8_t walked, debt, held, chg, jobs; int32_t clock; uint32_t flags; };
struct FreezeState { uint16_t id; uint8_t logged; uint8_t pos; uint32_t hb, hc, hr; int run; LONG jobs; FreezeHist h[FREEZE_HISTORY]; };
static FreezeState* g_freeze_state = nullptr;              // MAX_ACTORS
static LONG g_freeze_inv = 0;
static const uint32_t ROOT = 0x1DC;              // root matrix (X2-SMOOTH-002)
// FREEZEDIAG-ID / -ID-END (decimal actor ids): log every invocation of these actors while a spell DLL is loaded.
static int g_trace_id = 0, g_trace_id_end = 0;
// FREEZEDIAG-ALWAYS: trace them always, not only during spells. Each trace line also gives the largest bone angle
// change since the previous traced invocation (radians) and how many rotations changed by more than 0.5 rad.
static int g_trace_always = 0;
static float* g_trace_prev[MAX_ACTORS];
static volatile LONG g_magic_loaded = 0;
static void fd_on_load(uint8_t*, uint32_t, const char*) { InterlockedIncrement(&g_magic_loaded); }
static void fd_on_unload(uint8_t*) { InterlockedDecrement(&g_magic_loaded); }

static uint32_t fnv1a(uint32_t h, const void* p, size_t n) {
    const uint32_t* w = (const uint32_t*)p;
    for (size_t i = 0; i < n / 4; i++) { h ^= w[i]; h *= 16777619u; }
    return h;
}

static void freeze_diag() {
    uint8_t* base = *g_actor_table;
    int n = *g_actor_count;
    if (!base || n <= 0) return;
    if (n > MAX_ACTORS) n = MAX_ACTORS;
    char kind = "LRBS"[g_inv_kind & 3];
    InterlockedIncrement(&g_freeze_inv);
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + i * ACTOR_SIZE;
        FreezeState& s = g_freeze_state[i];
        if (a[OFF_ACTIVE] == 0 || *(uint32_t*)(a + OFF_PARENT) != 0) { s.id = 0; continue; }
        int nb = bone_count(a);
        const uint8_t* bones = *(uint8_t* const*)(a + OFF_BONES);
        if (!nb || !bones) { s.id = 0; continue; }
        uint32_t hb = 2166136261u, hc = 2166136261u, hr = fnv1a(2166136261u, a + ROOT, 64);
        for (int b = 0; b < nb; b++) hb = fnv1a(hb, bones + b * BONE_SIZE + BONE_ANGLE[0], 12);
        for_dyn_mesh(a, [&](uint8_t*, float* vtex, uint32_t nv) { hc = fnv1a(hc, vtex, (nv < 512 ? nv : 512) * 16); });
        LONG jobs = g_cloth_job_calls[i];
        uint16_t id = *(const uint16_t*)a;
        uint8_t chg = (hb != s.hb ? 1 : 0) | (hc != s.hc ? 2 : 0) | (hr != s.hr ? 4 : 0);
        FreezeHist& e = s.h[s.pos++ % FREEZE_HISTORY];
        e.kind = kind; e.walked = g_walked_now[i]; e.debt = g_owes_half_step[i]; e.held = g_held_kind[i];
        e.clock = *(int32_t*)(a + OFF_MOTION_CLOCK); e.flags = *(uint32_t*)(a + OFF_FLAGS);
        e.chg = chg; e.jobs = (uint8_t)(jobs - s.jobs);
        s.jobs = jobs; s.hb = hb; s.hc = hc; s.hr = hr;
        if ((g_magic_loaded > 0 || g_trace_always) && id >= g_trace_id && id <= g_trace_id_end) {
            const float* r = (const float*)(a + ROOT);
            float maxd = 0; int maxb = -1, maxc = 0, big = 0;
            if (!g_trace_prev[i]) g_trace_prev[i] = (float*)calloc((size_t)MAX_BONES * 3 + 1, sizeof(float));
            float* pv = g_trace_prev[i];
            if (pv) {
                bool have = pv[MAX_BONES * 3] == (float)id;
                for (int b = 0; b < nb; b++)
                    for (int c = 0; c < 3; c++) {
                        float v = *(const float*)(bones + b * BONE_SIZE + BONE_ANGLE[c]);
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
                pv[MAX_BONES * 3] = (float)id;
            }
            log_line("X2-SMOOTH-003-FREEZEDIAG trace: inv %ld %c slot %d id 0x%04X maxd %.3f bone %d ch %d big %d | chg %d jobs %d wdh %d%d%d clock 0x%X seq 0x%X flags 0x%08X "
                     "root (%.2f, %.2f, %.2f) fwd (%.3f, %.3f) start 0x%X end 0x%X len 0x%X loops %d short %d | stock %ld lend %ld wrap %ld clamp %ld ghold %ld",
                     g_freeze_inv, kind, i, id, maxd, maxb, maxc, big, chg, e.jobs, e.walked, e.debt, e.held, e.clock, *(uint32_t*)(a + OFF_MOTION_SEQ), e.flags,
                     r[12], r[13], r[14], r[8], r[10], *(int32_t*)(a + OFF_MOTION_START), *(int32_t*)(a + OFF_MOTION_END), *(int32_t*)(a + OFF_MOTION_LOOP_LEN),
                     (int)*(int16_t*)(a + OFF_MOTION_LOOPS), short_loop(a), g_stock_steps, g_loopend_holds, g_guard_wrap, g_guard_clamp, g_guard_hold);
        }
        if (s.id != id) { s.id = id; s.run = 0; s.logged = 0; continue; }
        if (chg == 0) {
            if (++s.run == FREEZE_RUN && !s.logged) {
                s.logged = 1;
                log_line("X2-SMOOTH-003-FREEZEDIAG: pose still %d inv (inv %ld %c) slot %d id 0x%04X bones %d | clock 0x%X seq 0x%X "
                         "start 0x%X end 0x%X len 0x%X loops %d flags 0x%08X | shortloop %d stockid %d freeze %d half %d live %d",
                         s.run, g_freeze_inv, kind, i, id, nb, *(int32_t*)(a + OFF_MOTION_CLOCK), *(uint32_t*)(a + OFF_MOTION_SEQ),
                         *(int32_t*)(a + OFF_MOTION_START), *(int32_t*)(a + OFF_MOTION_END), *(int32_t*)(a + OFF_MOTION_LOOP_LEN), (int)*(int16_t*)(a + OFF_MOTION_LOOPS),
                         *(uint32_t*)(a + OFF_FLAGS), short_loop(a), stock_id(a), (int)*g_actors_frozen, *g_half_rate_switch, smooth_live(3));
                char line[900];
                int k = _snprintf_s(line, sizeof(line), _TRUNCATE,
                                    "X2-SMOOTH-003-FREEZEDIAG:   last %d (kind clock flags walked/debt/held changed(b1 c2 r4) jobs):", FREEZE_HISTORY);
                for (int j = 0; j < FREEZE_HISTORY && k > 0; j++) {
                    const FreezeHist& f = s.h[(s.pos + j) % FREEZE_HISTORY];
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
                         s.run, g_freeze_inv, kind, chg, e.jobs, i, id, *(int32_t*)(a + OFF_MOTION_CLOCK), *(uint32_t*)(a + OFF_MOTION_SEQ),
                         *(int32_t*)(a + OFF_MOTION_START), *(int32_t*)(a + OFF_MOTION_END), *(int32_t*)(a + OFF_MOTION_LOOP_LEN), (int)*(int16_t*)(a + OFF_MOTION_LOOPS),
                         *(uint32_t*)(a + OFF_FLAGS));
            s.run = 0; s.logged = 0;
        }
    }
}

// --- Every invocation -----------------------------------------------------------------------------------------------------
// Before: put HOLD bytes back from the last walk, then switch the half-rate motion on for this invocation if 60 Hz limbs
// are live (logic and render-only invocations), off otherwise.
static void pre() {
    g_inv++;
    if (!g_calls_patched) {
        g_calls_patched = true;
        if (!apply_patches()) { row_SMOOTH003.on = 0; return; }
    }
    smooth_poll_hotkey();
    if (g_held_table) {
        __try { hold_restore(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_held_table = nullptr; fault("hold restore", GetExceptionCode()); }
    }
    g_inv_kind = g_disabled ? INV_STOCK : pacing_kind();
    if (g_inv_kind != INV_RENDER_ONLY) InterlockedIncrement(&g_logic_tick);
    bool want = !g_disabled && row_SMOOTH003.on && limbs_live() && (g_inv_kind == INV_LOGIC || g_inv_kind == INV_RENDER_ONLY);
    if (peel_hold()) InterlockedIncrement(&g_peel_invs);
    *g_half_rate_switch = want ? 1 : 0;
    g_we_set_half_rate = want;
    g_walk_ran = false;
    memset((void*)g_walked_now, 0, sizeof(g_walked_now));
}

// --- DRAWCHECK: which bones flicker as drawn (diagnostic) -------------------------------------------------------------
// After every drawn invocation (logic or render-only; backlog frames are not drawn) the bone angles of each active actor
// are kept for the last three drawn frames. For the middle frame, a bone whose rotation is far from the half-way point
// of the frames on both sides ("odd one out") is counted per bone and frame kind. The game writes some rotations as the
// other equivalent angle triple (x + 1/2, 1/2 - y, z + 1/2 turn: the same rotation), so every triple is first put in the
// form with the middle angle within a quarter turn; a bone near that quarter turn (gimbal lock) is not judged. Every
// DC_REPORT_FRAMES drawn frames the bones odd most often are logged with their counts, plus the first few odd frames
// in full: all three angles of the three frames, the damped-bone flag (bone byte +0x144) and the damping target (+0x114).
// The same is done for where each bone is drawn ("world"): the translation row of its matrix (bone +0x88, built by
// Ch_BuildRootAndBones from scale, rotation and position) relative to bone 0; odd when it is more than
// DC_WORLD_MIN_ODD of its distance to bone 0 away from half way, and further than the two frames are apart.
static const int   DC_ACTORS = 32, DC_BONES = 256;
static const float DC_MIN_ODD = 0.08f;                     // turns
static const float DC_GIMBAL_MARGIN = 0.02f;               // turns: middle angle this close to a quarter turn = not judged
static const int   DC_REPORT_FRAMES = 600;                 // drawn frames (~10 s) per summary
static const int   DC_REPORT_BONES = 12, DC_DETAIL_LINES = 12;
static const int   DC_MAX_FAULTS = 600;                    // faults in a row (~10 s of frames) before giving up
static const uint32_t BONE_WORLD_T = 0x88 + 0x30;          // bone matrix +0x88, translation row
static const float DC_WORLD_MIN_ODD = 0.05f;               // share of the bone's distance to bone 0
static int      g_opt_drawcheck = 1;
static float*   g_dc_ang = nullptr;                        // [3 frames][DC_ACTORS][DC_BONES][3], canonical triples
static uint16_t g_dc_id[3][DC_ACTORS];
static int      g_dc_kind[3];
static int      g_dc_head = 0, g_dc_frames = 0, g_dc_window = 0, g_dc_window_lines = 0;
static uint16_t g_dc_count[DC_ACTORS][DC_BONES][2];        // odd frames this window: [render-only, logic]
static uint16_t g_dc_count_id[DC_ACTORS];                  // actor id the counts belong to
static uint8_t  g_dc_damped[DC_ACTORS][DC_BONES];
static float*   g_dc_pos = nullptr;                        // [3 frames][DC_ACTORS][DC_BONES][3], relative to bone 0
static uint16_t g_dc_wcount[DC_ACTORS][DC_BONES][2];       // world odd frames this window: [render-only, logic]
static volatile LONG g_dc_odd[4] = {}, g_dc_faults = 0;

// --- MESHCHECK: which cloth meshes flicker as drawn (diagnostic, part of DRAWCHECK) -------------------------------------
// Ch_ClothDeform builds each HD mesh vertex buffer from the cloth vertices (each vertex a blend of three, optionally
// pushed out along that triangle's normal), then marks the buffer for upload with a one-line setter (FUN_00514ab0,
// `*flag = value`) on the buffer record + 0x88. Our copy of that setter also keeps the buffer: vertices (4 floats) at
// record + 0x8C, count at record + 0x84. The last build in an invocation is the one drawn; it is kept for the last three
// drawn frames. A vertex of the middle frame is odd when it is further than MESH_MIN_ODD of the mesh size from half way
// between the frames on both sides and further than those two are apart. Counts per actor id and frame kind go into
// the DRAWCHECK summary.
static const uint32_t DEFORM_UPLOAD_CALL = 0x475;          // Ch_ClothDeform + 0x475: CALL FUN_00514ab0 (gva 0x6DB275)
static const uint8_t  DEFORM_UPLOAD_BEFORE[] = { 0x05, 0x88, 0x00, 0x00, 0x00, 0x6A, 0x01, 0x50 };  // ADD EAX,0x88; PUSH 1; PUSH EAX
static const uint8_t  UPLOAD_SETTER_BYTES[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x08, 0x8B, 0x4D, 0x0C, 0x8B, 0x02, 0x89, 0x0A, 0x5D };
static const uint32_t MESH_REC_FLAG = 0x88, MESH_REC_VERTS = 0x8C, MESH_REC_COUNT = 0x84;
static const float    MESH_MIN_ODD = 0.02f;                // share of the mesh size (bounding box diagonal)
static const uint32_t MESH_MAX_VERTS = 16384;
enum { MESH_TRACKS = 64 };
struct MeshTrack {
    float* out;                        // the game's vertex buffer (key)
    uint16_t actor_id; uint32_t n;
    float* frames[3]; int kind[3]; uint32_t inv[3]; int head, count;
    uint8_t builds[3]; char by[8 * 3]; LONG tick[3];   // per frame: builds in the invocation, who built them, logic tick
    LONG odd[2], odd_verts[2]; float worst[2];   // this window: [render-only, logic]
};
static MeshTrack g_mesh[MESH_TRACKS];
static const int MESH_DETAIL_LINES = 12;                   // full lines per summary window
static int g_mesh_lines = 0;
static SRWLOCK g_mesh_lock = SRWLOCK_INIT;
static uint8_t* g_upload_setter = nullptr;


// Middle frame m against f0 and t: odd vertices and the largest offset as a share of the mesh size.
static int mesh_odd(const float* v0, const float* v1, const float* v2, uint32_t n, float* worst_share, uint32_t* worst_vertex = nullptr) {
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t v = 0; v < n; v++)
        for (int c = 0; c < 3; c++) { float x = v1[v * 4 + c]; if (x < lo[c]) lo[c] = x; if (x > hi[c]) hi[c] = x; }
    float size = sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
    if (!(size > 0)) return 0;
    int odd = 0; float worst = 0;
    for (uint32_t v = 0; v < n; v++) {
        float e = 0, span = 0;
        for (int c = 0; c < 3; c++) {
            float a = v0[v * 4 + c], b = v1[v * 4 + c], d = v2[v * 4 + c], mid = 0.5f * (a + d);
            e += (b - mid) * (b - mid); span += (d - a) * (d - a);
        }
        e = sqrtf(e);
        if (e > MESH_MIN_ODD * size && e > sqrtf(span)) {
            odd++;
            if (e / size > worst) { worst = e / size; if (worst_vertex) *worst_vertex = v; }
        }
    }
    *worst_share = worst;
    return odd;
}

static void mesh_record(uint8_t* rec, int kind) {
    float* out = *(float**)(rec + MESH_REC_VERTS);
    uint32_t n = *(uint32_t*)(rec + MESH_REC_COUNT);
    if (!out || !n || n > MESH_MAX_VERTS) return;
    AcquireSRWLockExclusive(&g_mesh_lock);
    MeshTrack* t = nullptr; MeshTrack* free_t = nullptr;
    for (int i = 0; i < MESH_TRACKS && !t; i++) {
        if (g_mesh[i].out == out) t = &g_mesh[i];
        else if (!g_mesh[i].out && !free_t) free_t = &g_mesh[i];
    }
    if (!t && free_t) { t = free_t; t->out = out; t->n = 0; }
    if (t && t->n != n) {
        for (int f = 0; f < 3; f++) { free(t->frames[f]); t->frames[f] = (float*)malloc(n * 16); }
        bool ok = t->frames[0] && t->frames[1] && t->frames[2];
        t->n = ok ? n : 0; t->head = 0; t->count = 0;
    }
    if (t && t->n) {
        t->actor_id = tl_deform_actor_id;
        int last = (t->head + 2) % 3;
        if (t->count && t->inv[last] == g_inv) {                       // rebuilt again: the later one is drawn
            memcpy(t->frames[last], out, n * 16);
            if (t->builds[last] < 8) t->by[last * 8 + t->builds[last]] = tl_deform_by;
            if (t->builds[last] < 255) t->builds[last]++;
        } else {
            // the newest frame is complete: judge the one before it against its neighbours
            if (t->count >= 3) {
                int m = (t->head + 1) % 3, f0 = t->head;
                float worst = 0;
                uint32_t wv = 0;
                int odd = mesh_odd(t->frames[f0], t->frames[m], t->frames[last], n, &worst, &wv);
                if (odd) {
                    int k = t->kind[m] == INV_RENDER_ONLY ? 0 : 1;
                    t->odd[k]++; t->odd_verts[k] += odd;
                    if (worst > t->worst[k]) t->worst[k] = worst;
                    if (worst > 0.10f && g_mesh_lines < MESH_DETAIL_LINES) {
                        g_mesh_lines++;
                        const int fr[3] = { f0, m, last };
                        char by[3][9];
                        for (int j = 0; j < 3; j++) {
                            int b = t->builds[fr[j]] < 8 ? t->builds[fr[j]] : 8;
                            memcpy(by[j], t->by + fr[j] * 8, b); by[j][b] = 0;
                        }
                        const float* a0 = t->frames[f0] + wv * 4; const float* a1 = t->frames[m] + wv * 4; const float* a2 = t->frames[last] + wv * 4;
                        log_line("X2-SMOOTH-003-DRAWCHECK: cloth id 0x%04X (%u vertices) %s frame odd, %d vertices, worst %u by %.1f%%: "
                                 "(%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f); frames kind %s/%s/%s inv %u/%u/%u tick %ld/%ld/%ld built by %s/%s/%s",
                                 t->actor_id, n, k == 0 ? "render-only" : "logic", odd, wv, 100.0f * worst,
                                 a0[0], a0[1], a0[2], a1[0], a1[1], a1[2], a2[0], a2[1], a2[2],
                                 t->kind[f0] == INV_RENDER_ONLY ? "R" : "L", t->kind[m] == INV_RENDER_ONLY ? "R" : "L",
                                 t->kind[last] == INV_RENDER_ONLY ? "R" : "L", t->inv[f0], t->inv[m], t->inv[last],
                                 t->tick[f0], t->tick[m], t->tick[last], by[0], by[1], by[2]);
                    }
                }
            }
            memcpy(t->frames[t->head], out, n * 16);
            t->kind[t->head] = kind; t->inv[t->head] = g_inv; t->tick[t->head] = g_logic_tick;
            t->builds[t->head] = 1; t->by[t->head * 8] = tl_deform_by;
            t->head = (t->head + 1) % 3;
            if (t->count < 3) t->count++;
        }
    }
    ReleaseSRWLockExclusive(&g_mesh_lock);
}

// Our copy of FUN_00514ab0 at its Ch_ClothDeform call: the same store, then the buffer is kept (drawn frames only).
static uint32_t __cdecl hook_mark_upload(uint32_t* flag, uint32_t value) {
    uint32_t old = *flag;
    *flag = value;
    int kind = g_inv_kind;
    if (g_opt_drawcheck && (kind == INV_LOGIC || kind == INV_RENDER_ONLY))
        __try { mesh_record((uint8_t*)flag - MESH_REC_FLAG, kind); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return old;
}

// Points the upload CALL in Ch_ClothDeform at hook_mark_upload, after checking the bytes around it and the setter.
static void install_meshcheck() {
    if (!g_opt_drawcheck || !g_cloth_deform) return;
    uint8_t* site = (uint8_t*)g_cloth_deform + DEFORM_UPLOAD_CALL;
    uint8_t* setter = call_target(site);
    if (memcmp(site - sizeof(DEFORM_UPLOAD_BEFORE), DEFORM_UPLOAD_BEFORE, sizeof(DEFORM_UPLOAD_BEFORE)) != 0 || !setter ||
        !in_image(setter) || memcmp(setter, UPLOAD_SETTER_BYTES, sizeof(UPLOAD_SETTER_BYTES)) != 0) {
        log_line("  X2-SMOOTH-003-DRAWCHECK: cloth upload CALL not at Ch_ClothDeform +0x475 - mesh check off");
        return;
    }
    if (!patch_call_rel32(site, setter, (void*)&hook_mark_upload)) { log_line("  X2-SMOOTH-003-DRAWCHECK: upload CALL patch failed - mesh check off"); return; }
    g_upload_setter = setter;
    log_line("  X2-SMOOTH-003-DRAWCHECK: mesh check on, CALL gva=0x%08X (expected 0x006DB275) -> setter gva=0x%08X (expected 0x00514AB0)",
             rt_to_gva(site), rt_to_gva(setter));
}

// DRAWCHECK summary part: cloth meshes with odd frames this window, then a new window.
static void meshcheck_report() {
    char buf[1024] = ""; int len = 0, shown = 0;
    AcquireSRWLockExclusive(&g_mesh_lock);
    for (int i = 0; i < MESH_TRACKS; i++) {
        MeshTrack& t = g_mesh[i];
        if (t.out && (t.odd[0] || t.odd[1]) && shown < 12) {
            int w = _snprintf_s(buf + len, sizeof(buf) - len, _TRUNCATE, "%s0x%04X (%u vertices) %ld/%ld frames, %ld/%ld vertices, worst %.1f%%/%.1f%%",
                                shown ? "; " : "", t.actor_id, t.n, t.odd[0], t.odd[1], t.odd_verts[0], t.odd_verts[1],
                                100.0f * t.worst[0], 100.0f * t.worst[1]);
            if (w > 0) { len += w; shown++; }
        }
        t.odd[0] = t.odd[1] = t.odd_verts[0] = t.odd_verts[1] = 0; t.worst[0] = t.worst[1] = 0;
    }
    g_mesh_lines = 0;
    ReleaseSRWLockExclusive(&g_mesh_lock);
    if (g_upload_setter)
        log_line("X2-SMOOTH-003-DRAWCHECK: last %d drawn frames, cloth meshes (render-only/logic): %s", g_dc_window,
                 shown ? buf : "none odd");
}

static float wrap_turn(float a) { a -= (float)(int)a; if (a > 0.5f) a -= 1.0f; if (a < -0.5f) a += 1.0f; return a; }

// Puts an angle triple (turns) in the form with the middle angle in [-1/4, 1/4]; false near the quarter turn.
static bool dc_canonical(float* t) {
    float x = wrap_turn(t[0]), y = wrap_turn(t[1]), z = wrap_turn(t[2]);
    if (y > 0.25f || y < -0.25f) { x = wrap_turn(x + 0.5f); y = wrap_turn(0.5f - y); z = wrap_turn(z + 0.5f); }
    t[0] = x; t[1] = y; t[2] = z;
    return y < 0.25f - DC_GIMBAL_MARGIN && y > -0.25f + DC_GIMBAL_MARGIN;
}

// How far the middle triple x1 is from half way between x0 and x2, beyond their own movement (0 = not odd).
static float dc_odd_by(const float* x0, const float* x1, const float* x2, int* channel) {
    float worst = 0; *channel = -1;
    for (int c = 0; c < 3; c++) {
        float err = 0;
        midcheck_bad(x0[c], x1[c], x2[c], &err);
        float span = turn_diff(x2[c], x0[c]);
        if (err > DC_MIN_ODD && err > span && err > worst) { worst = err; *channel = c; }
    }
    return worst;
}

// World position check: true when p1 is far from half way between p0 and p2 (see DC_WORLD_MIN_ODD); off = that
// distance, len = the bone's distance to bone 0.
static bool dc_world_odd(const float* p0, const float* p1, const float* p2, float* off, float* len) {
    float e = 0, span = 0, l = 0;
    for (int c = 0; c < 3; c++) {
        float mid = 0.5f * (p0[c] + p2[c]);
        e += (p1[c] - mid) * (p1[c] - mid); span += (p2[c] - p0[c]) * (p2[c] - p0[c]); l += p0[c] * p0[c];
    }
    *off = sqrtf(e); *len = sqrtf(l);
    return *len > 0 && *off > DC_WORLD_MIN_ODD * *len && *off > sqrtf(span);
}

// Keeps the drawn angles of actor slot i in out (canonical; NaN marks a bone not judged) and the drawn positions in
// pos; false if it has no skeleton.
static bool drawcheck_copy(int i, const uint8_t* a, float* out, float* pos) {
    if (!a[OFF_ACTIVE] || (a[OFF_RANGE_SKIP] != 0 && a[OFF_RANGE_KEEP] == 0)) return false;
    const uint8_t* bones = *(uint8_t* const*)(a + OFF_BONES);
    int nb = bone_count(a);
    if (!bones || !nb) return false;
    if (nb > DC_BONES) nb = DC_BONES;
    for (int b = 0; b < nb; b++) {
        float* t = out + b * 3;
        for (int c = 0; c < 3; c++) t[c] = *(const float*)(bones + b * BONE_SIZE + BONE_ANGLE[c]) * TURNS_PER_RADIAN;
        if (!dc_canonical(t)) t[0] = NAN;
        g_dc_damped[i][b] = bones[b * BONE_SIZE + 0x144];
        const float* w = (const float*)(bones + b * BONE_SIZE + BONE_WORLD_T);
        const float* r = (const float*)(bones + BONE_WORLD_T);
        for (int c = 0; c < 3; c++) pos[b * 3 + c] = w[c] - r[c];
    }
    return true;
}

// Compares the middle frame m of actor slot i with the frames on both sides; counts and (a few) logs the odd bones.
static void drawcheck_compare(int i, const uint8_t* a, int f0, int m, int t) {
    const float* A = g_dc_ang + ((size_t)f0 * DC_ACTORS + i) * DC_BONES * 3;
    const float* M = g_dc_ang + ((size_t)m * DC_ACTORS + i) * DC_BONES * 3;
    const float* N = g_dc_ang + ((size_t)t * DC_ACTORS + i) * DC_BONES * 3;
    uint16_t id = g_dc_id[m][i];
    if (g_dc_count_id[i] != id) {
        memset(g_dc_count[i], 0, sizeof(g_dc_count[i])); memset(g_dc_wcount[i], 0, sizeof(g_dc_wcount[i]));
        g_dc_count_id[i] = id;
    }
    int nb = bone_count(a);
    if (nb > DC_BONES) nb = DC_BONES;
    bool ro = g_dc_kind[m] == INV_RENDER_ONLY;
    const float* P0 = g_dc_pos + ((size_t)f0 * DC_ACTORS + i) * DC_BONES * 3;
    const float* P1 = g_dc_pos + ((size_t)m * DC_ACTORS + i) * DC_BONES * 3;
    const float* P2 = g_dc_pos + ((size_t)t * DC_ACTORS + i) * DC_BONES * 3;
    for (int b = 1; b < nb; b++) {
        float off, len;
        if (!dc_world_odd(P0 + b * 3, P1 + b * 3, P2 + b * 3, &off, &len)) continue;
        uint16_t& n = g_dc_wcount[i][b][ro ? 0 : 1];
        if (n < 0xFFFF) n++;
        if (g_dc_window_lines >= DC_DETAIL_LINES) continue;
        g_dc_window_lines++;
        const float* p0 = P0 + b * 3; const float* p1 = P1 + b * 3; const float* p2 = P2 + b * 3;
        log_line("X2-SMOOTH-003-DRAWCHECK: %s frame, slot=%d id=0x%04X bone %d drawn %.1f%% of its distance to bone 0 "
                 "(%.2f) off: before (%.2f %.2f %.2f) this (%.2f %.2f %.2f) after (%.2f %.2f %.2f); damped=%d",
                 ro ? "render-only" : "logic", i, id, b, 100.0f * off / len, len, p0[0], p0[1], p0[2], p1[0], p1[1],
                 p1[2], p2[0], p2[1], p2[2], g_dc_damped[i][b]);
    }
    for (int b = 0; b < nb; b++) {
        const float* x0 = A + b * 3; const float* x1 = M + b * 3; const float* x2 = N + b * 3;
        if (x0[0] != x0[0] || x1[0] != x1[0] || x2[0] != x2[0]) continue;      // near gimbal lock: not judged
        int wc;
        float worst = dc_odd_by(x0, x1, x2, &wc);
        if (wc < 0) continue;
        InterlockedIncrement(&g_dc_odd[g_dc_kind[m] & 3]);
        uint16_t& n = g_dc_count[i][b][ro ? 0 : 1];
        if (n < 0xFFFF) n++;
        if (g_dc_window_lines >= DC_DETAIL_LINES) continue;
        g_dc_window_lines++;
        const uint8_t* hb = *(uint8_t* const*)(a + OFF_BONES) + b * BONE_SIZE;
        log_line("X2-SMOOTH-003-DRAWCHECK: %s frame, slot=%d id=0x%04X bone %d odd by %.3f turn on ch %d: "
                 "before (%.3f %.3f %.3f) this (%.3f %.3f %.3f) after (%.3f %.3f %.3f); damped=%d target now (%.3f %.3f %.3f)",
                 ro ? "render-only" : "logic", i, id, b, worst, wc, x0[0], x0[1], x0[2], x1[0], x1[1], x1[2],
                 x2[0], x2[1], x2[2], hb[0x144], *(const float*)(hb + 0x114) * TURNS_PER_RADIAN,
                 *(const float*)(hb + 0x118) * TURNS_PER_RADIAN, *(const float*)(hb + 0x11C) * TURNS_PER_RADIAN);
    }
}

// Logs the bones of one counter table odd most often (what: which check).
static void dc_report_counts(const char* what, uint16_t (*count)[DC_BONES][2]) {
    struct Top { int slot, bone, total; } top[DC_REPORT_BONES] = {};
    int ro_all = 0, logic_all = 0;
    for (int i = 0; i < DC_ACTORS; i++)
        for (int b = 0; b < DC_BONES; b++) {
            int tot = count[i][b][0] + count[i][b][1];
            ro_all += count[i][b][0]; logic_all += count[i][b][1];
            if (!tot || tot <= top[DC_REPORT_BONES - 1].total) continue;
            int k = DC_REPORT_BONES - 1;
            while (k > 0 && top[k - 1].total < tot) { top[k] = top[k - 1]; k--; }
            top[k] = { i, b, tot };
        }
    char buf[1024] = ""; int len = 0;
    for (int k = 0; k < DC_REPORT_BONES && top[k].total; k++) {
        const Top& e = top[k];
        int w = _snprintf_s(buf + len, sizeof(buf) - len, _TRUNCATE, "%s0x%04X b%d %d/%d%s", k ? ", " : "",
                            g_dc_count_id[e.slot], e.bone, count[e.slot][e.bone][0], count[e.slot][e.bone][1],
                            g_dc_damped[e.slot][e.bone] ? " damped" : "");
        if (w < 0) break;
        len += w;
    }
    log_line("X2-SMOOTH-003-DRAWCHECK: last %d drawn frames, %s: odd bone frames render-only %d, logic %d; most often "
             "(id bone render-only/logic): %s", g_dc_window, what, ro_all, logic_all, ro_all + logic_all ? buf : "none");
}

// Logs the bones odd most often over the last window (angles, then drawn positions) and starts a new one.
static void drawcheck_report() {
    dc_report_counts("angles", g_dc_count);
    dc_report_counts("world", g_dc_wcount);
    meshcheck_report();
    memset(g_dc_count, 0, sizeof(g_dc_count));
    memset(g_dc_wcount, 0, sizeof(g_dc_wcount));
    g_dc_window = 0; g_dc_window_lines = 0;
}

// Logs (a few times) an actor whose bones could not be read, with what pointed there.
static void drawcheck_actor_fault(int i, const uint8_t* a) {
    InterlockedIncrement(&g_dc_faults);
    static volatile LONG lines = 0;
    if (lines >= 8) return;
    InterlockedIncrement(&lines);
    __try {
        log_line("X2-SMOOTH-003-DRAWCHECK: slot=%d id=0x%04X unreadable (bones %p, model header %p, parent %08X, "
                 "range %d/%d) - skipped this frame", i, *(const uint16_t*)a, *(void* const*)(a + OFF_BONES),
                 *(void* const*)(a + OFF_MODEL_HEADER), *(const uint32_t*)(a + OFF_PARENT), a[OFF_RANGE_SKIP], a[OFF_RANGE_KEEP]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { log_line("X2-SMOOTH-003-DRAWCHECK: slot=%d actor itself unreadable", i); }
}

static void drawcheck_post(int kind) {          // kind: pacing_kind() of the invocation just done
    if (!g_opt_drawcheck || !g_dc_ang || !g_dc_pos) return;
    if (kind != INV_LOGIC && kind != INV_RENDER_ONLY) return;     // not drawn
    uint8_t* base = *g_actor_table;
    int n = *g_actor_count;
    if (!base || n <= 0) return;
    if (n > DC_ACTORS) n = DC_ACTORS;
    int t = g_dc_head;                                     // the frame recorded now
    g_dc_kind[t] = kind;
    for (int i = 0; i < n; i++) {
        uint8_t* a = base + (size_t)i * ACTOR_SIZE;
        float* out = g_dc_ang + ((size_t)t * DC_ACTORS + i) * DC_BONES * 3;
        float* pos = g_dc_pos + ((size_t)t * DC_ACTORS + i) * DC_BONES * 3;
        g_dc_id[t][i] = 0xFFFF;
        __try { if (drawcheck_copy(i, a, out, pos)) g_dc_id[t][i] = *(const uint16_t*)a; }
        __except (EXCEPTION_EXECUTE_HANDLER) { drawcheck_actor_fault(i, a); }
    }
    for (int i = n; i < DC_ACTORS; i++) g_dc_id[t][i] = 0xFFFF;
    g_dc_head = (t + 1) % 3;
    if (++g_dc_frames < 3) return;
    int m = (t + 2) % 3, f0 = (t + 1) % 3;                 // middle frame m, oldest frame f0, newest t
    for (int i = 0; i < n; i++) {
        uint16_t id = g_dc_id[m][i];
        if (id == 0xFFFF || g_dc_id[f0][i] != id || g_dc_id[t][i] != id) continue;
        uint8_t* a = base + (size_t)i * ACTOR_SIZE;
        __try { drawcheck_compare(i, a, f0, m, t); }
        __except (EXCEPTION_EXECUTE_HANDLER) { drawcheck_actor_fault(i, a); }
    }
    if (++g_dc_window >= DC_REPORT_FRAMES) drawcheck_report();
}

// For X2-DIAG-CAPTURE-AT: the motion clock (actor +0x750, 1/256 animation frames) of the first active actor with this
// id; false if there is none.
bool smooth003_actor_clock(uint16_t id, int32_t* clock) {
    if (!g_actor_table || !g_actor_count) return false;
    __try {
        uint8_t* base = *g_actor_table;
        int n = *g_actor_count;
        if (!base || n <= 0) return false;
        if (n > MAX_ACTORS) n = MAX_ACTORS;
        for (int i = 0; i < n; i++) {
            const uint8_t* a = base + (size_t)i * ACTOR_SIZE;
            if (a[OFF_ACTIVE] && *(const uint16_t*)a == id) { *clock = *(const int32_t*)(a + OFF_MOTION_CLOCK); return true; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// --- POSELOG: one actor's drawn pose per frame, to a file (diagnostic, off) ---------------------------------------------
// X2-SMOOTH-003-POSELOG-ID=<actor id> with -FROM / -TO (motion clock range, actor +0x750): after every invocation in
// that range one line goes to <game>\fps60x2-poselog.csv (appended; each run starts with a "#" line): frame kind (L
// logic, R render-only, B backlog, S stock), the watched actor's motion and sample clock (+0x754), the line's actor id
// (the watched actor, then each actor attached to it), and per bone: damped byte (+0x144),
// angles in turns (+0xE4..), target (+0x114..), local position (+0xF4..), scale (+0x104..) and drawn position (matrix
// +0x88 translation minus bone 0's). Then "|" and per cloth mesh: vertex count and every vertex (the cloth job's
// result, entry +0x08). A stock run and a 60 fps run of the same cutscene compare at equal sampled clocks.
static int   g_poselog_id = 0, g_poselog_from = 0, g_poselog_to = 0;
static FILE* g_poselog = nullptr;
static bool  g_poselog_failed = false;

// One line: frame kind, the watched actor's clocks, this actor's id, its bones and cloth (see above).
static void poselog_line(const uint8_t* a, int32_t clock, int32_t sampled) {
    const uint8_t* bones = *(uint8_t* const*)(a + OFF_BONES);
    int nb = bone_count(a);
    fprintf(g_poselog, "%c,%d,%d,%04X", "LRBS"[pacing_kind() & 3], clock, sampled, *(const uint16_t*)a);
    if (bones && nb) {
        const float* r = (const float*)(bones + 0x88 + 0x30);
        for (int b = 0; b < nb; b++) {
            const uint8_t* hb = bones + b * BONE_SIZE;
            const float* ang = (const float*)(hb + BONE_ANGLE[0]);
            const float* tgt = (const float*)(hb + 0x114);
            const float* pos = (const float*)(hb + 0xF4);
            const float* scl = (const float*)(hb + 0x104);
            const float* w = (const float*)(hb + 0x88 + 0x30);
            fprintf(g_poselog, ",%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f", hb[0x144],
                    ang[0] * TURNS_PER_RADIAN, ang[1] * TURNS_PER_RADIAN, ang[2] * TURNS_PER_RADIAN,
                    tgt[0] * TURNS_PER_RADIAN, tgt[1] * TURNS_PER_RADIAN, tgt[2] * TURNS_PER_RADIAN,
                    pos[0], pos[1], pos[2], scl[0], scl[1], scl[2], w[0] - r[0], w[1] - r[1], w[2] - r[2]);
        }
    }
    for_dyn_mesh((uint8_t*)a, [&](uint8_t*, float* vtex, uint32_t nv) {
        fprintf(g_poselog, ",|,%u", nv);
        for (uint32_t v = 0; v < nv; v++) fprintf(g_poselog, ",%.2f,%.2f,%.2f", vtex[v * 4], vtex[v * 4 + 1], vtex[v * 4 + 2]);
    });
    fprintf(g_poselog, "\n");
}

// The watched actor, then every actor attached to it (actor +0x1A8 = parent actor), one line each.
static void poselog_post() {
    uint8_t* base = *g_actor_table;
    int n = *g_actor_count;
    if (!base || n <= 0) return;
    if (n > MAX_ACTORS) n = MAX_ACTORS;
    const uint8_t* watched = nullptr;
    for (int i = 0; i < n && !watched; i++) {
        const uint8_t* a = base + (size_t)i * ACTOR_SIZE;
        if (a[OFF_ACTIVE] && *(const uint16_t*)a == g_poselog_id) watched = a;
    }
    if (!watched || !bone_count(watched)) return;
    int32_t clock = *(const int32_t*)(watched + OFF_MOTION_CLOCK);
    int32_t sampled = *(const int32_t*)(watched + OFF_SAMPLED_CLOCK);
    if (clock < g_poselog_from || clock > g_poselog_to) return;
    if (!g_poselog) {
        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
        while (len && path[len - 1] != '\\') len--;
        _snprintf_s(path + len, MAX_PATH - len, _TRUNCATE, "fps60x2-poselog.csv");
        g_poselog = _fsopen(path, "a", _SH_DENYNO);        // shared: readable while the game runs
        if (!g_poselog) { g_poselog_failed = true; log_line("X2-SMOOTH-003-POSELOG: cannot open %s - off", path); return; }
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(g_poselog, "# run %02d:%02d:%02d id 0x%04X clocks %d..%d, %d bones\n", t.wHour, t.wMinute, t.wSecond,
                g_poselog_id, g_poselog_from, g_poselog_to, bone_count(watched));
        log_line("X2-SMOOTH-003-POSELOG: writing id 0x%04X and its attached actors at clocks %d..%d to %s", g_poselog_id,
                 g_poselog_from, g_poselog_to, path);
    }
    poselog_line(watched, clock, sampled);
    for (int i = 0; i < n; i++) {
        const uint8_t* a = base + (size_t)i * ACTOR_SIZE;
        if (a[OFF_ACTIVE] && *(const uint8_t* const*)(a + OFF_PARENT) == watched) poselog_line(a, clock, sampled);
    }
    fflush(g_poselog);
}

// After: freeze diagnostics, and a counters line every 1800 walks (~1 minute).
static void post() {
    if (g_poselog_id && !g_poselog_failed) __try { poselog_post(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Unreadable actors are skipped one by one inside; a fault here (the actor table itself) drops the frames kept so
    // far and the check carries on; only DC_MAX_FAULTS in a row turn it off.
    if (g_opt_drawcheck && g_dc_ang && smooth_live(3)) {
        static int faults_in_a_row = 0;
        __try { drawcheck_post(pacing_kind()); faults_in_a_row = 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_dc_frames = 0;
            InterlockedIncrement(&g_dc_faults);
            if (++faults_in_a_row >= DC_MAX_FAULTS) { g_opt_drawcheck = 0; log_line("X2-SMOOTH-003-DRAWCHECK: %d faults in a row - off", faults_in_a_row); }
        }
    }
    if (g_freeze_state) __try { freeze_diag(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_freeze_state = nullptr; log_line("X2-SMOOTH-003-FREEZEDIAG: fault - off"); }
    static LONG last = 0;
    if (g_walks - last >= 1800) {
        last = g_walks;
        log_line("X2-SMOOTH-003: walks=%ld stepped=%ld batches=%ld pays=%ld gated(seq=%ld motev=%ld) "
                 "loopguard(wrap=%ld clamp=%ld hold=%ld) loopend_holds=%ld endx=%ld sanity=%ld midcheck(bad=%ld held=%ld) drawcheck(odd logic=%ld render-only=%ld faults=%ld) stock_steps=%ld phase(skips=%ld half_logic=%ld) cloth_held=%ld cloth_drawn=%ld cloth_miss=%ld cloth_mixed=%ld half=%d live=%d",
                 g_walks, g_stepped, g_batches, g_pays, g_gated_seq, g_gated_motev, g_guard_wrap, g_guard_clamp,
                 g_guard_hold, g_loopend_holds, (LONG)g_endx, (LONG)g_sanity_reverts, (LONG)g_mid_bad, (LONG)g_mid_held, (LONG)g_dc_odd[INV_LOGIC], (LONG)g_dc_odd[INV_RENDER_ONLY], (LONG)g_dc_faults, g_stock_steps, (LONG)g_phase_skips, (LONG)g_half_logic_samples, (LONG)g_cloth_skipped, (LONG)g_extrap_n, (LONG)g_extrap_miss, (LONG)g_cloth_mixed, *g_half_rate_switch, smooth_live(3));
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

// --- Install: find every game address and check it; nothing is patched here --------------------------------------------
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
    // Ch_DecodeKeys: MOV EBX,[EDI+OFF_MOTION_CLOCK]; MOV EAX,[EDI+OFF_MOTION_END] - checks the motion-block shift (+0x10)
    uint8_t* sk = sig_find("X2-SMOOTH-003 sample clock (Ch_DecodeKeys)",
        "8B 9F 50 07 00 00 8B 87 2C 07 00 00 ?? 8D B3 FF 01 00 00", 0x6E841C);
    if (!sa || !sb || !sc || !sd || !se || !sf || !sk) return false;

    g_callsite_step = sa + 0x18; g_callsite_seq = sd + 0x08; g_callsite_noskel = se + 0x13; g_callsite_motev = sf + 0x0E;
    uint8_t* step = call_target(g_callsite_step);
    uint8_t* seq = call_target(g_callsite_seq);
    uint8_t* noskel = call_target(g_callsite_noskel);
    uint8_t* motev = call_target(g_callsite_motev);
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
              *(const uint32_t*)(sk + 2) == OFF_MOTION_CLOCK && *(const uint32_t*)(sk + 8) == OFF_MOTION_END;
    if (!ok) {
        log_line("  X2-SMOOTH-003: cross-check failed (step %p seq %p noskel %p sync %p motev %p disp %p freeze %p) - refused",
                 step, seq, noskel, sync, motev, disp, frz);
        return false;
    }
    g_actor_table = (uint8_t* volatile*)abs_operand("actor table", noskel + 0x07);
    g_actor_count = (volatile int32_t*)abs_operand("actor count", noskel + 0x23);
    g_half_rate_switch = (volatile int32_t*)abs_operand("half-rate switch", sd + 0x12);
    g_actors_frozen = (volatile int8_t*)abs_operand("freeze flag", frz + 0x03);
    if (!g_actor_table || !g_actor_count || !g_half_rate_switch || !g_actors_frozen) { log_line("  X2-SMOOTH-003: global operand outside FFX-2.exe - refused"); return false; }
    if (*g_half_rate_switch != 0) log_line("  X2-SMOOTH-003: note: half-rate switch was %d at install (debug menu?)", *g_half_rate_switch);
    g_step_motion = (step_motion_fn)step;
    g_seq_frame = (actor_fn)seq;
    g_motion_events = (actor_fn)motev;
    g_noskel_pass = (void_fn)noskel;
    g_job_sync = (void_fn)sync;
    g_skeleton_dispatch = (dispatch_fn)disp;
    g_opt_gates = config_row("X2-SMOOTH-003-GATES", 1);
    g_opt_hold = config_row("X2-SMOOTH-003-HOLD", 1);
    g_opt_damphold = config_row("X2-SMOOTH-003-DAMPHOLD", 1);
    g_opt_phase = config_row("X2-SMOOTH-003-PHASE", 1);
    g_poselog_id = config_row("X2-SMOOTH-003-POSELOG-ID", 0);
    g_poselog_from = config_row("X2-SMOOTH-003-POSELOG-FROM", 0);
    g_poselog_to = config_row("X2-SMOOTH-003-POSELOG-TO", 0);
    g_opt_loopguard = config_row("X2-SMOOTH-003-LOOPGUARD", 1);
    g_opt_loopend = config_row("X2-SMOOTH-003-LOOPEND", 1);
    g_opt_endextrap = config_row("X2-SMOOTH-003-ENDEXTRAP", 1);
    g_opt_endextrap_once = config_row("X2-SMOOTH-003-ENDEXTRAP-ONCE", 1);
    g_opt_sanity = config_row("X2-SMOOTH-003-SANITY", 1);
    g_opt_midcheck = config_row("X2-SMOOTH-003-MIDCHECK", 1);
    g_opt_drawcheck = config_row("X2-SMOOTH-003-DRAWCHECK", 1);
    if (g_opt_drawcheck) {
        g_dc_ang = (float*)calloc((size_t)3 * DC_ACTORS * DC_BONES * 3, sizeof(float));
        g_dc_pos = (float*)calloc((size_t)3 * DC_ACTORS * DC_BONES * 3, sizeof(float));
        if (!g_dc_ang || !g_dc_pos) g_opt_drawcheck = 0;
    }
    g_opt_keyhold = config_row("X2-SMOOTH-003-KEYHOLD", 1);
    g_opt_keyhold_limit = config_row("X2-SMOOTH-003-KEYHOLD-D", 0x300);
    if (g_opt_keyhold_limit < 0x80 || g_opt_keyhold_limit > 0x800) g_opt_keyhold_limit = 0x300;
    if (g_opt_keyhold) {
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
        if (!k1 || !k2 || !k3 || !k4) { g_opt_keyhold = 0; log_line("  X2-SMOOTH-003-KEYHOLD: layout cross-check failed - off"); }
        else log_line("  X2-SMOOTH-003-KEYHOLD: on, threshold 0x%X per 0x1000 key units (pose +0x758, list +0x75C, res +0x7F8)", g_opt_keyhold_limit);
    }
    g_stock_id = config_row("X2-SMOOTH-003-STOCK-ID", 0);
    g_stock_id_end = config_row("X2-SMOOTH-003-STOCK-ID-END", 0);
    if (g_opt_endextrap || g_opt_sanity) {
        g_prev_ang = (float*)calloc((size_t)MAX_ACTORS * MAX_BONES * 3, sizeof(float));
        g_cur_ang = (float*)calloc((size_t)MAX_ACTORS * MAX_BONES * 3, sizeof(float));
        if (!g_prev_ang || !g_cur_ang) { g_opt_endextrap = 0; g_opt_sanity = 0; log_line("  X2-SMOOTH-003: ENDEXTRAP/SANITY off (out of memory)"); }
        if (g_opt_midcheck && g_opt_sanity) {
            g_sampled_ang = (float*)calloc((size_t)MAX_ACTORS * MAX_BONES * 3, sizeof(float));
            g_mid_hold = (uint16_t*)calloc((size_t)MAX_ACTORS * MAX_BONES, sizeof(uint16_t));
            memset(g_sampled_id, 0xFF, sizeof g_sampled_id);
            if (!g_sampled_ang || !g_mid_hold) { g_opt_midcheck = 0; log_line("  X2-SMOOTH-003-MIDCHECK: off (out of memory)"); }
        } else g_opt_midcheck = 0;
    }
    if (g_stock_id)
        log_line("  X2-SMOOTH-003-STOCK-ID: actors %d..%d (0x%04X..0x%04X) take whole stock steps", g_stock_id,
                 g_stock_id_end ? g_stock_id_end : g_stock_id, g_stock_id, g_stock_id_end ? g_stock_id_end : g_stock_id);
    g_opt_shortloop = config_row("X2-SMOOTH-003-SHORTLOOP", 1);
    g_opt_freezediag = config_row("X2-SMOOTH-003-FREEZEDIAG", 0);
    if (g_opt_freezediag) {
        g_freeze_state = (FreezeState*)calloc(MAX_ACTORS, sizeof(FreezeState));
        g_freezediag_running = g_freeze_state != nullptr;
        g_trace_id = config_row("X2-SMOOTH-003-FREEZEDIAG-ID", 0);
        g_trace_id_end = config_row("X2-SMOOTH-003-FREEZEDIAG-ID-END", 0);
        g_trace_always = config_row("X2-SMOOTH-003-FREEZEDIAG-ALWAYS", 0);
        if (!g_trace_id_end) g_trace_id_end = g_trace_id;
        if (g_trace_id && g_freeze_state && magic_mods_subscribe("X2-SMOOTH-003-FREEZEDIAG", fd_on_load, fd_on_unload))
            log_line("  X2-SMOOTH-003-FREEZEDIAG: trace ids 0x%04X..0x%04X while a magic DLL is loaded", g_trace_id, g_trace_id_end);
        else g_trace_id = 0x10000;
        log_line("  X2-SMOOTH-003-FREEZEDIAG: %s (pose unchanged for %d invocations is logged)", g_freeze_state ? "on" : "out of memory - off", FREEZE_RUN);
    }
    g_opt_cloth = config_row("X2-SMOOTH-003-CLOTHHOLD", 4);
    g_opt_cloth_extrap_pct = config_row("X2-SMOOTH-003-CLOTHEXTRAP-PCT", 50);
    g_opt_cloth_mixed = config_row("X2-SMOOTH-003-CLOTHHOLD-MIXED", 1);
    if (g_opt_cloth != 0 && (g_opt_cloth < 2 || g_opt_cloth > 4)) {
        log_line("  X2-SMOOTH-003-CLOTHHOLD: mode %d not available - using mode 4", g_opt_cloth);
        g_opt_cloth = 4;
    }
    if (g_opt_cloth) {
        // entry through MOV ESI,[EBP+8]; MOV EAX,[ESI+0x1A0]; TEST EAX,0x200000 (cloth actor bit)
        uint8_t* cloth = sig_find("X2-SMOOTH-003-CLOTHHOLD Ch_ClothSimulate",
            "55 8B EC 81 EC B8 02 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 56 8B 75 08 89 B5 78 FD FF FF 8B 86 A0 01 00 00 A9 00 00 20 00",
            0x6F4C90);
        // Ch_ClothJob: loop over count actors: CALL attach (+0x19); CALL Ch_ClothSimulate (+0x1F); deform gate; deform
        g_cloth_job = sig_find("X2-SMOOTH-003-CLOTHHOLD cloth job (Ch_ClothJob)",
            "55 8B EC 53 8B 5D 0C 56 33 F6 39 33 7E ?? 57 90 8B 45 08 6A 00 8B 3C B0 57 E8 ?? ?? ?? ?? 57 E8 ?? ?? ?? ?? 83 C4 0C "
            "83 3D ?? ?? ?? ?? 00 75 09 57 E8", 0x6DB500);
        if (!cloth || !g_cloth_job || call_target(g_cloth_job + 0x1F) != cloth) {
            g_opt_cloth = 0;
            g_cloth_job = nullptr;
            log_line("  X2-SMOOTH-003-CLOTHHOLD: cloth job not found or does not call Ch_ClothSimulate - off");
        }
    }
    if (g_opt_cloth >= 3) {
        // job +0x27: CMP dword [gate],0 / JNZ +9 / PUSH EDI / CALL Ch_ClothDeform (verified by the job signature)
        g_cloth_deform_gate = (volatile int32_t*)abs_operand("cloth deform gate", g_cloth_job + 0x29);
        g_cloth_deform = (void (__cdecl*)(uint8_t*))call_target(g_cloth_job + 0x31);
        if (!g_cloth_deform_gate || !g_cloth_deform || !in_image((void*)g_cloth_deform)) {
            g_opt_cloth = 2;
            log_line("  X2-SMOOTH-003-CLOTHHOLD: deform call not at job +0x31 - modes 3/4 unavailable, mode 2");
        } else {
            log_line("  X2-SMOOTH-003-CLOTHHOLD: mode %d, mixed batches %s, deform gva=0x%08X (expected 0x006DAE00), gate gva=0x%08X (0x00DF6440)",
                     g_opt_cloth, g_opt_cloth_mixed ? "not stepped on render-only frames (MIXED)" : "run by the game", rt_to_gva((void*)g_cloth_deform), rt_to_gva((const void*)g_cloth_deform_gate));
        }
    }
    g_held_bytes = (uint8_t*)calloc(MAX_ACTORS, HOLD_BYTES);
    if (g_opt_damphold) g_damp_save = (DampSave*)calloc((size_t)MAX_ACTORS * DAMP_MAX_BONES, sizeof(DampSave));
    if (!g_held_bytes) { log_line("  X2-SMOOTH-003: out of memory - refused"); return false; }
    install_peel();

    log_line("  X2-SMOOTH-003: Ch_StepMotion gva=0x%08X (0x006E78F0) call 0x%08X (0x006E1394); Ch_NoSkelPass gva=0x%08X (0x006E1610) "
             "call 0x%08X (0x0060586C); SeqFrame gva=0x%08X (0x006D2720) call 0x%08X (0x006E7A65); Motev gva=0x%08X (0x006E27F0) "
             "call 0x%08X (0x006E1795); sync 0x%08X (0x00B520D0) dispatch 0x%08X (0x00B520E0); half gva=0x%08X (0x0116E3D4) "
             "freeze gva=0x%08X (0x00DF67F3); actors 0x%08X (0x01CC9604) count 0x%08X (0x01CC9600) stride 0x910; "
             "gates=%d hold=%d loopguard=%d loopend=%d shortloop=%d clothhold=%d; patches at the first pre(); F7 toggles",
             rt_to_gva(step), rt_to_gva(g_callsite_step), rt_to_gva(noskel), rt_to_gva(g_callsite_noskel),
             rt_to_gva(seq), rt_to_gva(g_callsite_seq), rt_to_gva(motev), rt_to_gva(g_callsite_motev),
             rt_to_gva(sync), rt_to_gva(disp), rt_to_gva((const void*)g_half_rate_switch), rt_to_gva((const void*)g_actors_frozen),
             rt_to_gva((const void*)g_actor_table), rt_to_gva((const void*)g_actor_count),
             g_opt_gates, g_opt_hold, g_opt_loopguard, g_opt_loopend, g_opt_shortloop, g_opt_cloth);
    return true;
}

// Dev hot reload: held poses back (pre would restore them next) and the engine's half-rate switch off.
static void stop() {
    __try { hold_restore(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_poselog) { fclose(g_poselog); g_poselog = nullptr; }
    if (g_we_set_half_rate) { *g_half_rate_switch = 0; g_we_set_half_rate = false; }
}

Row row_SMOOTH003 = {
    "X2-SMOOTH-003",
    "60 Hz limbs: engine half-rate motion + a half step on render-only frames (needs X2-SMOOTH=1)",
    1, install, pre, post, 0, 0, stop
};

} // namespace x2
