// SPDX-License-Identifier: Apache-2.0
// fps60x2.ini next to FFX-2.exe, read once at init. [x2] global switches, [rows] one key per X2- row.
// The file is optional: missing keys take the defaults below.
#include "x2.h"
#include <stdio.h>
#include <string.h>

namespace x2 {

Config g_cfg;
static wchar_t g_ini[MAX_PATH];

void config_load() {
    wchar_t dir[MAX_PATH];
    game_dir(dir, MAX_PATH);
    _snwprintf_s(g_ini, MAX_PATH, _TRUNCATE, L"%sfps60x2.ini", dir);
    g_cfg.enabled = GetPrivateProfileIntW(L"x2", L"enabled", 1, g_ini);
    g_cfg.pacing = GetPrivateProfileIntW(L"x2", L"pacing", 1, g_ini);
    g_cfg.exec_method = GetPrivateProfileIntW(L"x2", L"exec_method", 0, g_ini);
    g_cfg.unpack_timeout_s = GetPrivateProfileIntW(L"x2", L"unpack_timeout_s", 60, g_ini);
    g_cfg.log_pacing_events = GetPrivateProfileIntW(L"x2", L"log_pacing_events", 1, g_ini);
}

bool config_write(const char* sect, const char* key, const char* value) {
    wchar_t s[32], k[64], v[96];
    _snwprintf_s(s, 32, _TRUNCATE, L"%S", sect);
    _snwprintf_s(k, 64, _TRUNCATE, L"%S", key);
    _snwprintf_s(v, 96, _TRUNCATE, L"%S", value);
    return WritePrivateProfileStringW(s, k, v, g_ini) != 0;
}

// ASCII string key (wide API: the game path may hold non-ANSI characters).
void config_str(const char* sect, const char* key, char* out, int size, const char* def) {
    wchar_t s[32], k[64], d[96], v[96];
    _snwprintf_s(s, 32, _TRUNCATE, L"%S", sect);
    _snwprintf_s(k, 64, _TRUNCATE, L"%S", key);
    _snwprintf_s(d, 96, _TRUNCATE, L"%S", def);
    GetPrivateProfileStringW(s, k, d, v, 96, g_ini);
    int i = 0;
    for (; i < size - 1 && v[i]; i++) out[i] = (v[i] < 0x80) ? (char)v[i] : '?';
    out[i] = 0;
}

// Running under Wine / Proton (ntdll exports wine_get_version). Rows whose cost exists only behind Proton's slower
// present path (Steam Deck) default on there and off on Windows.
bool on_wine() {
    static int w = -1;
    if (w < 0) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        w = (nt && GetProcAddress(nt, "wine_get_version")) ? 1 : 0;
    }
    return w != 0;
}

int config_row(const char* row_id, int def) {
    // user-facing switches in [x2] that stand for a row: soft_reset=<buttons>|0|off (X2-SR-003), as FFX [fps60]
    if (!strcmp(row_id, "X2-SR-003")) {
        char b[96];
        config_str("x2", "soft_reset", b, sizeof(b), "");
        if (b[0]) return !(strcmp(b, "0") == 0 || _stricmp(b, "off") == 0);
    }
    wchar_t key[64];
    _snwprintf_s(key, 64, _TRUNCATE, L"%S", row_id);
    return GetPrivateProfileIntW(L"rows", key, def, g_ini);
}

} // namespace x2
