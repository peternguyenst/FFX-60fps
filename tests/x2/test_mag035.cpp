// SPDX-License-Identifier: Apache-2.0
// Tests for X2-MAG-035 (rows/X2-MAG-035.cpp): pass a "walker template" spell DLL only when the blink detector has
// seen it blink. Uses the real spell DLLs from the game folder (loaded as data: no code of theirs runs); those tests
// are skipped when the game is not installed.
#include "test.h"
#include "fakes.h"
#include <stdio.h>
#include <stdlib.h>
#include <random>

using namespace x2;

namespace x2 { extern Row row_MAG035; }

static const char* kDefaultMagicDir =
    "G:\\SteamLibrary\\steamapps\\common\\FINAL FANTASY FFX&FFX-2 HD Remaster\\magicFiles\\FFX-2";

enum { V_SHORT = 1, V_SAME = 0, V_LONG = -1 };   // what X2-DIAG-BLINK reports per render-only frame

struct LoadedDll {
    HMODULE module = nullptr;
    char name[24] = {};
    uint8_t* base() const { return (uint8_t*)module; }
};

static const char* magic_dir() {
    static char dir[MAX_PATH];
    size_t got = 0;
    if (getenv_s(&got, dir, sizeof(dir), "X2_MAGIC_DIR") != 0 || got == 0) strcpy_s(dir, kDefaultMagicDir);
    return dir;
}

