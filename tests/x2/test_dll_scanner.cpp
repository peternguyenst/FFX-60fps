// SPDX-License-Identifier: Apache-2.0
// Tests for the shared signature scanner, x2_pattern.cpp (BytePattern, parse_pattern, pattern_matches, scan_module,
// matches_at): pattern text, wildcards, exactly-one-hit counting, executable sections only. Run against real spell
// DLLs where the game is installed. Written against the old copy in X2-MAG-016 before the three copies were merged;
// X2-MAG-016.cpp is included for its driver signatures and its DLL-load logic (the census at the end).
#include "test.h"
#include "fakes.h"
#include "../../src/x2/rows/X2-MAG-016.cpp"
#include <string>

using namespace x2;

static const char* kMagicDir =
    "G:\\SteamLibrary\\steamapps\\common\\FINAL FANTASY FFX&FFX-2 HD Remaster\\magicFiles\\FFX-2";

struct Dll {
    HMODULE m = nullptr;
    uint32_t size = 0;
    explicit Dll(int number) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\magic_%04d.dll", kMagicDir, number);
        m = LoadLibraryExA(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (m) {
            IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)((uint8_t*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
            size = nt->OptionalHeader.SizeOfImage;
        }
    }
    ~Dll() { if (m) FreeLibrary(m); }
    uint8_t* base() const { return (uint8_t*)m; }
};

// "AB CD .." for the bytes of a string (to look for data, which must not be found by a code scan)
static std::string hex_of(const char* s) {
    std::string out;
    char b[4];
    for (; *s; s++) { snprintf(b, sizeof(b), "%02X ", (uint8_t)*s); out += b; }
    return out;
}

static std::string bytes_pattern(int n) {           // "00 00 ..." with n bytes
    std::string out;
    for (int i = 0; i < n; i++) out += "00 ";
    return out;
}

TEST(scanner_parses_bytes_and_wildcards) {
    BytePattern p;
    CHECK(parse_pattern("55 8B ?? ec", &p));
    CHECK_EQ(p.len, 4);
    CHECK_EQ(p.b[0], 0x55); CHECK_EQ(p.m[0], 1);
    CHECK_EQ(p.b[1], 0x8B); CHECK_EQ(p.m[1], 1);
    CHECK_EQ(p.m[2], 0);                                // ?? = any byte
    CHECK_EQ(p.b[3], 0xEC); CHECK_EQ(p.m[3], 1);        // lower case is fine
}

TEST(scanner_rejects_bad_patterns) {
    BytePattern p;
    CHECK(!parse_pattern("", &p));
    CHECK(!parse_pattern("   ", &p));
    CHECK(!parse_pattern("5G", &p));
    CHECK(!parse_pattern("5", &p));                     // half a byte
}

TEST(scanner_pattern_length_limit) {
    BytePattern p;
    CHECK(parse_pattern(bytes_pattern(sizeof(p.b)).c_str(), &p));
    CHECK(!parse_pattern(bytes_pattern(sizeof(p.b) + 1).c_str(), &p));
}

TEST(scanner_matches_with_wildcards) {
    const uint8_t code[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08 };
    CHECK(matches_at(code, "55 8B EC"));
    CHECK(matches_at(code, "55 ?? EC 83"));
    CHECK(!matches_at(code, "55 8B ED"));
    CHECK(!matches_at(code, "zz"));                     // a bad pattern never matches
}

TEST(scanner_rejects_a_buffer_that_is_not_a_dll) {
    uint8_t junk[4096] = {};
    uint8_t* first = (uint8_t*)1;
    CHECK_EQ(scan_module(junk, sizeof(junk), "55 8B EC", &first), -1);
    CHECK(first == nullptr);
}

TEST(scanner_finds_the_record_driver_exactly_once) {
    Dll d(221);                                         // the record-driver family's own DLL
    if (!d.m) { t::skip("game spell DLLs not found"); return; }
    uint8_t* gate = nullptr;
    CHECK_EQ(scan_module(d.base(), d.size, SIG_GATE, &gate), 1);
    CHECK(gate != nullptr);
    CHECK(matches_at(gate - OFF_GATE, PRO_ENTRY));     // the driver entry sits at the fixed offset
    CHECK(matches_at(gate - OFF_GATE + OFF_EPI, PRO_EPI));
}

TEST(scanner_counts_no_hit_and_more_than_one) {
    Dll d(221);
    if (!d.m) { t::skip("game spell DLLs not found"); return; }
    uint8_t* first = nullptr;
    CHECK_EQ(scan_module(d.base(), d.size, "DE AD BE EF 13 37 C0 DE 99", &first), 0);
    CHECK(first == nullptr);
    CHECK_EQ(scan_module(d.base(), d.size, "55 8B EC", &first), 2);   // every function starts so: stops at 2
    CHECK(first != nullptr);
    Dll other(386);                                     // a template DLL: no record driver
    if (other.m) CHECK_EQ(scan_module(other.base(), other.size, SIG_GATE, &first), 0);
}

TEST(scanner_looks_at_code_only) {
    Dll d(646);                                         // a cross-fade DLL: the marker string is in its data
    if (!d.m) { t::skip("game spell DLLs not found"); return; }
    const char* marker = "kohno corss fade begin";
    bool in_image = false;
    for (uint32_t i = 0; i + strlen(marker) <= d.size && !in_image; i++)
        in_image = memcmp(d.base() + i, marker, strlen(marker)) == 0;
    CHECK(in_image);                                    // the bytes are there ...
    uint8_t* first = nullptr;
    CHECK_EQ(scan_module(d.base(), d.size, hex_of(marker).c_str(), &first), 0);   // ... but not in code
}

// Census (--slow): X2-MAG-016's DLL-load logic recognises exactly the 79 record-driver DLLs (census() is in
// test_dll_census.cpp).
typedef void (*load_fn)(uint8_t*, uint32_t, const char*);
int census(load_fn on_load_fn, const char* recognised_text, int* recognised);

TEST(mag016_recognises_the_79_record_drivers) {
    if (!t::slow_tests()) { t::skip("slow: run x2_tests.exe --slow"); return; }
    row_MAG016.on = 1;
    g_disabled = 0;
    int recognised = 0;
    int dlls = census(on_load, "driver hook failed", &recognised);
    if (dlls < 0) { t::skip("game spell DLLs not found"); return; }
    CHECK_EQ(dlls, 844);
    CHECK_EQ(recognised, 79);
}
