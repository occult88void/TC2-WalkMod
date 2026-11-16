#include <windows.h>
#include <winnt.h>
#include <tlhelp32.h>
#include <dinput.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Belt-and-suspenders: make MSVC link the Windows CNG library even if a
// project-level linker setting is accidentally omitted.
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "user32.lib")

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

    // Write explicit UTF-8 bytes instead of relying on the CRT's wide-file
    // encoding mode. This prevents the log from appearing as CJK/mojibake
    // when copied between Windows tools.
    const int required = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return;
    std::vector<char> utf8(static_cast<size_t>(required));
    if (WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8.data(), required, nullptr, nullptr) <= 0) return;

    const std::wstring path = GetLogPath();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") == 0 && file) {
        if (_fseeki64(file, 0, SEEK_END) == 0 && _ftelli64(file) == 0) {
            static const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
            fwrite(bom, 1, sizeof(bom), file);
        }
        fwrite(utf8.data(), 1, static_cast<size_t>(required - 1), file);
        fwrite("\r\n", 1, 2, file);
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
    constexpr SIZE_T chunkSize = 64 * 1024;
    std::vector<BYTE> buffer(chunkSize);

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
        const SIZE_T available = static_cast<SIZE_T>(regionEnd - current);
        const SIZE_T regionBytes = (size - offset < available) ? (size - offset) : available;
        const SIZE_T part = (regionBytes > chunkSize) ? chunkSize : regionBytes;

        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), current, buffer.data(), part, &bytesRead) ||
            bytesRead != part) {
            Log(L"FINGERPRINT: %S read failed offset=0x%llX requested=0x%llX received=0x%llX error=%lu",
                sectionName, static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(part),
                static_cast<unsigned long long>(bytesRead), GetLastError());
            complete = false;
            break;
        }

        status = BCryptHashData(hash, buffer.data(), static_cast<ULONG>(bytesRead), 0);
        if (status < 0) {
            complete = false;
            break;
        }
        offset += bytesRead;
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


struct HashProbeTarget {
    const char* name;
    std::uint32_t value;
};

constexpr HashProbeTarget kHashProbeTargets[] = {
    { "CITEntityAvatarTC2", 0x2155AD67u },
    { "CITEntityAvatarSettings", 0x20F54DA4u },
    { "PlayerCharacterSetting", 0x31D9280Du },
    { "CITEntityHeadQuarterCharacterSpawnPoint", 0xAE884E8Fu },
    { "CITEntityHeadQuarterExitPoint", 0x5AA79419u },
    { "CMissionAtomSpawn", 0x430810CAu },
    { "CMissionAtomGetLocalPlayer", 0xB0B2E4E7u },
    { "CMissionAtomTeleportEntity", 0x5D01241Du },
    { "CMissionAtomDisablePedestrian", 0x80E55B58u },
};

