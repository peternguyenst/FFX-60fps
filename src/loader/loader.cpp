// SPDX-License-Identifier: Apache-2.0
// Shared loader: ships as dbghelp.dll in the FFX / FFX-2 game folder (both exes import only MiniDumpWriteDump from
// dbghelp). It forwards dbghelp to System32 (forwards.cpp) and loads the game's own mod DLL from the game folder:
//   FFX.exe   -> fps60_ffx.dll, jobs_ffx.dll (each one only if present)
//   FFX-2.exe -> fps60_ffx2.dll
// Any other host, or no mod DLL for this game, or a load failure: pure forwarder, the game runs stock.
// A mod DLL may export `void __cdecl mod_minidump_pre(void* exception_param, int* dump_type)`: called (SEH-guarded)
// before the game's crash dump is written. Keep this file small and stable: both games' releases ship it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef BOOL(WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
typedef void(__cdecl* minidump_pre_t)(void*, int*);

static minidump_pre_t g_minidump_pre = nullptr;

static MiniDumpWriteDump_t resolve_real_minidump() {
    static MiniDumpWriteDump_t real = nullptr;
    if (real) return real;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return nullptr;
    lstrcatW(path, L"\\dbghelp.dll");
    HMODULE m = LoadLibraryW(path);
    if (!m) return nullptr;
    real = (MiniDumpWriteDump_t)GetProcAddress(m, "MiniDumpWriteDump");
    return real;
}

extern "C" BOOL WINAPI ld_MiniDumpWriteDump(
    HANDLE hProcess, DWORD ProcessId, HANDLE hFile, int DumpType,
    void* ExceptionParam, void* UserStreamParam, void* CallbackParam) {
    MiniDumpWriteDump_t real = resolve_real_minidump();
    if (!real) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }
    if (g_minidump_pre) {
        __try { g_minidump_pre(ExceptionParam, &DumpType); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return real(hProcess, ProcessId, hFile, DumpType, ExceptionParam, UserStreamParam, CallbackParam);
}

// One line in fps60_loader.log, only when a mod DLL is present but does not load (otherwise silent).
static void loader_note(const wchar_t* dir, const wchar_t* dll, DWORD err) {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%sfps60_loader.log", dir);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char line[512];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d %S failed to load (error %lu) - game runs stock\r\n",
                        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, dll, err);
    DWORD w;
    if (n > 0) WriteFile(f, line, (DWORD)n, &w, nullptr);
    CloseHandle(f);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(hinst);

    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return TRUE;
    wchar_t* name = exe;
    for (wchar_t* p = exe; *p; p++) if (*p == L'\\' || *p == L'/') name = p + 1;

    static const wchar_t* const FFX_DLLS[] = { L"fps60_ffx.dll", L"jobs_ffx.dll", nullptr };
    static const wchar_t* const FFX2_DLLS[] = { L"fps60_ffx2.dll", nullptr };
    const wchar_t* const* dlls = nullptr;
    if (lstrcmpiW(name, L"FFX.exe") == 0) dlls = FFX_DLLS;
    else if (lstrcmpiW(name, L"FFX-2.exe") == 0) dlls = FFX2_DLLS;
    if (!dlls) return TRUE;

    *name = 0;                                   // exe now holds the game folder, with trailing '\'
    for (; *dlls; dlls++) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s%s", exe, *dlls);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;   // this mod not installed

        // A mod DLL's own DllMain only starts its init thread, so loading it here (under the loader lock) is safe.
        HMODULE m = LoadLibraryW(path);
        if (!m) { loader_note(exe, *dlls, GetLastError()); continue; }
        if (!g_minidump_pre) g_minidump_pre = (minidump_pre_t)GetProcAddress(m, "mod_minidump_pre");
    }
    return TRUE;
}