// As data: maps and relocates the image, but runs no DllMain and loads no imports.
static LoadedDll load_magic_file(const char* file_name) {
    LoadedDll d;
    char path[MAX_PATH];
    strncpy_s(d.name, file_name, _TRUNCATE);
    snprintf(path, sizeof(path), "%s\\%s", magic_dir(), file_name);
    d.module = LoadLibraryExA(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    return d;
}

static LoadedDll load_magic(int number) {
    char file_name[24];
    snprintf(file_name, sizeof(file_name), "magic_%04d.dll", number);
    return load_magic_file(file_name);
}

static void unload(LoadedDll& d) {
    fake::magic_on_unload(d.base());
    FreeLibrary(d.module);
    d.module = nullptr;
}

// Install the row with the given ini overrides; afterwards the fake knows its load/unload callbacks.
static void install_row() {
    row_MAG035.on = 1;
    g_disabled = 0;
    CHECK(row_MAG035.install());
    fake::passed.clear();
}

// Spell loaded, then `count` render-only frames with the given verdict.
static void report(const LoadedDll& d, int verdict, int count) {
    for (int i = 0; i < count; i++) mag035_pair(d.base(), verdict);
}

static bool open_or_skip(LoadedDll& d, int number) {
    d = load_magic(number);
    if (d.module) return true;
    t::skip("game spell DLLs not found (set X2_MAGIC_DIR)");
    return false;
}

TEST(mag035_passes_a_template_dll_once_it_blinks) {
    fake::reset();
    install_row();
    LoadedDll d;
    if (!open_or_skip(d, 386)) return;          // Requiem, a template DLL
    fake::magic_on_load(d.base(), 0, d.name);
    report(d, V_SHORT, 23);
    CHECK_EQ(fake::passed.size(), 0);           // the 24-frame window is not full yet
    report(d, V_SHORT, 1);
    CHECK_EQ(fake::passed.size(), 1);
    CHECK(fake::passed[0].base == d.base());
    CHECK(fake::log_contains("magic_0386.dll blinks"));
    report(d, V_SHORT, 100);
    CHECK_EQ(fake::passed.size(), 1);           // passed once, not again
    unload(d);
}

TEST(mag035_needs_20_of_24) {
    fake::reset();
    install_row();
    LoadedDll d;
    if (!open_or_skip(d, 386)) return;
    fake::magic_on_load(d.base(), 0, d.name);
    for (int i = 0; i < 50; i++) {              // 19 short + 5 same, over and over: never 20 in any window
        report(d, V_SHORT, 19);
        report(d, V_SAME, 5);
    }
    CHECK_EQ(fake::passed.size(), 0);
    report(d, V_SHORT, 20);                       // now the last 24 hold 20 shorts
    CHECK_EQ(fake::passed.size(), 1);
    unload(d);
}

TEST(mag035_long_frames_count_against_a_blink) {
    fake::reset();
    install_row();
    LoadedDll d;
    if (!open_or_skip(d, 386)) return;
    fake::magic_on_load(d.base(), 0, d.name);
    for (int i = 0; i < 20; i++) {              // 21 short + 3 long = 18 net: not enough
        report(d, V_SHORT, 21);
        report(d, V_LONG, 3);
    }
    CHECK_EQ(fake::passed.size(), 0);
    unload(d);
}

TEST(mag035_ignores_detector_noise) {
    // Measured 24/10/2026: with no spell on screen about a third of render-only frames come out "short" by chance,
    // a few percent "long". The old bar (6 of 12) passed such noise on most casts.
    fake::reset();
    install_row();
    LoadedDll d;
    if (!open_or_skip(d, 386)) return;
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    int noise_passes = 0;
    for (int cast = 0; cast < 50; cast++) {
        fake::magic_on_load(d.base(), 0, d.name);
        for (int frame = 0; frame < 300; frame++) {
            double r = u(rng);
            mag035_pair(d.base(), r < 0.36 ? V_SHORT : r < 0.41 ? V_LONG : V_SAME);
        }
        noise_passes += (int)fake::passed.size();
        fake::passed.clear();
        fake::magic_on_unload(d.base());
    }
    CHECK(noise_passes <= 1);                   // about 0.1% expected; 50 casts should give 0
    FreeLibrary(d.module);
}

TEST(mag035_the_old_6_of_12_pattern_no_longer_passes) {
    fake::reset();
    install_row();
    LoadedDll d;
    if (!open_or_skip(d, 946)) return;          // Glimmer of Despair: passed 31 times this way before 24/10
    fake::magic_on_load(d.base(), 0, d.name);
    for (int i = 0; i < 40; i++) {
        report(d, V_SHORT, 6);
        report(d, V_SAME, 6);
    }
    CHECK_EQ(fake::passed.size(), 0);
    unload(d);
}

TEST(mag035_never_passes_an_excluded_dll) {
    fake::reset();
    install_row();
    LoadedDll d;
    if (!open_or_skip(d, 449)) return;          // a template DLL that crashed once while passed
    fake::magic_on_load(d.base(), 0, d.name);
    report(d, V_SHORT, 100);
    CHECK_EQ(fake::passed.size(), 0);
    unload(d);
}

TEST(mag035_ini_keys_change_the_bar) {
    fake::reset();
    fake::set_ini("X2-MAG-035-PAIRS", 12);
    fake::set_ini("X2-MAG-035-NEED", 6);
    install_row();
    LoadedDll d;
    if (!open_or_skip(d, 386)) return;
    fake::magic_on_load(d.base(), 0, d.name);
    report(d, V_SHORT, 6);
    report(d, V_SAME, 6);
    CHECK_EQ(fake::passed.size(), 1);
    unload(d);
    fake::reset();
    install_row();                              // back to the defaults for the tests after this one
}

TEST(mag035_finds_exactly_the_310_template_dlls) {
    // Offline survey 22/10/2026: 310 of the 844 spell DLLs share the template draw function. 449 is excluded, so 309
    // can be passed. Every DLL gets a full blink; only templates may be passed. Slow the first time (antivirus scans
    // each DLL as it loads), so it runs only with --slow.
    if (!t::slow_tests()) { t::skip("slow: run x2_tests.exe --slow"); return; }
    fake::reset();
    install_row();
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\magic_*.dll", magic_dir());   // includes language copies (magic_0718_us)
    WIN32_FIND_DATAA found;
    HANDLE find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) { t::skip("game spell DLLs not found (set X2_MAGIC_DIR)"); return; }
    int dlls = 0, passed = 0;
    do {
        LoadedDll d = load_magic_file(found.cFileName);
        if (!d.module) continue;
        dlls++;
        fake::magic_on_load(d.base(), 0, d.name);
        report(d, V_SHORT, 24);
        passed += (int)fake::passed.size();
        fake::passed.clear();
        unload(d);
    } while (FindNextFileA(find, &found));
    FindClose(find);
    CHECK_EQ(dlls, 844);
    CHECK_EQ(passed, 309);
}

TEST(mag035_off_does_nothing) {
    fake::reset();
    install_row();
    LoadedDll d;
    if (!open_or_skip(d, 386)) return;
    fake::magic_on_load(d.base(), 0, d.name);
    row_MAG035.on = 0;
    report(d, V_SHORT, 100);
    CHECK_EQ(fake::passed.size(), 0);
    row_MAG035.on = 1;
    unload(d);
}