void ScanExecutableSectionForHashes(const char* sectionName, const BYTE* address,
                                    SIZE_T size, const BYTE* imageBase,
                                    ULONGLONG scanDeadline) {
    std::uint64_t counts[_countof(kHashProbeTargets)]{};
    unsigned printed[_countof(kHashProbeTargets)]{};
    SIZE_T offset = 0;
    SIZE_T lastProgress = 0;
    std::uint64_t totalRead = 0;
    std::uint64_t totalSkipped = 0;
    constexpr SIZE_T chunkSize = 64 * 1024;
    std::vector<BYTE> carry;
    carry.reserve(sizeof(std::uint32_t) - 1);

    Log(L"HASHSCAN_SECTION_BEGIN: %S size=0x%llX chunk=0x%llX",
        sectionName, static_cast<unsigned long long>(size),
        static_cast<unsigned long long>(chunkSize));

    while (offset < size) {
        if (GetTickCount64() >= scanDeadline) {
            Log(L"HASHSCAN_TIMEOUT: section=%S offset=0x%llX/%0llX read=0x%llX skipped=0x%llX",
                sectionName, static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(totalRead),
                static_cast<unsigned long long>(totalSkipped));
            break;
        }

        MEMORY_BASIC_INFORMATION mbi{};
        const BYTE* current = address + offset;
        if (VirtualQuery(current, &mbi, sizeof(mbi)) != sizeof(mbi)) {
            const SIZE_T step = (size - offset > 0x1000) ? 0x1000 : (size - offset);
            Log(L"HASHSCAN_SKIP: section=%S offset=0x%llX reason=VirtualQuery_failed error=%lu",
                sectionName, static_cast<unsigned long long>(offset), GetLastError());
            offset += step;
            totalSkipped += step;
            carry.clear();
            continue;
        }

        const BYTE* regionEnd = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= current) {
            const SIZE_T step = (size - offset > 0x1000) ? 0x1000 : (size - offset);
            Log(L"HASHSCAN_SKIP: section=%S offset=0x%llX reason=invalid_region",
                sectionName, static_cast<unsigned long long>(offset));
            offset += step;
            totalSkipped += step;
            carry.clear();
            continue;
        }

        const SIZE_T available = static_cast<SIZE_T>(regionEnd - current);
        const SIZE_T regionBytes = (size - offset < available) ? (size - offset) : available;
        if (mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect)) {
            Log(L"HASHSCAN_SKIP: section=%S offset=0x%llX bytes=0x%llX state=0x%lX protect=0x%08X",
                sectionName, static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(regionBytes), mbi.State, mbi.Protect);
            offset += regionBytes;
            totalSkipped += regionBytes;
            carry.clear();
            continue;
        }

        const SIZE_T requested = (regionBytes > chunkSize) ? chunkSize : regionBytes;
        std::vector<BYTE> block(requested);
        SIZE_T bytesRead = 0;
        const BOOL readOk = ReadProcessMemory(GetCurrentProcess(), current,
            block.data(), requested, &bytesRead);
        if (bytesRead == 0) {
            Log(L"HASHSCAN_READ_ERROR: section=%S offset=0x%llX requested=0x%llX error=%lu",
                sectionName, static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(requested), GetLastError());
            offset += requested;
            totalSkipped += requested;
            carry.clear();
            continue;
        }

        std::vector<BYTE> scanData;
        scanData.reserve(carry.size() + bytesRead);
        scanData.insert(scanData.end(), carry.begin(), carry.end());
        scanData.insert(scanData.end(), block.begin(), block.begin() + bytesRead);

        for (SIZE_T i = 0; i + sizeof(std::uint32_t) <= scanData.size(); ++i) {
            std::uint32_t value = 0;
            std::memcpy(&value, scanData.data() + i, sizeof(value));
            size_t t = _countof(kHashProbeTargets);
            switch (value) {
                case 0x2155AD67u: t = 0; break;
                case 0x20F54DA4u: t = 1; break;
                case 0x31D9280Du: t = 2; break;
                case 0xAE884E8Fu: t = 3; break;
                case 0x5AA79419u: t = 4; break;
                case 0x430810CAu: t = 5; break;
                case 0xB0B2E4E7u: t = 6; break;
                case 0x5D01241Du: t = 7; break;
                case 0x80E55B58u: t = 8; break;
                default: break;
            }
            if (t == _countof(kHashProbeTargets)) continue;

            ++counts[t];
            if (printed[t] < 8) {
                char context[24 * 3 + 1]{};
                const SIZE_T contextStart = (i > 8) ? (i - 8) : 0;
                const SIZE_T contextEnd = ((i + 8 + 4) < scanData.size()) ? (i + 8 + 4) : scanData.size();
                SIZE_T cursor = 0;
                for (SIZE_T j = contextStart; j < contextEnd; ++j) {
                    const int written = sprintf_s(context + cursor, sizeof(context) - cursor,
                        "%02X%s", static_cast<unsigned>(scanData[j]),
                        (j + 1 < contextEnd) ? " " : "");
                    if (written <= 0) break;
                    cursor += static_cast<SIZE_T>(written);
                }

                const std::int64_t relative = static_cast<std::int64_t>(offset) -
                    static_cast<std::int64_t>(carry.size()) + static_cast<std::int64_t>(i);
                if (relative >= 0 && static_cast<SIZE_T>(relative) < size) {
                    const DWORD rva = static_cast<DWORD>((address + relative) - imageBase);
                    Log(L"HASHREF: section=%S name=%S hash=0x%08X rva=0x%08X address=%p bytes=%S",
                        sectionName, kHashProbeTargets[t].name, value, rva,
                        address + relative, context);
                }
                ++printed[t];
            }
        }

        if (scanData.size() >= sizeof(std::uint32_t) - 1) {
            carry.assign(scanData.end() - (sizeof(std::uint32_t) - 1), scanData.end());
        } else {
            carry = scanData;
        }

        totalRead += bytesRead;
        if (!readOk || bytesRead != requested) {
            Log(L"HASHSCAN_PARTIAL_READ: section=%S offset=0x%llX requested=0x%llX received=0x%llX error=%lu",
                sectionName, static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(requested),
                static_cast<unsigned long long>(bytesRead), GetLastError());
            totalSkipped += requested - bytesRead;
            carry.clear();
        }
        offset += requested;

        if (offset - lastProgress >= 16 * 1024 * 1024 || offset == size) {
            Log(L"HASHSCAN_PROGRESS: section=%S offset=0x%llX/%0llX read=0x%llX skipped=0x%llX",
                sectionName, static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(totalRead),
                static_cast<unsigned long long>(totalSkipped));
            lastProgress = offset;
        }
    }

    for (size_t t = 0; t < _countof(kHashProbeTargets); ++t) {
        if (counts[t] != 0) {
            Log(L"HASHSCAN: section=%S name=%S total=%llu logged=%u",
                sectionName, kHashProbeTargets[t].name,
                static_cast<unsigned long long>(counts[t]), printed[t]);
        }
    }

    Log(L"HASHSCAN_SECTION_END: %S read=0x%llX skipped=0x%llX",
        sectionName, static_cast<unsigned long long>(totalRead),
        static_cast<unsigned long long>(totalSkipped));
}

