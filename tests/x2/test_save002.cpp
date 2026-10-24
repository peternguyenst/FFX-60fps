// SPDX-License-Identifier: Apache-2.0
// Tests for X2-SAVE-002 (more save slots): the slot-count signature and all 22 table references checked against the
// installed FFX-2.exe (mapped as an image, not run), and the slot count from the ini. The row's .cpp is included so its
// static helpers can be called.
#include "test.h"
#include "fakes.h"
#include "../../src/x2/rows/X2-SAVE-002.cpp"

using namespace x2;

static const char* kExe = "G:\\SteamLibrary\\steamapps\\common\\FINAL FANTASY FFX&FFX-2 HD Remaster\\FFX-2.exe";
static const uint32_t IMAGE_BASE_GVA = 0x400000;

TEST(save002_sites_in_the_installed_exe) {
    HMODULE h = LoadLibraryExA(kExe, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) { t::skip("FFX-2.exe not found"); return; }
    uint8_t* image = (uint8_t*)((uintptr_t)h & ~(uintptr_t)3);
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(image + ((IMAGE_DOS_HEADER*)image)->e_lfanew);
    IMAGE_SECTION_HEADER* text = IMAGE_FIRST_SECTION(nt);
    BytePattern p;
    CHECK(parse_pattern(SIG_SLOT_COUNT, &p));
    uint8_t* anchor = nullptr;
    int hits = count_hits(image + text->VirtualAddress, image + text->VirtualAddress + text->Misc.VirtualSize, p,
                          &anchor);
    CHECK_EQ(hits, 1);
    if (hits == 1) {
        CHECK_EQ(anchor - image + IMAGE_BASE_GVA, SLOT_COUNT_GVA);
        Sites s;
        CHECK(find_sites(anchor, &s));
        // absolute operands move with the image's relocation; compare them with the slot count's (gva 0x00DEB39C)
        uint32_t count_var = read_u32(anchor + 0x14);
        CHECK_EQ(s.old_slot_table - count_var, 0x00DEB3D0 - 0x00DEB39C);
        CHECK_EQ(s.old_in_use_table - count_var, 0x00DEBD30 - 0x00DEB39C);
        CHECK_EQ(s.count_value[0] - image + IMAGE_BASE_GVA, 0x51D262);
        CHECK_EQ(s.count_value[1] - image + IMAGE_BASE_GVA, 0x51D878);
    }
    FreeLibrary(h);
}

TEST(save002_slot_count_limits) {
    CHECK_EQ(clamp_slots(999), 999);
    CHECK_EQ(clamp_slots(50), STOCK_SLOTS);
    CHECK_EQ(clamp_slots(100000), MAX_SLOTS);
}
