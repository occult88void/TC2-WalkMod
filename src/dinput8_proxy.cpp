#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dinput.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <vector>
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
    SYSTEMTIME st{}; GetLocalTime(&st);
    wchar_t line[1024]{};
    const int chars = swprintf_s(line, L"[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, message);
    if (chars > 0) { DWORD written = 0; WriteFile(file, line, static_cast<DWORD>(chars * sizeof(wchar_t)), &written, nullptr); }
    CloseHandle(file);
}

const wchar_t* ProtectionName(DWORD p) noexcept {
    switch (p & 0xff) {
    case PAGE_NOACCESS: return L"NOACCESS";
    case PAGE_READONLY: return L"READONLY";
    case PAGE_READWRITE: return L"READWRITE";
    case PAGE_WRITECOPY: return L"WRITECOPY";
    case PAGE_EXECUTE: return L"EXECUTE";
    case PAGE_EXECUTE_READ: return L"EXECUTE_READ";
    case PAGE_EXECUTE_READWRITE: return L"EXECUTE_READWRITE";
    case PAGE_EXECUTE_WRITECOPY: return L"EXECUTE_WRITECOPY";
    default: return L"OTHER";
    }
}


std::wstring HexBytes(const BYTE* data, SIZE_T size) {
    static const wchar_t* digits = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(size * 2);
    for (SIZE_T i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0F]);
    }
    return out;
}

bool IsReadableProtection(DWORD protect) noexcept {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protect & 0xff) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

void LogExecutableSectionFingerprint(const wchar_t* name, const BYTE* address, SIZE_T size) noexcept {
    wchar_t line[512]{};
    if (!address || size == 0) return;

    MEMORY_BASIC_INFORMATION firstMbi{};
    if (!VirtualQuery(address, &firstMbi, sizeof(firstMbi)) || firstMbi.State != MEM_COMMIT ||
        !IsReadableProtection(firstMbi.Protect)) {
        swprintf_s(line, L"FINGERPRINT: %s skipped; first memory page is not readable/committed.", name);
        WriteLog(line);
        return;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0, resultLength = 0;
    std::vector<BYTE> hashObject;
    BYTE digest[32]{};
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0);
    if (status >= 0) {
        hashObject.resize(objectLength);
        status = BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0);
    }

    bool complete = status >= 0;
    SIZE_T offset = 0;
    while (complete && offset < size) {
        MEMORY_BASIC_INFORMATION mbi{};
        const BYTE* current = address + offset;
        if (!VirtualQuery(current, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect)) {
            complete = false;
            break;
        }
        const BYTE* regionEnd = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= current) { complete = false; break; }
        SIZE_T available = static_cast<SIZE_T>(regionEnd - current);
        SIZE_T remaining = size - offset;
        SIZE_T regionBytes = (available < remaining) ? available : remaining;
        SIZE_T consumed = 0;
        while (consumed < regionBytes) {
            const ULONG chunk = static_cast<ULONG>((regionBytes - consumed) > (1024 * 1024) ? (1024 * 1024) : (regionBytes - consumed));
            status = BCryptHashData(hash, const_cast<PUCHAR>(current + consumed), chunk, 0);
            if (status < 0) { complete = false; break; }
            consumed += chunk;
        }
        offset += regionBytes;
    }

    if (complete) status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);

    if (complete && status >= 0) {
        const SIZE_T sampleSize = size < 32 ? size : 32;
        const std::wstring digestHex = HexBytes(digest, sizeof(digest));
        const std::wstring sampleHex = HexBytes(address, sampleSize);
        swprintf_s(line, L"FINGERPRINT: %s size=0x%llX sha256=%s first_bytes=%s",
            name, static_cast<unsigned long long>(size), digestHex.c_str(), sampleHex.c_str());
    } else {
        swprintf_s(line, L"FINGERPRINT: %s incomplete; readable committed bytes=%llu of %llu; NTSTATUS=0x%08lX",
            name, static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size), static_cast<unsigned long>(status));
    }
    WriteLog(line);
}

void LogModuleList() noexcept {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) { WriteLog(L"MODULES: CreateToolhelp32Snapshot failed."); return; }
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            wchar_t line[1200]{};
            swprintf_s(line, L"MODULE: base=%p size=0x%08lX name=%s path=%s",
                me.modBaseAddr, static_cast<unsigned long>(me.modBaseSize), me.szModule, me.szExePath);
            WriteLog(line);
        } while (Module32NextW(snap, &me));
    } else WriteLog(L"MODULES: Module32FirstW failed.");
    CloseHandle(snap);
}