void ScanExecutableSectionsForHashes() {
    HMODULE image = GetModuleHandleW(nullptr);
    if (!image) return;
    const BYTE* base = reinterpret_cast<const BYTE*>(image);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;

    Log(L"=== HASH SCAN BEGIN (read-only executable memory scan, 30s limit) ===");
    Log(L"HASHSCAN_POLICY: executable sections only; writable .data and non-executable data sections are intentionally excluded");
    const ULONGLONG scanDeadline = GetTickCount64() + 30000;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 || section->Misc.VirtualSize == 0) continue;
        char name[9]{};
        memcpy(name, section->Name, 8);
        const BYTE* address = base + section->VirtualAddress;
        if (GetTickCount64() >= scanDeadline) {
            Log(L"HASHSCAN_SECTION_NOT_STARTED: %S reason=global_time_budget_exhausted", name);
            break;
        }
        Log(L"HASHSCAN_SECTION: %S rva=0x%08X size=0x%08X", name,
            section->VirtualAddress, static_cast<DWORD>(section->Misc.VirtualSize));
        try {
            ScanExecutableSectionForHashes(name, address, section->Misc.VirtualSize, base, scanDeadline);
        } catch (...) {
            Log(L"HASHSCAN_SECTION_EXCEPTION: %S", name);
        }
    }
    if (GetTickCount64() >= scanDeadline) {
        Log(L"HASHSCAN: global time budget exhausted; remaining executable sections were not scanned");
    }
    Log(L"=== HASH SCAN END ===");
}


void DumpSpawnHashCodeWindow() {
    constexpr DWORD targetRva = 0x044E8A08u;
    constexpr DWORD windowBefore = 0x40u;
    constexpr SIZE_T windowSize = 0x100u;

    HMODULE image = GetModuleHandleW(nullptr);
    if (!image || targetRva < windowBefore) {
        Log(L"CODE_WINDOW_ERROR: invalid image or target RVA");
        return;
    }

    const BYTE* base = reinterpret_cast<const BYTE*>(image);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        Log(L"CODE_WINDOW_ERROR: invalid DOS signature");
        return;
    }
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        targetRva >= nt->OptionalHeader.SizeOfImage) {
        Log(L"CODE_WINDOW_ERROR: invalid PE header or target outside image");
        return;
    }

    const DWORD startRva = targetRva - windowBefore;
    const DWORD endRva = startRva + static_cast<DWORD>(windowSize);
    bool insideExecutableSection = false;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const DWORD sectionStart = section->VirtualAddress;
        const DWORD sectionSize = section->Misc.VirtualSize;
        const DWORD sectionEnd = sectionStart + sectionSize;
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 &&
            startRva >= sectionStart && endRva <= sectionEnd) {
            insideExecutableSection = true;
            break;
        }
    }
    if (!insideExecutableSection) {
        Log(L"CODE_WINDOW_ERROR: requested range is not fully inside an executable PE section");
        return;
    }

    const BYTE* source = base + startRva;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(source, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect)) {
        Log(L"CODE_WINDOW_ERROR: memory range is not committed/readable");
        return;
    }

    std::vector<BYTE> bytes(windowSize);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), source, bytes.data(), windowSize, &bytesRead) ||
        bytesRead != windowSize) {
        Log(L"CODE_WINDOW_ERROR: ReadProcessMemory requested=0x%llX received=0x%llX error=%lu",
            static_cast<unsigned long long>(windowSize),
            static_cast<unsigned long long>(bytesRead), GetLastError());
        return;
    }

    Log(L"=== CODE WINDOW BEGIN (read-only) ===");
    Log(L"CODE_WINDOW_TARGET: hash=CMissionAtomSpawn hash_rva=0x%08X start_rva=0x%08X size=0x%llX",
        targetRva, startRva, static_cast<unsigned long long>(windowSize));

    for (SIZE_T offset = 0; offset < bytes.size(); offset += 16) {
        char hex[16 * 3 + 1]{};
        SIZE_T cursor = 0;
        const SIZE_T count = (bytes.size() - offset < 16) ? bytes.size() - offset : 16;
        for (SIZE_T i = 0; i < count; ++i) {
            const int written = sprintf_s(hex + cursor, sizeof(hex) - cursor,
                "%02X%s", static_cast<unsigned>(bytes[offset + i]),
                (i + 1 < count) ? " " : "");
            if (written <= 0) break;
            cursor += static_cast<SIZE_T>(written);
        }
        Log(L"CODE_BYTES: rva=0x%08X bytes=%S",
            startRva + static_cast<DWORD>(offset), hex);
    }
    Log(L"=== CODE WINDOW END ===");
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


