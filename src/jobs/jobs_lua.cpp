// SPDX-License-Identifier: Apache-2.0
// Lua 5.4 host. Runs jobs/scripts/main.lua at start and again whenever a file in jobs/scripts changes (a fresh Lua
// state each time, so a reload never sees the previous script's globals). Scripts are read through the wide file
// API (the game path may hold non-ANSI characters); `require "name"` loads jobs/scripts/name.lua the same way.
// Script API (table `jobs`):
//   jobs.log(...)            one line in jobs.log, arguments joined with tostring and a space
//   jobs.ply(i)              snapshot of party record i (0..PLY_COUNT-1) as a table, or nil if unreadable
//   jobs.PLY_COUNT           number of records
// The Lua state is used only under g_lua_lock: later hooks call into it from the game thread.
#include "jobs.h"
#include <stdio.h>
#include <string.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace jobs {

static CRITICAL_SECTION g_lua_lock;
static lua_State*       g_L = nullptr;
static wchar_t          g_script_dir[MAX_PATH];   // <game>\jobs\scripts\  (trailing '\')

// Whole file as bytes; caller frees with HeapFree. False if missing or unreadable.
static bool read_file(const wchar_t* path, char** data, DWORD* size) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = GetFileSize(f, nullptr), got = 0;
    char* buf = (n != INVALID_FILE_SIZE) ? (char*)HeapAlloc(GetProcessHeap(), 0, n + 1) : nullptr;
    bool ok = buf && ReadFile(f, buf, n, &got, nullptr) && got == n;
    CloseHandle(f);
    if (!ok) { if (buf) HeapFree(GetProcessHeap(), 0, buf); return false; }
    buf[n] = 0;
    *data = buf;
    *size = n;
    return true;
}

// Loads jobs/scripts/<name>.lua as a chunk on the stack. Returns a Lua status, or -1 if the file isn't there.
static int load_script(lua_State* L, const char* name) {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s%S.lua", g_script_dir, name);
    char* data;
    DWORD size;
    if (!read_file(path, &data, &size)) return -1;
    char chunk[96];
    _snprintf_s(chunk, sizeof(chunk), _TRUNCATE, "@%s.lua", name);
    int st = luaL_loadbuffer(L, data, size, chunk);
    HeapFree(GetProcessHeap(), 0, data);
    return st;
}

// package.searchers entry: require "name" -> jobs/scripts/name.lua (names are plain file names: no dots or slashes).
static int searcher(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    for (const char* c = name; *c; c++)
        if (*c == '.' || *c == '/' || *c == '\\') { lua_pushfstring(L, "\n\tjobs: '%s' is not a plain script name", name); return 1; }
    int st = load_script(L, name);
    if (st == -1) { lua_pushfstring(L, "\n\tjobs: no jobs/scripts/%s.lua", name); return 1; }
    if (st != LUA_OK) return lua_error(L);
    lua_pushstring(L, name);
    return 2;
}

static int l_log(lua_State* L) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, ' ');
        luaL_tolstring(L, i, nullptr);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    log_line("lua: %s", lua_tostring(L, -1));
    return 0;
}

static void set_int(lua_State* L, const char* k, lua_Integer v) { lua_pushinteger(L, v); lua_setfield(L, -2, k); }

static const char* const STAT_NAMES[8] = { "str", "def", "mag", "mdef", "agi", "luck", "eva", "acc" };

