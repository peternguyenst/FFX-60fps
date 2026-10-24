// SPDX-License-Identifier: Apache-2.0
// Census over every installed spell DLL (--slow): X2-MAG-030's DLL-load logic must recognise exactly the timeline
// drivers its notes count (71 shape A + 11 shape B + 10 shape C = 92), and X2-MAG-016's (in test_dll_scanner.cpp's
// translation unit) exactly the 79 record drivers. A DLL "is recognised" when every signature check passed and the row
// went on to hook it (the fake hook_entry refuses, which the row logs as "driver hook failed").
#include "test.h"
#include "fakes.h"
#include "../../src/x2/rows/X2-MAG-030.cpp"

using namespace x2;

static const char* kDir = "G:\\SteamLibrary\\steamapps\\common\\FINAL FANTASY FFX&FFX-2 HD Remaster\\magicFiles\\FFX-2";

// Load every magic_*.dll as data and pass it to on_load; returns the number of DLLs, or -1 if none are installed.
typedef void (*load_fn)(uint8_t*, uint32_t, const char*);
int census(load_fn on_load_fn, const char* recognised_text, int* recognised) {
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\magic_*.dll", kDir);
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA(pattern, &f);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int dlls = 0;
    *recognised = 0;
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", kDir, f.cFileName);
        HMODULE m = LoadLibraryExA(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) continue;
        dlls++;
        IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)((uint8_t*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
        fake::log.clear();
        on_load_fn((uint8_t*)m, nt->OptionalHeader.SizeOfImage, f.cFileName);
        if (fake::log_contains(recognised_text)) (*recognised)++;
        FreeLibrary(m);
    } while (FindNextFileA(h, &f));
    FindClose(h);
    return dlls;
}

TEST(mag030_recognises_the_92_timeline_drivers) {
    if (!t::slow_tests()) { t::skip("slow: run x2_tests.exe --slow"); return; }
    row_MAG030.on = 1;
    g_disabled = 0;
    int recognised = 0;
    int dlls = census(on_load, "driver hook failed", &recognised);
    if (dlls < 0) { t::skip("game spell DLLs not found"); return; }
    CHECK_EQ(dlls, 844);
    CHECK_EQ(recognised, 92);
}