std::wstring GetCaptureRoot() {
    std::wstring path = GetLogPath();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path.resize(slash + 1);
    else path.clear();
    path += L"TC2_runtime_capture";
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Log(L"CAPTURE_ERROR: cannot create capture root error=%lu", GetLastError());
        return L"";
    }
    path += L"\\capture_";
    path += std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(path.c_str(), nullptr)) {
        Log(L"CAPTURE_ERROR: cannot create unique capture directory error=%lu", GetLastError());
        return L"";
    }
    return path;
}

bool CaptureOneSection(const BYTE* imageBase, const IMAGE_SECTION_HEADER& section,
                       const std::wstring& directory) {
    char narrowName[9]{};
    memcpy(narrowName, section.Name, 8);
    wchar_t wideName[16]{};
    if (MultiByteToWideChar(CP_ACP, 0, narrowName, -1, wideName, _countof(wideName)) <= 0) {
        Log(L"CAPTURE_SECTION_ERROR: invalid section name");
        return false;
    }

    const DWORD rva = section.VirtualAddress;
    const SIZE_T size = section.Misc.VirtualSize;
    if (size == 0) return true;

    std::wstring filePath = directory + L"\\" + wideName + L".bin";
    HANDLE output = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        Log(L"CAPTURE_SECTION_ERROR: section=%s CreateFile error=%lu", wideName, GetLastError());
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD copied = 0;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &copied, 0);
    }
    std::vector<BYTE> hashObject;
    if (status >= 0 && objectLength != 0) {
        hashObject.resize(objectLength);
        status = BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0);
    }

    bool complete = status >= 0 && hash != nullptr;
    SIZE_T offset = 0;
    constexpr SIZE_T chunkSize = 1024 * 1024;
    std::vector<BYTE> buffer(chunkSize);

    while (complete && offset < size) {
        const BYTE* current = imageBase + rva + offset;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(current, &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || !IsReadableProtection(mbi.Protect)) {
            Log(L"CAPTURE_READ_ERROR: section=%s offset=0x%llX reason=uncommitted_or_unreadable",
                wideName, static_cast<unsigned long long>(offset));
            complete = false;
            break;
        }

        const BYTE* regionEnd = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= current) {
            Log(L"CAPTURE_READ_ERROR: section=%s offset=0x%llX reason=invalid_memory_region",
                wideName, static_cast<unsigned long long>(offset));
            complete = false;
            break;
        }

        const SIZE_T available = static_cast<SIZE_T>(regionEnd - current);
        const SIZE_T remaining = size - offset;
        const SIZE_T requested = (remaining < available)
            ? ((remaining < chunkSize) ? remaining : chunkSize)
            : ((available < chunkSize) ? available : chunkSize);

        SIZE_T bytesRead = 0;
        if (requested == 0 ||
            !ReadProcessMemory(GetCurrentProcess(), current, buffer.data(), requested, &bytesRead) ||
            bytesRead != requested) {
            Log(L"CAPTURE_READ_ERROR: section=%s offset=0x%llX requested=0x%llX received=0x%llX error=%lu",
                wideName, static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(requested),
                static_cast<unsigned long long>(bytesRead), GetLastError());
            complete = false;
            break;
        }

        status = BCryptHashData(hash, buffer.data(), static_cast<ULONG>(bytesRead), 0);
        if (status < 0) {
            Log(L"CAPTURE_HASH_ERROR: section=%s offset=0x%llX status=0x%08X",
                wideName, static_cast<unsigned long long>(offset), static_cast<unsigned>(status));
            complete = false;
            break;
        }

        DWORD bytesWritten = 0;
        if (!WriteFile(output, buffer.data(), static_cast<DWORD>(bytesRead), &bytesWritten, nullptr) ||
            bytesWritten != bytesRead) {
            Log(L"CAPTURE_WRITE_ERROR: section=%s offset=0x%llX requested=0x%llX written=0x%lX error=%lu",
                wideName, static_cast<unsigned long long>(offset),
                static_cast<unsigned long long>(bytesRead), bytesWritten, GetLastError());
            complete = false;
            break;
        }
        offset += bytesRead;
    }

    BYTE digest[32]{};
    if (complete && offset == size) {
        status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
        if (status < 0 || !FlushFileBuffers(output)) {
            Log(L"CAPTURE_FINALIZE_ERROR: section=%s hash_status=0x%08X error=%lu",
                wideName, static_cast<unsigned>(status), GetLastError());
            complete = false;
        }
    } else {
        complete = false;
    }

    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(output);

    if (!complete) {
        DeleteFileW(filePath.c_str());
        Log(L"CAPTURE_SECTION_INCOMPLETE: section=%s captured=0x%llX expected=0x%llX partial_file_deleted",
            wideName, static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size));
        return false;
    }

    wchar_t hex[65]{};
    for (size_t i = 0; i < sizeof(digest); ++i) {
        swprintf_s(hex + i * 2, 3, L"%02X", digest[i]);
    }
    Log(L"CAPTURE_SECTION_OK: section=%s RVA=0x%08X size=0x%llX sha256=%s file=%s",
        wideName, rva, static_cast<unsigned long long>(size), hex, filePath.c_str());
    return true;
}

