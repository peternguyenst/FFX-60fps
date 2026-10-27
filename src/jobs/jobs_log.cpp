// SPDX-License-Identifier: Apache-2.0
// jobs.log next to FFX.exe. Same line format as fps60.log / fps60x2.log, separate file and state.
#include "jobs.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace jobs {

static HANDLE           g_log_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_lock;
static bool             g_log_lock_ready = false;

void game_dir(wchar_t* out, size_t cap) {
    out[0] = 0;
    DWORD n = GetModuleFileNameW(nullptr, out, (DWORD)cap);
    if (n == 0 || n >= cap) { out[0] = 0; return; }
    for (DWORD i = n; i > 0; i--) if (out[i - 1] == L'\\' || out[i - 1] == L'/') { out[i] = 0; return; }
    out[0] = 0;
}

void log_open() {
    if (!g_log_lock_ready) { InitializeCriticalSection(&g_log_lock); g_log_lock_ready = true; }
    wchar_t dir[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    game_dir(dir, MAX_PATH);
    if (!dir[0]) return;
    for (int i = 3; i >= 1; i--) {                   // jobs.log -> .1 -> .2 -> .3
        if (i == 1) _snwprintf_s(from, MAX_PATH, _TRUNCATE, L"%sjobs.log", dir);
        else _snwprintf_s(from, MAX_PATH, _TRUNCATE, L"%sjobs.%d.log", dir, i - 1);
        _snwprintf_s(to, MAX_PATH, _TRUNCATE, L"%sjobs.%d.log", dir, i);
        MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
    }
    _snwprintf_s(from, MAX_PATH, _TRUNCATE, L"%sjobs.log", dir);
    g_log_file = CreateFileW(from, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void log_line(const char* fmt, ...) {
    if (g_log_file == INVALID_HANDLE_VALUE || !g_log_lock_ready) return;
    char buf[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond,
                        t.wMilliseconds);
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf_s(buf + n, sizeof(buf) - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    n = (m < 0) ? (int)strlen(buf) : n + m;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_log_lock);
    DWORD w;
    if (g_log_file != INVALID_HANDLE_VALUE) WriteFile(g_log_file, buf, (DWORD)n, &w, nullptr);
    LeaveCriticalSection(&g_log_lock);
}

} // namespace jobs
