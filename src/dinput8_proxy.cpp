#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dinput.h>
#include <string>
#include <cstdio>

namespace {
HMODULE g_systemDinput8 = nullptr;
INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;

void WriteLog(const wchar_t* message) noexcept {
    wchar_t exePath[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    std::wstring path(exePath, n);
    const auto slash = path.find_last_of(L"\\/");
    path.resize(slash == std::wstring::npos ? 0 : slash + 1);
    path += L"TC2_Walkmod.log";

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t line[768]{};
    const int chars = swprintf_s(line, L"[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, message);
    if (chars > 0) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(chars * sizeof(wchar_t)), &written, nullptr);
    }
    CloseHandle(file);
}

void LogGameImageInfo() noexcept {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) {
        WriteLog(L"ERROR: GetModuleHandleW(NULL) failed.");
        return;
    }

    wchar_t path[MAX_PATH]{};
    const DWORD pathLen = GetModuleFileNameW(exe, path, MAX_PATH);
    if (pathLen && pathLen < MAX_PATH) {
        wchar_t line[MAX_PATH + 64]{};
        swprintf_s(line, L"Game executable: %s", path);
        WriteLog(line);
    }

    const auto base = reinterpret_cast<const BYTE*>(exe);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        WriteLog(L"ERROR: Invalid in-memory DOS header.");
        return;
    }

    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        WriteLog(L"ERROR: Invalid in-memory PE32+ header.");
        return;
    }

    wchar_t line[256]{};
    swprintf_s(line,
        L"Game image: base=%p SizeOfImage=0x%08lX PE timestamp=0x%08lX machine=0x%04X",
        exe,
        static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage),
        static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
        static_cast<unsigned>(nt->FileHeader.Machine));
    WriteLog(line);
}

BOOL CALLBACK InitializeProxy(PINIT_ONCE, PVOID, PVOID*) noexcept {
    LogGameImageInfo();

    wchar_t systemDir[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        WriteLog(L"ERROR: GetSystemDirectoryW failed.");
        return TRUE;
    }

    std::wstring dllPath(systemDir, n);
    dllPath += L"\\dinput8.dll";
    g_systemDinput8 = LoadLibraryW(dllPath.c_str());
    if (g_systemDinput8) {
        WriteLog(L"Proxy loaded. Original System32 dinput8.dll loaded successfully.");
    } else {
        const DWORD error = GetLastError();
        wchar_t line[128]{};
        swprintf_s(line, L"ERROR: Could not load System32 dinput8.dll. Win32 error=%lu",
            static_cast<unsigned long>(error));
        WriteLog(line);
    }
    return TRUE;
}

FARPROC GetOriginal(const char* name) noexcept {
    InitOnceExecuteOnce(&g_initOnce, InitializeProxy, nullptr, nullptr);
    return g_systemDinput8 ? GetProcAddress(g_systemDinput8, name) : nullptr;
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version,
    REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    using Fn = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto fn = reinterpret_cast<Fn>(GetOriginal("DirectInput8Create"));
    if (!fn) {
        WriteLog(L"ERROR: DirectInput8Create export was not found in System32 dinput8.dll.");
        return E_FAIL;
    }

    const HRESULT hr = fn(instance, version, riid, out, outer);
    wchar_t line[128]{};
    swprintf_s(line, L"DirectInput8Create called; HRESULT=0x%08lX",
        static_cast<unsigned long>(hr));
    WriteLog(line);
    return hr;
}

extern "C" HRESULT WINAPI DllCanUnloadNow() {
    using Fn = HRESULT (WINAPI*)();
    auto fn = reinterpret_cast<Fn>(GetOriginal("DllCanUnloadNow"));
    return fn ? fn() : S_FALSE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* out) {
    using Fn = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto fn = reinterpret_cast<Fn>(GetOriginal("DllGetClassObject"));
    return fn ? fn(clsid, iid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" HRESULT WINAPI DllRegisterServer() {
    using Fn = HRESULT (WINAPI*)();
    auto fn = reinterpret_cast<Fn>(GetOriginal("DllRegisterServer"));
    return fn ? fn() : E_NOTIMPL;
}

extern "C" HRESULT WINAPI DllUnregisterServer() {
    using Fn = HRESULT (WINAPI*)();
    auto fn = reinterpret_cast<Fn>(GetOriginal("DllUnregisterServer"));
    return fn ? fn() : E_NOTIMPL;
}

extern "C" LPCDIDATAFORMAT WINAPI GetdfDIJoystick() {
    using Fn = LPCDIDATAFORMAT (WINAPI*)();
    auto fn = reinterpret_cast<Fn>(GetOriginal("GetdfDIJoystick"));
    return fn ? fn() : nullptr;
}