void CaptureRuntimeSections() {
    HMODULE image = GetModuleHandleW(nullptr);
    if (!image) {
        Log(L"CAPTURE_ERROR: main image handle unavailable");
        return;
    }
    const BYTE* base = reinterpret_cast<const BYTE*>(image);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        Log(L"CAPTURE_ERROR: invalid DOS signature");
        return;
    }
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        Log(L"CAPTURE_ERROR: invalid PE64 header");
        return;
    }

    const std::wstring directory = GetCaptureRoot();
    if (directory.empty()) return;

    Log(L"=== RUNTIME SECTION CAPTURE BEGIN (read-only) ===");
    Log(L"CAPTURE_IMAGE: base=%p size=0x%08X section_count=%u directory=%s",
        image, nt->OptionalHeader.SizeOfImage, nt->FileHeader.NumberOfSections, directory.c_str());

    unsigned captured = 0;
    unsigned failed = 0;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->Misc.VirtualSize == 0) continue;
        if (CaptureOneSection(base, *section, directory)) ++captured;
        else ++failed;
    }

    Log(L"CAPTURE_SUMMARY: captured_sections=%u failed_sections=%u total_sections=%u",
        captured, failed, nt->FileHeader.NumberOfSections);
    Log(L"=== RUNTIME SECTION CAPTURE END ===");
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


struct DescriptorTarget {
    const char* label;
    DWORD rva;
};

bool WriteCaptureFile(const std::wstring& path, const BYTE* bytes, SIZE_T size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log(L"TARGET_CAPTURE_WRITE_ERROR: file=%s error=%lu", path.c_str(), GetLastError());
        return false;
    }
    SIZE_T offset = 0;
    bool ok = true;
    while (offset < size) {
        const DWORD part = static_cast<DWORD>((size - offset > 1024 * 1024) ? 1024 * 1024 : size - offset);
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, part, &written, nullptr) || written != part) {
            Log(L"TARGET_CAPTURE_WRITE_ERROR: file=%s offset=0x%llX error=%lu",
                path.c_str(), static_cast<unsigned long long>(offset), GetLastError());
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && !FlushFileBuffers(file)) {
        Log(L"TARGET_CAPTURE_FLUSH_ERROR: file=%s error=%lu", path.c_str(), GetLastError());
        ok = false;
    }
    CloseHandle(file);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

