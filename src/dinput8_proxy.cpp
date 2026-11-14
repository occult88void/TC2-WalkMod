#include <windows.h>
#include <winnt.h>
#include <tlhelp32.h>
#include <dinput.h>
#include <bcrypt.h>

#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Belt-and-suspenders: make MSVC link the Windows CNG library even if a
// project-level linker setting is accidentally omitted.
#pragma comment(lib, "bcrypt.lib")

namespace {

using DirectInput8CreateFn = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DllCanUnloadNowFn = HRESULT (WINAPI*)();
using DllGetClassObjectFn = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
using DllRegisterServerFn = HRESULT (WINAPI*)();
using DllUnregisterServerFn = HRESULT (WINAPI*)();
using GetdfDIJoystickFn = LPCDIDATAFORMAT (WINAPI*)();

HMODULE g_original = nullptr;
DirectInput8CreateFn g_DirectInput8Create = nullptr;
DllCanUnloadNowFn g_DllCanUnloadNow = nullptr;
DllGetClassObjectFn g_DllGetClassObject = nullptr;
DllRegisterServerFn g_DllRegisterServer = nullptr;
DllUnregisterServerFn g_DllUnregisterServer = nullptr;
GetdfDIJoystickFn g_GetdfDIJoystick = nullptr;

std::wstring GetLogPath() {
    wchar_t exePath[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"TC2_Walkmod.log";
    }
    std::wstring path(exePath, length);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.resize(slash + 1);
    } else {
        path.clear();
    }
    path += L"TC2_Walkmod.log";
    return path;
}

void Log(const wchar_t* format, ...) {
    wchar_t message[2048]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);

    const std::wstring path = GetLogPath();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a+, ccs=UTF-8") == 0 && file) {
        fwprintf(file, L"%s\n", message);
        fclose(file);
    }
}

bool IsReadableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) || (protection & PAGE_NOACCESS)) {
        return false;
    }
    const DWORD base = protection & 0xFF;
    return base == PAGE_READONLY || base == PAGE_READWRITE ||
           base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READ ||
           base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
}

void LogFingerprint(const char* sectionName, const BYTE* address, SIZE_T size) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD bytesCopied = 0;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        Log(L"FINGERPRINT: %S status=BCryptOpenAlgorithmProvider error=0x%08X", sectionName, static_cast<unsigned>(status));
        return;
    }

    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &bytesCopied, 0);
    if (status < 0 || objectLength == 0) {
        Log(L"FINGERPRINT: %S status=BCryptGetProperty error=0x%08X", sectionName, static_cast<unsigned>(status));
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return;
    }

    std::vector<BYTE> hashObject(objectLength);
    status = BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0);
    if (status < 0) {
        Log(L"FINGERPRINT: %S status=BCryptCreateHash error=0x%08X", sectionName, static_cast<unsigned>(status));
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return;
    }

    bool complete = true;
    SIZE_T offset = 0;
    constexpr SIZE_T chunkSize = 1024 * 1024;
    while (offset < size) {
        MEMORY_BASIC_INFORMATION mbi{};
        const BYTE* current = address + offset;
        if (VirtualQuery(current, &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect)) {
            complete = false;
            break;
        }

        const BYTE* regionEnd = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= current) {
            complete = false;
            break;
        }
        SIZE_T available = static_cast<SIZE_T>(regionEnd - current);
        SIZE_T regionBytes = (size - offset < available) ? (size - offset) : available;
        SIZE_T hashedInRegion = 0;
        while (hashedInRegion < regionBytes) {
            const SIZE_T remaining = regionBytes - hashedInRegion;
            const ULONG part = static_cast<ULONG>((remaining > chunkSize) ? chunkSize : remaining);
            status = BCryptHashData(hash, const_cast<PUCHAR>(current + hashedInRegion), part, 0);
            if (status < 0) {
                complete = false;
                break;
            }
            hashedInRegion += part;
        }
        if (!complete) break;
        offset += regionBytes;
    }

    BYTE digest[32]{};
    if (complete && offset == size) {
        status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
        if (status >= 0) {
            wchar_t hex[65]{};
            for (size_t i = 0; i < sizeof(digest); ++i) {
                swprintf_s(hex + i * 2, 3, L"%02X", digest[i]);
            }
            Log(L"FINGERPRINT: %S size=0x%llX sha256=%s", sectionName,
                static_cast<unsigned long long>(size), hex);
        } else {
            Log(L"FINGERPRINT: %S status=BCryptFinishHash error=0x%08X", sectionName, static_cast<unsigned>(status));
        }
    } else {
        Log(L"FINGERPRINT: %S incomplete offset=0x%llX size=0x%llX",
            sectionName, static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size));
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
}