void LogGameSections() noexcept {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) { WriteLog(L"SECTIONS: GetModuleHandleW failed."); return; }
    const BYTE* base = reinterpret_cast<const BYTE*>(exe);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) {
        WriteLog(L"SECTIONS: invalid DOS header."); return;
    }
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        WriteLog(L"SECTIONS: invalid PE32+ header."); return;
    }
    wchar_t line[512]{};
    swprintf_s(line, L"IMAGE: base=%p image_size=0x%08lX sections=%u timestamp=0x%08lX",
        exe, static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage),
        static_cast<unsigned>(nt->FileHeader.NumberOfSections), static_cast<unsigned long>(nt->FileHeader.TimeDateStamp));
    WriteLog(line);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char nameA[9]{}; memcpy(nameA, sec[i].Name, 8);
        wchar_t nameW[16]{}; MultiByteToWideChar(CP_ACP, 0, nameA, -1, nameW, 16);
        const BYTE* address = base + sec[i].VirtualAddress;
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T queried = VirtualQuery(address, &mbi, sizeof(mbi));
        const wchar_t* protection = queried ? ProtectionName(mbi.Protect) : L"QUERY_FAILED";
        swprintf_s(line, L"SECTION: %s address=%p RVA=0x%08lX virtual=0x%08lX raw=0x%08lX characteristics=0x%08lX protection=%s state=0x%lX",
            nameW, address, static_cast<unsigned long>(sec[i].VirtualAddress),
            static_cast<unsigned long>(sec[i].Misc.VirtualSize), static_cast<unsigned long>(sec[i].SizeOfRawData),
            static_cast<unsigned long>(sec[i].Characteristics), protection,
            queried ? static_cast<unsigned long>(mbi.State) : 0ul);
        WriteLog(line);
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            LogExecutableSectionFingerprint(nameW, address, static_cast<SIZE_T>(sec[i].Misc.VirtualSize));
        }
    }
}

BOOL CALLBACK InitializeProxy(PINIT_ONCE, PVOID, PVOID*) noexcept {
    WriteLog(L"=== TC2 Walkmod diagnostic bootstrap 04 ===");
    wchar_t path[MAX_PATH]{};
    const DWORD pathLen = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (pathLen && pathLen < MAX_PATH) { wchar_t line[MAX_PATH + 64]{}; swprintf_s(line, L"Game executable: %s", path); WriteLog(line); }
    LogGameSections();
    LogModuleList();

    wchar_t systemDir[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { WriteLog(L"ERROR: GetSystemDirectoryW failed."); return TRUE; }
    std::wstring dllPath(systemDir, n); dllPath += L"\\dinput8.dll";
    g_systemDinput8 = LoadLibraryW(dllPath.c_str());
    if (g_systemDinput8) WriteLog(L"Proxy loaded. Original System32 dinput8.dll loaded successfully.");
    else { wchar_t line[128]{}; swprintf_s(line, L"ERROR: Could not load System32 dinput8.dll. Win32 error=%lu", static_cast<unsigned long>(GetLastError())); WriteLog(line); }
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

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    using Fn = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto fn = reinterpret_cast<Fn>(GetOriginal("DirectInput8Create"));
    if (!fn) { WriteLog(L"ERROR: DirectInput8Create export missing in System32 dinput8.dll."); return E_FAIL; }
    const HRESULT hr = fn(instance, version, riid, out, outer);
    wchar_t line[128]{}; swprintf_s(line, L"DirectInput8Create called; HRESULT=0x%08lX", static_cast<unsigned long>(hr)); WriteLog(line);
    return hr;
}
extern "C" HRESULT WINAPI DllCanUnloadNow() { using Fn = HRESULT (WINAPI*)(); auto fn = reinterpret_cast<Fn>(GetOriginal("DllCanUnloadNow")); return fn ? fn() : S_FALSE; }
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID i, LPVOID* o) { using Fn = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*); auto fn = reinterpret_cast<Fn>(GetOriginal("DllGetClassObject")); return fn ? fn(c, i, o) : CLASS_E_CLASSNOTAVAILABLE; }
extern "C" HRESULT WINAPI DllRegisterServer() { using Fn = HRESULT (WINAPI*)(); auto fn = reinterpret_cast<Fn>(GetOriginal("DllRegisterServer")); return fn ? fn() : E_NOTIMPL; }
extern "C" HRESULT WINAPI DllUnregisterServer() { using Fn = HRESULT (WINAPI*)(); auto fn = reinterpret_cast<Fn>(GetOriginal("DllUnregisterServer")); return fn ? fn() : E_NOTIMPL; }
extern "C" LPCDIDATAFORMAT WINAPI GetdfDIJoystick() { using Fn = LPCDIDATAFORMAT (WINAPI*)(); auto fn = reinterpret_cast<Fn>(GetOriginal("GetdfDIJoystick")); return fn ? fn() : nullptr; }

