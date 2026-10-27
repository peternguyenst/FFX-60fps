// SPDX-License-Identifier: Apache-2.0
// jobs.ini next to FFX.exe, read once at init. The file is optional: missing keys take the defaults below.
#include "jobs.h"
#include <stdio.h>

namespace jobs {

Config g_cfg;

void config_load() {
    wchar_t dir[MAX_PATH], ini[MAX_PATH];
    game_dir(dir, MAX_PATH);
    _snwprintf_s(ini, MAX_PATH, _TRUNCATE, L"%sjobs.ini", dir);
    g_cfg.enabled = GetPrivateProfileIntW(L"jobs", L"enabled", 1, ini);
    g_cfg.unpack_timeout_s = GetPrivateProfileIntW(L"jobs", L"unpack_timeout_s", 60, ini);
    g_cfg.script_poll_ms = GetPrivateProfileIntW(L"jobs", L"script_poll_ms", 500, ini);
    if (g_cfg.script_poll_ms < 100) g_cfg.script_poll_ms = 100;
}

} // namespace jobs
