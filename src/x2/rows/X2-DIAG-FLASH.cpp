// SPDX-License-Identifier: Apache-2.0
// X2-DIAG-FLASH (diagnostic, off, observe only): logs the two ways a spell can light up the whole screen.
//
//   tint   service +0xE08: sets the HD post-process tint colour (r, g, b, alpha); the HD frame clears the alpha again
//          after each frame
//   bloom  service +0xE0C: a fade slot that switches the bloom on and sets its level
// Per invocation, while a value is non-zero (at most 1500 lines): the tint sets and last alpha, the bloom switches and
// level, and what the HD frame sees before its reset. With X2-DIAG-FTBLINK on, only while one of its X2-DIAG-FTBLINK-DLL
// spell DLLs is loaded. X2-DIAG-FTBLINK's ALT mode reads its HD pointers from this row. Evidence: re/ledger/LENS-GLOW.md §5.
#include "../x2.h"
#include <intrin.h>

namespace x2 {

extern Row row_DIAGFLASH;
bool ftblink_gate();                             // X2-DIAG-FTBLINK: true unless it is on and no watched DLL is loaded

typedef void(__cdecl* tint_t)(float, float, float, float);
typedef void(__cdecl* bloom_t)(int);
typedef void(__fastcall* reset_t)(uint8_t*);
static uint8_t* g_tint_fn = nullptr; static tint_t g_tint = nullptr;
static uint8_t* g_bloom_fn = nullptr; static bloom_t g_bloom = nullptr;
static uint8_t* g_reset_fn = nullptr; static reset_t g_reset = nullptr;
uint8_t** g_ctx_ptr = nullptr;                   // [0xDD10F4] (also read by X2-DIAG-FTBLINK-ALT)
uint8_t** g_obj_ptr = nullptr;                   // [0xDDC74C] (also read by X2-DIAG-FTBLINK-ALT)

struct Inv { int pk_exe, pk_dll; uint32_t pk_col; int pk_mode; int tint_n; float tint_a; int bloom_n; int bloom_arg; int at_n; float at_a; int at_lock, at_armed, at_b58, at_b59; float at_b5c; };
static Inv g_cur = {};
static LONG g_lines = 0, g_inv = 0;

// Packed-colour tint FUN_0067f210 (magic service +0x3FC; EgoFlash FUN_100010c0 in magic_0106, pppop_flash in the EXE):
// cdecl (handle, packed RGBA, mode), returns a handle; queues Gfx_QueueTintCmd, which the HD frame clears unless re-queued.
typedef uint32_t(__cdecl* pk_t)(uint32_t h, uint32_t col, uint32_t mode);
static uint8_t* g_pk_fn = nullptr; static pk_t g_pk = nullptr;
static uint32_t __cdecl h_pk(uint32_t h, uint32_t col, uint32_t mode) {
    if (in_image(_ReturnAddress())) g_cur.pk_exe++; else g_cur.pk_dll++;
    g_cur.pk_col = col; g_cur.pk_mode = (int)(mode & 0xFF);
    return g_pk(h, col, mode);
}
static void __cdecl h_tint(float r, float g, float b, float a) {
    g_cur.tint_n++; g_cur.tint_a = a;
    g_tint(r, g, b, a);
}
static void __cdecl h_bloom(int on) {
    g_cur.bloom_n++; g_cur.bloom_arg = on;
    g_bloom(on);
}
static void __fastcall h_reset(uint8_t* ctx) {
    __try {
        g_cur.at_n++;
        g_cur.at_a = *(float*)(ctx + 0x12390);
        g_cur.at_lock = ctx[0x123A8];
        g_cur.at_armed = ctx[0x123B0];
        uint8_t* obj = g_obj_ptr ? *g_obj_ptr : nullptr;
        if (obj) { g_cur.at_b58 = obj[0x58]; g_cur.at_b59 = obj[0x59]; g_cur.at_b5c = *(float*)(obj + 0x5C); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_reset(ctx);
}

static bool install() {
    g_tint_fn = sig_find("X2-DIAG-FLASH tint set FUN_004548e0",
        "55 8B EC E8 ?? ?? ?? ?? 80 B8 A8 23 01 00 00 75 ?? D9 45 08 D9 98 84 23 01 00", 0x4548E0);
    g_bloom_fn = sig_find("X2-DIAG-FLASH bloom enable FUN_004546a0",
        "55 8B EC 83 7D 08 00 53 0F 95 C3 E8 ?? ?? ?? ?? 80 78 58 00 88 58 58 75", 0x4546A0);
    g_reset_fn = sig_find("X2-DIAG-FLASH tint reset FUN_004aec60",
        "80 B9 A8 23 01 00 00 75 1B 80 B9 B0 23 01 00 01 75 12 D9 EE", 0x4AEC60);
    // FUN_0067f210: PUSH EBP; MOV EBP,ESP; SUB ESP,0x18; PUSH EBX; MOV EBX,[EBP+0xC]; MOV EAX,EBX; SHR EAX,0x18 (plain RETs)
    g_pk_fn = sig_find("X2-DIAG-FLASH packed tint FUN_0067f210",
        "55 8B EC 83 EC 18 53 8B 5D 0C 8B C3 C1 E8 18 89 45 EC 8B C3", 0x67F210);
    if (!g_tint_fn || !g_bloom_fn || !g_reset_fn || !g_pk_fn) return false;
    // getters: FUN_004a8040 `MOV EAX,[ctx]; RET`, FUN_004f7980 `MOV EAX,[obj]; TEST EAX,EAX`
    uint8_t* gc = call_target(g_tint_fn + 3);
    uint8_t* go = call_target(g_bloom_fn + 0xB);
    if (!gc || !go || !sig_match(gc, "A1 ?? ?? ?? ?? C3") || !sig_match(go, "A1 ?? ?? ?? ?? 85 C0 75")) {
        log_line("  X2-DIAG-FLASH: context getters not as proven - off");
        return false;
    }
    g_ctx_ptr = (uint8_t**)abs_operand("HD context", gc + 1);
    g_obj_ptr = (uint8_t**)abs_operand("bloom object", go + 1);
    if (!g_ctx_ptr || !g_obj_ptr) return false;
    log_line("  X2-DIAG-FLASH: tint 0x%08X bloom 0x%08X reset 0x%08X ctx [0x%08X] obj [0x%08X] (expected 0x004548E0 / 0x004546A0 / "
             "0x004AEC60, 0x00DD10F4 / 0x00DDC74C)", rt_to_gva(g_tint_fn), rt_to_gva(g_bloom_fn), rt_to_gva(g_reset_fn),
             rt_to_gva(g_ctx_ptr), rt_to_gva(g_obj_ptr));
    return true;
}

static void pre() {
    static bool done = false;
    if (done) return;
    done = true;                                 // game thread, between invocations
    bool ok = hook_entry("X2-DIAG-FLASH tint", g_tint_fn, (void*)&h_tint, (void**)&g_tint) &&
              hook_entry("X2-DIAG-FLASH bloom", g_bloom_fn, (void*)&h_bloom, (void**)&g_bloom) &&
              hook_entry("X2-DIAG-FLASH reset", g_reset_fn, (void*)&h_reset, (void**)&g_reset) &&
              hook_entry("X2-DIAG-FLASH packed tint", g_pk_fn, (void*)&h_pk, (void**)&g_pk);
    log_line("X2-DIAG-FLASH: hooks %s", ok ? "on" : "FAILED");
}

static void post() {
    Inv c = g_cur;
    g_cur = Inv{};
    g_inv++;
    bool any = c.tint_a != 0.0f || c.at_a != 0.0f || c.at_b5c != 0.0f || c.pk_exe || c.pk_dll;   // visible tint / bloom / packed tint
    if (!any || g_lines >= 1500 || !ftblink_gate()) return;
    g_lines++;
    log_line("FLASH #%ld %s | packed tint exe %d dll %d col %08X mode %d | tint set %d a %.3f | bloom set %d arg %d | at HD frame x%d: tint a %.3f lock %d armed %d bloom %d/%d lvl %.3f",
             g_inv, render_only() ? "R" : "L", c.pk_exe, c.pk_dll, c.pk_col, c.pk_mode, c.tint_n, (double)c.tint_a, c.bloom_n, c.bloom_arg, c.at_n, (double)c.at_a,
             c.at_lock, c.at_armed, c.at_b58, c.at_b59, (double)c.at_b5c);
}

Row row_DIAGFLASH = {
    "X2-DIAG-FLASH",
    "diagnostic: full-screen tint / bloom set and seen at the HD frame, per invocation (magic light flashes)",
    0, install, pre, post, 0
};

} // namespace x2