static int l_ply(lua_State* L) {
    lua_Integer i = luaL_checkinteger(L, 1);
    if (i < 0 || i >= PLY_COUNT || !g_ply) { lua_pushnil(L); return 1; }
    uint8_t r[PLY_STRIDE];
    if (!safe_copy(r, g_ply + i * PLY_STRIDE, sizeof(r))) { lua_pushnil(L); return 1; }
    auto u32 = [&](int off) { uint32_t v; memcpy(&v, r + off, 4); return (lua_Integer)v; };
    lua_createtable(L, 0, 12);
    set_int(L, "index", i);
    set_int(L, "level", r[ply::LEVEL]);
    set_int(L, "cap_byte", r[ply::CAP]);
    set_int(L, "ap", u32(ply::AP));
    set_int(L, "ap_total", u32(ply::AP_TOTAL));
    set_int(L, "hp", u32(ply::HP));
    set_int(L, "mp", u32(ply::MP));
    set_int(L, "max_hp", u32(ply::MAX_HP));
    set_int(L, "max_mp", u32(ply::MAX_MP));
    set_int(L, "flags", r[ply::FLAGS]);
    lua_createtable(L, 0, 10);                       // base: what levels and stat spheres write
    set_int(L, "hp", u32(ply::BASE_HP));
    set_int(L, "mp", u32(ply::BASE_MP));
    for (int s = 0; s < 8; s++) set_int(L, STAT_NAMES[s], r[ply::BASE_STAT + s]);
    lua_setfield(L, -2, "base");
    lua_createtable(L, 0, 8);                        // final: base + gear, as the game shows it
    for (int s = 0; s < 8; s++) set_int(L, STAT_NAMES[s], r[ply::STAT + s]);
    lua_setfield(L, -2, "final");
    return 1;
}

static lua_State* new_state() {
    lua_State* L = luaL_newstate();
    if (!L) return nullptr;
    luaL_openlibs(L);
    lua_getglobal(L, "package");                     // put our searcher first, drop the C-library searchers
    lua_getfield(L, -1, "searchers");
    lua_pushcfunction(L, searcher);
    lua_rawseti(L, -2, 2);
    lua_pushnil(L); lua_rawseti(L, -2, 4);
    lua_pushnil(L); lua_rawseti(L, -2, 3);
    lua_pop(L, 2);
    lua_createtable(L, 0, 4);
    lua_pushcfunction(L, l_log); lua_setfield(L, -2, "log");
    lua_pushcfunction(L, l_ply); lua_setfield(L, -2, "ply");
    set_int(L, "PLY_COUNT", PLY_COUNT);
    lua_setglobal(L, "jobs");
    return L;
}

// Fresh state, run main.lua. On any error the new state is dropped and the previous one (if any) stays.
static void reload(const char* why) {
    lua_State* L = new_state();
    if (!L) { log_line("lua: out of memory creating a state"); return; }
    int st = load_script(L, "main");
    if (st == -1) { log_line("lua: no jobs/scripts/main.lua (%s)", why); lua_close(L); return; }
    if (st == LUA_OK) st = lua_pcall(L, 0, 0, 0);
    if (st != LUA_OK) {
        log_line("lua: main.lua failed (%s): %s - keeping the previous scripts", why, lua_tostring(L, -1));
        lua_close(L);
        return;
    }
    EnterCriticalSection(&g_lua_lock);
    lua_State* old = g_L;
    g_L = L;
    LeaveCriticalSection(&g_lua_lock);
    if (old) lua_close(old);
    log_line("lua: main.lua loaded (%s)", why);
}

// Newest last-write time over jobs/scripts/*.lua (0 if none).
static ULONGLONG scripts_stamp() {
    wchar_t pat[MAX_PATH];
    _snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s*.lua", g_script_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ULONGLONG newest = 0;
    do {
        ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (t > newest) newest = t;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return newest;
}

static DWORD WINAPI watch_thread(LPVOID) {
    ULONGLONG seen = scripts_stamp();
    for (;;) {
        Sleep(g_cfg.script_poll_ms);
        ULONGLONG now = scripts_stamp();
        if (now == seen) continue;
        seen = now;
        Sleep(100);                                  // let the editor finish writing
        reload("file changed");
    }
}

void lua_start() {
    InitializeCriticalSection(&g_lua_lock);
    wchar_t dir[MAX_PATH];
    game_dir(dir, MAX_PATH);
    _snwprintf_s(g_script_dir, MAX_PATH, _TRUNCATE, L"%sjobs\\scripts\\", dir);
    reload("start");
    HANDLE t = CreateThread(nullptr, 0, watch_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else log_line("lua: reload watcher not started (error %lu)", GetLastError());
}

} // namespace jobs
