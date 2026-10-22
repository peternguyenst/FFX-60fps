// SPDX-License-Identifier: Apache-2.0
// X2-MAG-033: magic_0146 node update FUN_10001b00 (cdecl, plain RET) crash. State 1 (node +0x18 == 1) reads the
// pointer at node +0x1080 (field +0x30 of the "EgoThCst" sub-object at +0x1050) and tests word [ptr+0x3E] & 0x2000;
// the pointer is filled by the sub-object's draw callback FUN_10001390 (svc +0xA84 / +0xA88 on sub+0x30). If logic
// reaches state 1 before any draw filled it, MOVZX EAX,word [EAX+0x3E] at 0x10001B5E reads 0x3E: U 22/10/2026
// 21:32:13 (D fps60x2.log, X2-DIAG-CRASH: 0xC0000005 at magic_0146.dll+0x1B5E, read 0x0000003E, EAX 0; inv kind
// LOGIC), and the friend's ~30 min battle crashes (log ends ~10 s after magic_0146 loaded).
// Fix: while the pointer is null, take the stock wait path the function already has for "bit 0x2000 clear" (JZ ->
// 0x10001ECB: INC dword [node+0x2C]; RET), so the node waits until a draw has filled it. Only magic_0146 contains this
// code (byte scan of the 844 FFX-2 magic DLLs, 22/10).
// Evidence: outer repo re/ledger/CRASH-0146.md (S + D).
#include "../x2.h"
#include <string.h>

namespace x2 {

typedef void(__cdecl* upd_t)(int* p);

extern Row row_MAG033;
static uint8_t* g_mod = nullptr;
static uint8_t* g_entry = nullptr;
static upd_t g_orig = nullptr;
static volatile LONG g_waits = 0;

// FUN_10001b00 prologue: PUSH EBP; MOV EBP,ESP; AND ESP,-8; SUB ESP,0x2C; MOV EDX,[EBP+8]; PUSH EBX/ESI/EDI;
// MOV EDI,[EDX+4]; MOV [ESP+0xC],EDI; MOV EAX,[EDI+0x18]
static const uint8_t SIG_ENTRY[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x2C, 0x8B, 0x55, 0x08, 0x53, 0x56,
    0x57, 0x8B, 0x7A, 0x04, 0x89, 0x7C, 0x24, 0x0C, 0x8B, 0x47, 0x18};
// +0x54: MOV EAX,[EDI+0x1080]; MOV [ESP+0x10],EAX; MOVZX EAX,word [EAX+0x3E]; AND EAX,0x2000; MOV [ESP+0x18],EAX; JZ
static const uint8_t SIG_STATE1[] = {0x8B, 0x87, 0x80, 0x10, 0x00, 0x00, 0x89, 0x44, 0x24, 0x10, 0x0F, 0xB7, 0x40, 0x3E,
    0x25, 0x00, 0x20, 0x00, 0x00, 0x89, 0x44, 0x24, 0x18, 0x0F, 0x84};

static void __cdecl h_upd(int* p) {
    if (row_MAG033.on && !g_disabled) {
        __try {
            uint8_t* node = (uint8_t*)(uintptr_t)p[1];
            if (*(int*)(node + 0x18) == 1 && *(void**)(node + 0x1080) == nullptr) {
                ++*(int*)(node + 0x2C);                  // the stock wait path
                if (InterlockedIncrement(&g_waits) <= 5)
                    log_line("X2-MAG-033: magic_0146 node %p in state 1 with no draw data yet - waits (%ld)", node, g_waits);
                return;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_orig(p);
}

static void on_load(uint8_t* mod, uint32_t size, const char* name) {
    if (!row_MAG033.on || g_disabled || _stricmp(name, "magic_0146.dll") != 0) return;
    uint8_t* e = mod + 0x1B00;
    bool ok = false;
    __try {
        ok = size > 0x1B80 && memcmp(e, SIG_ENTRY, sizeof(SIG_ENTRY)) == 0 &&
             memcmp(e + 0x54, SIG_STATE1, sizeof(SIG_STATE1)) == 0 && (e[-1] == 0xCC || e[-1] == 0xC3);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok) { log_line("X2-MAG-033: %s FUN_10001b00 bytes mismatch - refused", name); return; }
    g_mod = mod;
    g_entry = e;
    if (!hook_entry("X2-MAG-033 node update", e, (void*)&h_upd, (void**)&g_orig)) {
        log_line("X2-MAG-033: %s hook failed - refused", name);
        g_mod = g_entry = nullptr;
        return;
    }
    log_line("X2-MAG-033: %s hooked: node update +0x1B00 (gva 0x10001B00)", name);
}

static void on_unload(uint8_t* mod) {
    if (!g_mod || mod != g_mod) return;
    hook_remove(g_entry);
    log_line("X2-MAG-033: magic_0146.dll unloaded (state-1 waits for draw data so far %ld)", g_waits);
    g_mod = g_entry = nullptr;
    g_orig = nullptr;
}

static bool install() {
    if (!magic_mods_subscribe("X2-MAG-033", on_load, on_unload)) return false;
    log_line("  X2-MAG-033: magic_0146 node update hooked per load");
    return true;
}

Row row_MAG033 = {
    "X2-MAG-033",
    "magic_0146 node update: state 1 waits until its draw data exists (null read crash in long battles)",
    1, install, nullptr, nullptr, 0
};

} // namespace x2