void LogImageInfo() {
    HMODULE image = GetModuleHandleW(nullptr);
    if (!image) {
        Log(L"IMAGE: GetModuleHandleW(nullptr) failed error=%lu", GetLastError());
        return;
    }

    const BYTE* base = reinterpret_cast<const BYTE*>(image);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        Log(L"IMAGE: invalid DOS signature");
        return;
    }
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        Log(L"IMAGE: invalid PE64 header");
        return;
    }

    Log(L"IMAGE: base=%p image_size=0x%08X sections=%u timestamp=0x%08X machine=0x%04X",
        image, nt->OptionalHeader.SizeOfImage, nt->FileHeader.NumberOfSections,
        nt->FileHeader.TimeDateStamp, nt->FileHeader.Machine);

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        char name[9]{};
        memcpy(name, section->Name, 8);
        const BYTE* address = base + section->VirtualAddress;
        const SIZE_T virtualSize = section->Misc.VirtualSize;
        MEMORY_BASIC_INFORMATION mbi{};
        DWORD protection = 0;
        DWORD state = 0;
        if (VirtualQuery(address, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            protection = mbi.Protect;
            state = mbi.State;
        }
        Log(L"SECTION: %S address=%p RVA=0x%08X virtual=0x%08X raw=0x%08X characteristics=0x%08X protection=0x%08X state=0x%lX",
            name, address, section->VirtualAddress, static_cast<DWORD>(virtualSize),
            section->SizeOfRawData, section->Characteristics, protection, state);

        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 && virtualSize != 0) {
            LogFingerprint(name, address, virtualSize);
        }
    }
}

void LogModules() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        Log(L"MODULES: snapshot failed error=%lu", GetLastError());
        return;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            Log(L"MODULE: %s base=%p size=0x%08X path=%s",
                entry.szModule, entry.modBaseAddr, entry.modBaseSize, entry.szExePath);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

DWORD WINAPI DiagnosticWorker(LPVOID) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    Log(L"=== TC2 Walkmod diagnostic bootstrap 04 (bcrypt fix) ===");
    Log(L"Game executable: %s", exePath);
    LogImageInfo();
    LogModules();
    return 0;
}

bool LoadOriginal() {
    if (g_original) return true;
    wchar_t systemDirectory[MAX_PATH]{};
    UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        Log(L"PROXY: GetSystemDirectoryW failed error=%lu", GetLastError());
        return false;
    }
    std::wstring path(systemDirectory, length);
    path += L"\\dinput8.dll";
    g_original = LoadLibraryW(path.c_str());
    if (!g_original) {
        Log(L"PROXY: failed to load system dinput8.dll error=%lu", GetLastError());
        return false;
    }
    g_DirectInput8Create = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(g_original, "DirectInput8Create"));
    g_DllCanUnloadNow = reinterpret_cast<DllCanUnloadNowFn>(GetProcAddress(g_original, "DllCanUnloadNow"));
    g_DllGetClassObject = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(g_original, "DllGetClassObject"));
    g_DllRegisterServer = reinterpret_cast<DllRegisterServerFn>(GetProcAddress(g_original, "DllRegisterServer"));
    g_DllUnregisterServer = reinterpret_cast<DllUnregisterServerFn>(GetProcAddress(g_original, "DllUnregisterServer"));
    g_GetdfDIJoystick = reinterpret_cast<GetdfDIJoystickFn>(GetProcAddress(g_original, "GetdfDIJoystick"));
    Log(L"PROXY: original System32 dinput8.dll loaded %s", g_DirectInput8Create ? L"successfully" : L"but DirectInput8Create export is missing");
    return g_DirectInput8Create != nullptr;
}

} // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* output, LPUNKNOWN outer) {
    if (!LoadOriginal() || !g_DirectInput8Create) return E_FAIL;
    Log(L"DirectInput8Create called");
    const HRESULT result = g_DirectInput8Create(instance, version, riid, output, outer);
    Log(L"DirectInput8Create HRESULT=0x%08X", static_cast<unsigned>(result));
    return result;
}

extern "C" HRESULT WINAPI DllCanUnloadNow() {
    if (!LoadOriginal() || !g_DllCanUnloadNow) return S_FALSE;
    return g_DllCanUnloadNow();
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* output) {
    if (!LoadOriginal() || !g_DllGetClassObject) return CLASS_E_CLASSNOTAVAILABLE;
    return g_DllGetClassObject(clsid, iid, output);
}

extern "C" HRESULT WINAPI DllRegisterServer() {
    if (!LoadOriginal() || !g_DllRegisterServer) return E_FAIL;
    return g_DllRegisterServer();
}

extern "C" HRESULT WINAPI DllUnregisterServer() {
    if (!LoadOriginal() || !g_DllUnregisterServer) return E_FAIL;
    return g_DllUnregisterServer();
}

extern "C" LPCDIDATAFORMAT WINAPI GetdfDIJoystick() {
    if (!LoadOriginal() || !g_GetdfDIJoystick) return nullptr;
    return g_GetdfDIJoystick();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE worker = CreateThread(nullptr, 0, DiagnosticWorker, nullptr, 0, nullptr);
        if (worker) CloseHandle(worker);
    }
    return TRUE;
}