bool IsPlausibleHeapPointer(const BYTE* imageBase, SIZE_T imageSize, std::uint64_t value) {
    if (value < 0x10000000000ull || value >= 0x0000800000000000ull || (value & 7ull) != 0) {
        return false;
    }
    const std::uint64_t imageStart = reinterpret_cast<std::uint64_t>(imageBase);
    return value < imageStart || value >= imageStart + imageSize;
}

void CaptureDescriptorTarget(const BYTE* imageBase, SIZE_T imageSize,
                             const DescriptorTarget& target, const std::wstring& directory) {
    constexpr SIZE_T descriptorSize = 0x200;
    if (target.rva >= imageSize || descriptorSize > imageSize - target.rva) {
        Log(L"TARGET_DESCRIPTOR_ERROR: name=%S reason=outside_image rva=0x%08X",
            target.label, target.rva);
        return;
    }

    std::vector<BYTE> descriptor(descriptorSize);
    SIZE_T bytesRead = 0;
    const BYTE* source = imageBase + target.rva;
    if (!ReadProcessMemory(GetCurrentProcess(), source, descriptor.data(), descriptor.size(), &bytesRead) ||
        bytesRead != descriptor.size()) {
        Log(L"TARGET_DESCRIPTOR_ERROR: name=%S reason=read_failed received=0x%llX error=%lu",
            target.label, static_cast<unsigned long long>(bytesRead), GetLastError());
        return;
    }

    wchar_t label[64]{};
    MultiByteToWideChar(CP_ACP, 0, target.label, -1, label, _countof(label));
    const std::wstring descriptorPath = directory + L"\\" + label + L"_descriptor.bin";
    if (!WriteCaptureFile(descriptorPath, descriptor.data(), descriptor.size())) return;
    Log(L"TARGET_DESCRIPTOR_OK: name=%s RVA=0x%08X size=0x%llX file=%s",
        label, target.rva, static_cast<unsigned long long>(descriptor.size()), descriptorPath.c_str());

    std::vector<std::uint64_t> seen;
    unsigned capturedRefs = 0;
    for (SIZE_T offset = 0; offset + 16 <= descriptor.size(); offset += 8) {
        std::uint64_t pointer = 0;
        std::uint32_t count = 0;
        std::uint32_t secondary = 0;
        std::memcpy(&pointer, descriptor.data() + offset, sizeof(pointer));
        std::memcpy(&count, descriptor.data() + offset + 8, sizeof(count));
        std::memcpy(&secondary, descriptor.data() + offset + 12, sizeof(secondary));

        // The native merge routine reads [array+0] as a pointer and [array+8]
        // as a 32-bit element count, then computes pointer + count * 8.
        // The following dword is NOT a capacity field; treating it as one
        // incorrectly rejects valid descriptors.
        if (!IsPlausibleHeapPointer(imageBase, imageSize, pointer) ||
            count == 0 || count > 4096) {
            continue;
        }
        if (std::find(seen.begin(), seen.end(), pointer) != seen.end()) continue;

        const SIZE_T captureSize = static_cast<SIZE_T>(count) * sizeof(std::uint64_t);
        MEMORY_BASIC_INFORMATION mbi{};
        const BYTE* heapAddress = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(pointer));
        if (VirtualQuery(heapAddress, &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !IsReadableProtection(mbi.Protect)) {
            Log(L"TARGET_HEAP_REF_SKIP: name=%s field=0x%llX pointer=%p count=%u secondary=%u reason=not_readable_private_memory",
                label, static_cast<unsigned long long>(offset), heapAddress, count, secondary);
            continue;
        }

        const BYTE* regionEnd = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= heapAddress ||
            static_cast<SIZE_T>(regionEnd - heapAddress) < captureSize) {
            Log(L"TARGET_HEAP_REF_SKIP: name=%s field=0x%llX pointer=%p count=%u secondary=%u requested=0x%llX reason=array_exceeds_readable_region",
                label, static_cast<unsigned long long>(offset), heapAddress, count, secondary,
                static_cast<unsigned long long>(captureSize));
            continue;
        }

        std::vector<BYTE> heapBytes(captureSize);
        bytesRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), heapAddress, heapBytes.data(), captureSize, &bytesRead) ||
            bytesRead != captureSize) {
            Log(L"TARGET_HEAP_REF_ERROR: name=%s field=0x%llX pointer=%p count=%u secondary=%u requested=0x%llX received=0x%llX error=%lu",
                label, static_cast<unsigned long long>(offset), heapAddress, count, secondary,
                static_cast<unsigned long long>(captureSize),
                static_cast<unsigned long long>(bytesRead), GetLastError());
            continue;
        }

        wchar_t fileName[128]{};
        swprintf_s(fileName, L"%s_ref_%02u_field_%03llX.bin", label, capturedRefs,
            static_cast<unsigned long long>(offset));
        const std::wstring heapPath = directory + L"\\" + fileName;
        if (!WriteCaptureFile(heapPath, heapBytes.data(), heapBytes.size())) continue;

        seen.push_back(pointer);
        Log(L"TARGET_HEAP_REF_OK: name=%s field=0x%llX pointer=%p count=%u capacity=%u captured=0x%llX file=%s",
            label, static_cast<unsigned long long>(offset), heapAddress, count, capacity,
            static_cast<unsigned long long>(captureSize), heapPath.c_str());
        ++capturedRefs;
    }

    Log(L"TARGET_DESCRIPTOR_SUMMARY: name=%s heap_arrays_captured=%u", label, capturedRefs);
}

