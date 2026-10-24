// SPDX-License-Identifier: Apache-2.0
// x2_tests.exe: unit tests for the FFX-2 mod's logic, run without the game.
// Build: MSBuild x2_tests.vcxproj /p:Configuration=Release /p:Platform=Win32
// Run:   artifacts\tests\Release\x2_tests.exe [--slow]   (exit code 0 = all passed; --slow adds the census of every
//        installed spell DLL, which takes minutes the first time)
// Tests that need the installed game files (magic DLLs) are skipped when the files are missing; set X2_MAGIC_DIR to
// point at another magicFiles\FFX-2 folder.
#include "test.h"
#include <string.h>

namespace t {

static TestCase* g_first = nullptr;
static TestCase** g_tail = &g_first;
static int g_failures_in_test = 0;
static bool g_skipped = false;
static bool g_slow = false;

bool slow_tests() { return g_slow; }

void add(TestCase* tc) { *g_tail = tc; g_tail = &tc->next; }

void fail(const char* file, int line, const char* what) {
    const char* short_name = strrchr(file, '\\');
    printf("    FAIL %s:%d: %s\n", short_name ? short_name + 1 : file, line, what);
    g_failures_in_test++;
}

void skip(const char* why) {
    printf("    skipped: %s\n", why);
    g_skipped = true;
}

} // namespace t

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // print as we go, so a hang shows which test it is in
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--slow") == 0) t::g_slow = true;
    int passed = 0, failed = 0, skipped = 0;
    for (t::TestCase* tc = t::g_first; tc; tc = tc->next) {
        printf("%s\n", tc->name);
        t::g_failures_in_test = 0;
        t::g_skipped = false;
        tc->fn();
        if (t::g_failures_in_test) failed++;
        else if (t::g_skipped) skipped++;
        else passed++;
    }
    printf("\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
    return failed ? 1 : 0;
}
