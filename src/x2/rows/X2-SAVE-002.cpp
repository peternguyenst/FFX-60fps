// SPDX-License-Identifier: Apache-2.0
// X2-SAVE-002: more save slots (999 by default instead of 200).
//
// The save module keeps one entry per slot in two fixed tables sized for 200 slots: the slot table (file number, date,
// time; 12 bytes per slot) and the in-use table (4 bytes per slot). The slot count is set to 200 (100 in Last Mission)
// each time the save or load screen opens (FUN_0051d250, FUN_0051d850). This row moves both tables into this DLL,
// sized for MAX_SLOTS, by rewriting the 22 instructions that address them (all in the save module, FUN_0051d0f0 ..
// FUN_0051e420), and raises the count to X2-SAVE-002-SLOTS; Last Mission gets that count minus 100, as in the stock
// game. Files stay ffx2_NNN (the game prints the number with %03d, so 1000 and above get four digits). Every site is
// checked byte for byte, and every operand must point into the old tables, before anything is written.
// Evidence: re/ledger/SAVE.md.
#include "../x2.h"
#include <string.h>

namespace x2 {

static const int STOCK_SLOTS = 200;
static const int MAX_SLOTS = 9999;
static const int DEFAULT_SLOTS = 999;

struct SlotEntry { int32_t file; int32_t date; int32_t time; };     // the game's 12-byte slot table entry
static SlotEntry g_slot_table[MAX_SLOTS];
static int32_t   g_slot_in_use[MAX_SLOTS];

enum Table { SLOT_TABLE, IN_USE_TABLE };

// One instruction that addresses a table: its gva, its bytes with the table address as ??, where that address sits in
// the instruction, which table and the byte offset into it the instruction uses.
struct TableRef { uint32_t gva; const char* bytes; uint8_t operand_at; uint8_t table; uint8_t field; };
static const TableRef TABLE_REFS[] = {
    // FUN_0051d0f0 (load a save)
    { 0x51D103, "83 3D ?? ?? ?? ?? 00",          2, SLOT_TABLE,   0x0 },   // CMP [slot 0 file],0
    { 0x51D114, "FF 34 85 ?? ?? ?? ??",          3, SLOT_TABLE,   0x0 },   // PUSH [EAX*4 + file]
    // FUN_0051d930 (write a save)
    { 0x51D961, "83 3C 85 ?? ?? ?? ?? 00",       3, IN_USE_TABLE, 0x0 },   // CMP [EAX*4 + in use],0
    { 0x51D97B, "FF 34 85 ?? ?? ?? ??",          3, SLOT_TABLE,   0x0 },
    // FUN_0051dae0 (scan the save folder)
    { 0x51DBED, "89 0C 95 ?? ?? ?? ??",          3, SLOT_TABLE,   0x4 },   // date
    { 0x51DC02, "89 34 95 ?? ?? ?? ??",          3, SLOT_TABLE,   0x0 },   // file
    { 0x51DC1A, "C7 04 B5 ?? ?? ?? ?? 01 00 00 00", 3, IN_USE_TABLE, 0x0 },
    { 0x51DC28, "89 04 95 ?? ?? ?? ??",          3, SLOT_TABLE,   0x8 },   // time
    { 0x51DC49, "C7 04 B5 ?? ?? ?? ?? FF FF FF FF", 3, IN_USE_TABLE, 0x0 },
    { 0x51DC54, "C7 04 85 ?? ?? ?? ?? FF FF FF FF", 3, SLOT_TABLE,   0x0 },
    { 0x51DC5F, "C7 04 85 ?? ?? ?? ?? FF FF FF FF", 3, SLOT_TABLE,   0x4 },
    { 0x51DC6A, "C7 04 85 ?? ?? ?? ?? FF FF FF FF", 3, SLOT_TABLE,   0x8 },
    // FUN_0051dd00 (build the list)
    { 0x51DD9C, "FF B7 ?? ?? ?? ??",             2, SLOT_TABLE,   0x0 },   // PUSH [EDI + file]
    { 0x51DE01, "FF B7 ?? ?? ?? ??",             2, SLOT_TABLE,   0x0 },
    { 0x51DE86, "8B 8F ?? ?? ?? ??",             2, SLOT_TABLE,   0x8 },
    { 0x51DE9F, "8B 9B ?? ?? ?? ??",             2, SLOT_TABLE,   0x4 },
    { 0x51E17F, "8B 8F ?? ?? ?? ??",             2, SLOT_TABLE,   0x8 },
    { 0x51E195, "8B 9B ?? ?? ?? ??",             2, SLOT_TABLE,   0x4 },
    // FUN_0051e420 (sort by date)
    { 0x51E43B, "C7 45 FC ?? ?? ?? ??",          3, SLOT_TABLE,   0x10 },  // MOV [EBP-4], &slot 1 date
    { 0x51E499, "89 04 8D ?? ?? ?? ??",          3, SLOT_TABLE,   0x0 },
    { 0x51E4A5, "89 04 8D ?? ?? ?? ??",          3, SLOT_TABLE,   0x4 },
    { 0x51E4B2, "89 04 8D ?? ?? ?? ??",          3, SLOT_TABLE,   0x8 },
};
static const int TABLE_REF_COUNT = sizeof(TABLE_REFS) / sizeof(TABLE_REFS[0]);

// FUN_0051d250: MOVSX EAX,[Last Mission flag]; NEG; SBB; AND EAX,-100; ADD EAX,200; MOV [slot count],EAX; MOV EAX,[EBP+8]
static const char* const SIG_SLOT_COUNT =
    "0F BE 05 ?? ?? ?? ?? F7 D8 1B C0 83 E0 9C 05 C8 00 00 00 A3 ?? ?? ?? ?? 8B 45 08";
static const uint32_t SLOT_COUNT_GVA = 0x51D253;
// The same count in FUN_0051d850 (the ADD's 4-byte value is what gets patched in both)
struct CountRef { uint32_t gva; const char* bytes; uint8_t value_at; };
static const CountRef COUNT_REFS[] = {
    { 0x51D25E, "83 E0 9C 05 C8 00 00 00 A3",    4 },
    { 0x51D874, "83 E0 9C 05 C8 00 00 00 56 A3", 4 },
};

struct Sites {
    uint8_t* table_operand[TABLE_REF_COUNT];
    uint8_t* count_value[2];
    uint32_t old_slot_table, old_in_use_table;
};

static uint32_t read_u32(const uint8_t* p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
// Address of a gva in the mapping where the slot-count signature hit is at `anchor`
static uint8_t* at_gva(uint8_t* anchor, uint32_t gva) { return anchor + (int32_t)(gva - SLOT_COUNT_GVA); }

// Checks every site against its expected bytes, starting from the slot-count signature hit `anchor`. The old tables
// are taken from the first slot-table and in-use references; every other operand must equal old table + field, and
// the in-use table must start right after 200 slot entries (the stock layout). False = touch nothing.
static bool find_sites(uint8_t* anchor, Sites* out) {
    out->old_slot_table = read_u32(at_gva(anchor, TABLE_REFS[0].gva) + TABLE_REFS[0].operand_at);
    out->old_in_use_table = read_u32(at_gva(anchor, TABLE_REFS[2].gva) + TABLE_REFS[2].operand_at);
    if (out->old_in_use_table != out->old_slot_table + STOCK_SLOTS * sizeof(SlotEntry)) {
        log_line("  X2-SAVE-002: in-use table 0x%08X does not follow 200 slots of 0x%08X - refused",
                 out->old_in_use_table, out->old_slot_table);
        return false;
    }
    for (int i = 0; i < TABLE_REF_COUNT; ++i) {
        const TableRef& r = TABLE_REFS[i];
        uint8_t* at = at_gva(anchor, r.gva);
        uint32_t expected = (r.table == SLOT_TABLE ? out->old_slot_table : out->old_in_use_table) + r.field;
        if (!matches_at(at, r.bytes) || read_u32(at + r.operand_at) != expected) {
            log_line("  X2-SAVE-002: table reference at gva=0x%08X not as expected - refused", r.gva);
            return false;
        }
        out->table_operand[i] = at + r.operand_at;
    }
    for (int i = 0; i < 2; ++i) {
        uint8_t* at = at_gva(anchor, COUNT_REFS[i].gva);
        if (!matches_at(at, COUNT_REFS[i].bytes)) {
            log_line("  X2-SAVE-002: slot count at gva=0x%08X not as expected - refused", COUNT_REFS[i].gva);
            return false;
        }
        out->count_value[i] = at + COUNT_REFS[i].value_at;
    }
    return true;
}

static int clamp_slots(int n) { return n < STOCK_SLOTS ? STOCK_SLOTS : n > MAX_SLOTS ? MAX_SLOTS : n; }

static bool install() {
    uint8_t* anchor = sig_find("X2-SAVE-002 slot count (FUN_0051d250)", SIG_SLOT_COUNT, 0x51D253);
    if (!anchor) return false;
    Sites s;
    if (!find_sites(anchor, &s)) return false;
    int slots = clamp_slots(config_row("X2-SAVE-002-SLOTS", DEFAULT_SLOTS));
    // carry over whatever the game already put in the old tables, then point every reference at the new ones
    memset(g_slot_table, 0xFF, sizeof g_slot_table);
    memset(g_slot_in_use, 0xFF, sizeof g_slot_in_use);
    if (!safe_copy(g_slot_table, (const void*)(uintptr_t)s.old_slot_table, STOCK_SLOTS * sizeof(SlotEntry)) ||
        !safe_copy(g_slot_in_use, (const void*)(uintptr_t)s.old_in_use_table, STOCK_SLOTS * sizeof(int32_t))) {
        log_line("  X2-SAVE-002: old tables unreadable - refused");
        return false;
    }
    for (int i = 0; i < TABLE_REF_COUNT; ++i) {
        const TableRef& r = TABLE_REFS[i];
        uint8_t* table = r.table == SLOT_TABLE ? (uint8_t*)g_slot_table : (uint8_t*)g_slot_in_use;
        if (!patch_u32(s.table_operand[i], (uint32_t)(uintptr_t)(table + r.field))) {
            log_line("  X2-SAVE-002: patch at gva=0x%08X failed - the %d already moved are put back", r.gva, i);
            for (int j = 0; j < i; ++j) {
                const TableRef& back = TABLE_REFS[j];
                patch_u32(s.table_operand[j],
                          (back.table == SLOT_TABLE ? s.old_slot_table : s.old_in_use_table) + back.field);
            }
            return false;
        }
    }
    // the moved tables already work with 200 slots; a failed count write only keeps that count
    if (!patch_u32(s.count_value[0], (uint32_t)slots) || !patch_u32(s.count_value[1], (uint32_t)slots)) {
        patch_u32(s.count_value[0], (uint32_t)STOCK_SLOTS);
        log_line("  X2-SAVE-002: slot count patch failed - tables moved, 200 slots kept");
        return true;
    }
    log_line("  X2-SAVE-002: %d table references moved from gva=0x%08X / 0x%08X (expected 0x00DEB3D0 / 0x00DEBD30) "
             "to this DLL; %d save slots (%d in Last Mission)",
             TABLE_REF_COUNT, s.old_slot_table, s.old_in_use_table, slots, slots - 100);
    return true;
}

Row row_SAVE002 = {
    "X2-SAVE-002",
    "more save slots: slot tables moved into this DLL, count X2-SAVE-002-SLOTS (default 999, stock 200)",
    1, install, nullptr, nullptr, 0
};

} // namespace x2