bool WaitForDescriptorCaptureHotkey() {
    Log(L"CAPTURE_TRIGGER: enter Freedrive and press Ctrl+Shift+F10 once; waiting up to 10 minutes");
    const ULONGLONG deadline = GetTickCount64() + 10ull * 60ull * 1000ull;
    bool f10WasDown = false;
    while (GetTickCount64() < deadline) {
        const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        const bool modifiersDown =
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (f10Down && !f10WasDown && modifiersDown) {
            Log(L"CAPTURE_TRIGGERED: Ctrl+Shift+F10");
            return true;
        }
        f10WasDown = f10Down;
        Sleep(50);
    }
    Log(L"CAPTURE_TRIGGER_TIMEOUT: no hotkey received within 10 minutes");
    return false;
}

void CaptureTargetedTypeDescriptors() {
    HMODULE image = GetModuleHandleW(nullptr);
    if (!image) {
        Log(L"TARGET_CAPTURE_ERROR: main image handle unavailable");
        return;
    }
    const BYTE* base = reinterpret_cast<const BYTE*>(image);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        Log(L"TARGET_CAPTURE_ERROR: invalid DOS signature");
        return;
    }
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        Log(L"TARGET_CAPTURE_ERROR: invalid PE64 header");
        return;
    }

    Log(L"TARGET_CAPTURE_IMAGE: base=%p size=0x%08X timestamp=0x%08X",
        image, nt->OptionalHeader.SizeOfImage, nt->FileHeader.TimeDateStamp);
    if (nt->OptionalHeader.SizeOfImage != 0x05F94000u ||
        nt->FileHeader.TimeDateStamp != 0x68D1CFB8u) {
        Log(L"TARGET_CAPTURE_ABORT: executable fingerprint does not match analyzed build");
        return;
    }

    const std::wstring directory = GetCaptureRoot();
    if (directory.empty()) return;
    Log(L"=== TARGETED TYPE DESCRIPTOR CAPTURE BEGIN (read-only) ===");
    const DescriptorTarget targets[] = {
        { "avatar_tc2", 0x0344F430u },
        { "human_player", 0x0342CCC8u },
        { "ghost_player", 0x0342CDE0u },
        { "avatar_parent_descriptor", 0x0386C3B0u },
        { "player_shared_parent_descriptor", 0x038AE500u },
        { "mission_spawn_registry", 0x038B7430u },
    };
    for (const auto& target : targets) {
        CaptureDescriptorTarget(base, nt->OptionalHeader.SizeOfImage, target, directory);
    }
    Log(L"=== TARGETED TYPE DESCRIPTOR CAPTURE END ===");
}

DWORD WINAPI DiagnosticWorker(LPVOID) {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    Log(L"=== TC2 Walkmod diagnostic bootstrap 07 (targeted descriptor capture) ===");
    Log(L"Game executable: %s", exePath);
    LogImageInfo();
    LogModules();
    if (WaitForDescriptorCaptureHotkey()) {
        CaptureTargetedTypeDescriptors();
    }
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

