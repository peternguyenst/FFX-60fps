// SPDX-License-Identifier: Apache-2.0
// fps60x2.log next to FFX-2.exe. Same line format as the FFX mod's fps60.log, separate file and state.
#include "x2.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace x2 {

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

// Dev hot reload: x2_reload.cpp loads each new copy from fps60x2_live_<tick>.dll, so a copy is a reload only when its
// own file has that name (as the FFX mod's hot_copy; an environment flag would survive into a restarted game).
bool hot_copy() {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&hot_copy, &self)) return false;
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const wchar_t* base = path;
    for (DWORD i = 0; i < n; i++) if (path[i] == L'\\' || path[i] == L'/') base = path + i + 1;
    return _wcsnicmp(base, L"fps60x2_live_", 13) == 0;
}

static void open_append(const wchar_t* dir) {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%sfps60x2.log", dir);
    g_log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log_file != INVALID_HANDLE_VALUE) SetFilePointer(g_log_file, 0, nullptr, FILE_END);
}

void log_open() {
    if (!g_log_lock_ready) { InitializeCriticalSection(&g_log_lock); g_log_lock_ready = true; }
    wchar_t dir[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    game_dir(dir, MAX_PATH);
    if (!dir[0]) return;
    if (hot_copy()) { open_append(dir); return; }   // the previous copy closed the log: continue the same file
    for (int i = 3; i >= 1; i--) {                   // fps60x2.log -> .1 -> .2 -> .3
        if (i == 1) _snwprintf_s(from, MAX_PATH, _TRUNCATE, L"%sfps60x2.log", dir);
        else _snwprintf_s(from, MAX_PATH, _TRUNCATE, L"%sfps60x2.%d.log", dir, i - 1);
        _snwprintf_s(to, MAX_PATH, _TRUNCATE, L"%sfps60x2.%d.log", dir, i);
        MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
    }
    _snwprintf_s(from, MAX_PATH, _TRUNCATE, L"%sfps60x2.log", dir);
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

void log_close() {
    if (!g_log_lock_ready) return;
    EnterCriticalSection(&g_log_lock);
    if (g_log_file != INVALID_HANDLE_VALUE) { CloseHandle(g_log_file); g_log_file = INVALID_HANDLE_VALUE; }
    LeaveCriticalSection(&g_log_lock);
}

// Dev hot reload: a failed load gives the log back to the passive copy.
void log_reopen() {
    if (!g_log_lock_ready || g_log_file != INVALID_HANDLE_VALUE) return;
    wchar_t dir[MAX_PATH];
    game_dir(dir, MAX_PATH);
    if (!dir[0]) return;
    EnterCriticalSection(&g_log_lock);
    open_append(dir);
    LeaveCriticalSection(&g_log_lock);
}

} // namespace x2
