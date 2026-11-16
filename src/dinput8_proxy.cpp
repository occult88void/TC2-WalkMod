#include <windows.h>
#include <winnt.h>
#include <dinput.h>
#include <algorithm>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {
using DirectInput8CreateFn = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DllCanUnloadNowFn = HRESULT (WINAPI*)();
using DllGetClassObjectFn = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
using DllRegisterServerFn = HRESULT (WINAPI*)();
using DllUnregisterServerFn = HRESULT (WINAPI*)();
using GetdfDIJoystickFn = LPCDIDATAFORMAT (WINAPI*)();

HMODULE g_systemDinput8 = nullptr;
DirectInput8CreateFn g_DirectInput8Create = nullptr;
DllCanUnloadNowFn g_DllCanUnloadNow = nullptr;
DllGetClassObjectFn g_DllGetClassObject = nullptr;
DllRegisterServerFn g_DllRegisterServer = nullptr;
DllUnregisterServerFn g_DllUnregisterServer = nullptr;
GetdfDIJoystickFn g_GetdfDIJoystick = nullptr;

constexpr DWORD kImageSize = 0x05F94000u;
constexpr DWORD kImageTimestamp = 0x68D1CFB8u;
constexpr SIZE_T kDescriptorSize = 0x200;
constexpr SIZE_T kMaxArrayCount = 4096;
constexpr SIZE_T kObjectSize = 0x100;

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return L".\\";
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".\\";
    p.resize(slash + 1);
    return p;
}

void Log(const wchar_t* format, ...) {
    wchar_t message[2048]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
    va_end(args);
    int needed = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return;
    std::vector<char> utf8(static_cast<size_t>(needed));
    if (WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8.data(), needed, nullptr, nullptr) <= 0) return;
    FILE* f = nullptr;
    std::wstring path = ExeDir() + L"TC2_Walkmod.log";
    if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return;
    if (_fseeki64(f, 0, SEEK_END) == 0 && _ftelli64(f) == 0) {
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        fwrite(bom, 1, sizeof(bom), f);
    }
    fwrite(utf8.data(), 1, static_cast<size_t>(needed - 1), f);
    fwrite("\r\n", 1, 2, f);
    fclose(f);
}

bool ReadMemory(const void* address, void* output, SIZE_T size) {
    SIZE_T received = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, output, size, &received) != FALSE && received == size;
}

bool Readable(DWORD protect) {
    if ((protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    switch (protect & 0xFF) {
        case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
            return true;
        default: return false;
    }
}

bool WriteFileBytes(const std::wstring& path, const BYTE* data, SIZE_T size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"CAPTURE_FILE_ERROR: file=%s error=%lu", path.c_str(), GetLastError());
        return false;
    }
    SIZE_T offset = 0;
    bool ok = true;
    while (offset < size) {
        DWORD part = static_cast<DWORD>((size - offset > 1024 * 1024) ? 1024 * 1024 : size - offset);
        DWORD written = 0;
        if (!WriteFile(h, data + offset, part, &written, nullptr) || written != part) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && !FlushFileBuffers(h)) ok = false;
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

std::wstring NewCaptureDirectory() {
    std::wstring root = ExeDir() + L"TC2_runtime_capture";
    if (!CreateDirectoryW(root.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Log(L"CAPTURE_DIRECTORY_ERROR: %s error=%lu", root.c_str(), GetLastError());
        return L"";
    }
    wchar_t leaf[80]{};
    swprintf_s(leaf, L"capture_%lu_%llu", GetCurrentProcessId(),
               static_cast<unsigned long long>(GetTickCount64()));
    std::wstring dir = root + L"\\" + leaf;
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        Log(L"CAPTURE_DIRECTORY_ERROR: %s error=%lu", dir.c_str(), GetLastError());
        return L"";
    }
    return dir;
}

struct Target { const wchar_t* name; DWORD rva; };
struct ObjectRef { std::uint64_t address; std::vector<std::wstring> sources; };

bool PlausiblePointer(const BYTE* image, SIZE_T imageSize, std::uint64_t value) {
    if (value < 0x10000000000ull || value >= 0x0000800000000000ull || (value & 7ull)) return false;
    const std::uint64_t start = reinterpret_cast<std::uint64_t>(image);
    return value < start || value >= start + imageSize;
}

void AddObject(std::vector<ObjectRef>& objects, std::uint64_t address, const std::wstring& source) {
    auto it = std::find_if(objects.begin(), objects.end(),
        [address](const ObjectRef& item) { return item.address == address; });
    if (it == objects.end()) {
        ObjectRef item{};
        item.address = address;
        item.sources.push_back(source);
        objects.push_back(std::move(item));
    } else if (std::find(it->sources.begin(), it->sources.end(), source) == it->sources.end()) {
        it->sources.push_back(source);
    }
}

void CaptureObjects(const std::vector<ObjectRef>& objects, const std::wstring& dir) {
    Log(L"=== TARGET REFERENCED OBJECT CAPTURE BEGIN (read-only) ===");
    Log(L"TARGET_OBJECT_CANDIDATES: unique=%llu", static_cast<unsigned long long>(objects.size()));
    unsigned captured = 0, skipped = 0;
    for (const auto& item : objects) {
        const BYTE* address = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(item.address));
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !Readable(mbi.Protect)) {
            ++skipped;
            Log(L"TARGET_OBJECT_SKIP: address=%p reason=not_readable_private_memory", address);
            continue;
        }
        const BYTE* end = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
        if (end <= address) { ++skipped; continue; }
        SIZE_T available = static_cast<SIZE_T>(end - address);
        SIZE_T size = available < kObjectSize ? available : kObjectSize;
        if (size < 0x40) { ++skipped; continue; }
        std::vector<BYTE> bytes(size);
        if (!ReadMemory(address, bytes.data(), size)) {
            ++skipped;
            Log(L"TARGET_OBJECT_READ_ERROR: address=%p error=%lu", address, GetLastError());
            continue;
        }
        wchar_t name[96]{};
        swprintf_s(name, L"object_%03u_%016llX.bin", captured, static_cast<unsigned long long>(item.address));
        if (!WriteFileBytes(dir + L"\\" + name, bytes.data(), bytes.size())) { ++skipped; continue; }
        Log(L"TARGET_OBJECT_OK: address=%p captured=0x%llX sources=%llu file=%s",
            address, static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(item.sources.size()), name);
        for (const auto& src : item.sources) Log(L"TARGET_OBJECT_SOURCE: address=%p source=%s", address, src.c_str());
        ++captured;
    }
    Log(L"TARGET_OBJECT_SUMMARY: candidates=%llu captured=%u skipped=%u",
        static_cast<unsigned long long>(objects.size()), captured, skipped);
    Log(L"=== TARGET REFERENCED OBJECT CAPTURE END ===");
}

void CaptureDescriptor(const BYTE* image, SIZE_T imageSize, const Target& target,
                       const std::wstring& dir, std::vector<ObjectRef>& objects) {
    if (target.rva > imageSize || kDescriptorSize > imageSize - target.rva) {
        Log(L"TARGET_DESCRIPTOR_ERROR: name=%s reason=outside_image", target.name);
        return;
    }
    std::vector<BYTE> descriptor(kDescriptorSize);
    if (!ReadMemory(image + target.rva, descriptor.data(), descriptor.size())) {
        Log(L"TARGET_DESCRIPTOR_ERROR: name=%s reason=read_failed error=%lu", target.name, GetLastError());
        return;
    }
    std::wstring descriptorFile = std::wstring(target.name) + L"_descriptor.bin";
    if (!WriteFileBytes(dir + L"\\" + descriptorFile, descriptor.data(), descriptor.size())) return;
    Log(L"TARGET_DESCRIPTOR_OK: name=%s rva=0x%08X size=0x%llX file=%s",
        target.name, target.rva, static_cast<unsigned long long>(descriptor.size()), descriptorFile.c_str());

    std::vector<std::uint64_t> seen;
    unsigned arrays = 0;
    for (SIZE_T off = 0; off + 16 <= descriptor.size(); off += 8) {
        std::uint64_t pointer = 0;
        std::uint32_t count = 0, secondary = 0;
        memcpy(&pointer, descriptor.data() + off, 8);
        memcpy(&count, descriptor.data() + off + 8, 4);
        memcpy(&secondary, descriptor.data() + off + 12, 4);
        if (!PlausiblePointer(image, imageSize, pointer) || count == 0 || count > kMaxArrayCount) continue;
        if (std::find(seen.begin(), seen.end(), pointer) != seen.end()) continue;

        SIZE_T bytesNeeded = static_cast<SIZE_T>(count) * sizeof(std::uint64_t);
        const BYTE* array = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(pointer));
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(array, &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !Readable(mbi.Protect)) {
            Log(L"TARGET_HEAP_REF_SKIP: name=%s field=0x%llX pointer=%p count=%u reason=not_readable_private_memory",
                target.name, static_cast<unsigned long long>(off), array, count);
            continue;
        }
        const BYTE* end = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
        if (end <= array || static_cast<SIZE_T>(end - array) < bytesNeeded) {
            Log(L"TARGET_HEAP_REF_SKIP: name=%s field=0x%llX pointer=%p count=%u reason=array_exceeds_region",
                target.name, static_cast<unsigned long long>(off), array, count);
            continue;
        }
        std::vector<BYTE> contents(bytesNeeded);
        if (!ReadMemory(array, contents.data(), bytesNeeded)) {
            Log(L"TARGET_HEAP_REF_ERROR: name=%s field=0x%llX error=%lu",
                target.name, static_cast<unsigned long long>(off), GetLastError());
            continue;
        }
        for (SIZE_T i = 0; i < count; ++i) {
            std::uint64_t candidate = 0;
            memcpy(&candidate, contents.data() + i * 8, 8);
            if (!PlausiblePointer(image, imageSize, candidate)) continue;
            wchar_t source[160]{};
            swprintf_s(source, L"%s.field_%03llX.element_%llu", target.name,
                       static_cast<unsigned long long>(off), static_cast<unsigned long long>(i));
            AddObject(objects, candidate, source);
        }
        wchar_t arrayFile[128]{};
        swprintf_s(arrayFile, L"%s_ref_%02u_field_%03llX.bin", target.name, arrays,
                   static_cast<unsigned long long>(off));
        if (!WriteFileBytes(dir + L"\\" + arrayFile, contents.data(), contents.size())) continue;
        seen.push_back(pointer);
        Log(L"TARGET_HEAP_REF_OK: name=%s field=0x%llX pointer=%p count=%u secondary=%u captured=0x%llX file=%s",
            target.name, static_cast<unsigned long long>(off), array, count, secondary,
            static_cast<unsigned long long>(bytesNeeded), arrayFile);
        ++arrays;
    }
    Log(L"TARGET_DESCRIPTOR_SUMMARY: name=%s heap_arrays_captured=%u", target.name, arrays);
}

bool WaitForHotkey() {
    Log(L"CAPTURE_TRIGGER: enter Freedrive and press Ctrl+Shift+F10 once; waiting up to 10 minutes");
    ULONGLONG deadline = GetTickCount64() + 10ull * 60ull * 1000ull;
    bool wasDown = false;
    while (GetTickCount64() < deadline) {
        bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
                    (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (f10 && !wasDown && mods) {
            Log(L"CAPTURE_TRIGGERED: Ctrl+Shift+F10");
            return true;
        }
        wasDown = f10;
        Sleep(50);
    }
    Log(L"CAPTURE_TRIGGER_TIMEOUT: no hotkey within 10 minutes");
    return false;
}


struct CodeTarget { const wchar_t* name; DWORD rva; SIZE_T size; };

bool IsExecutableRange(const BYTE* image, const IMAGE_NT_HEADERS64& nt, DWORD rva, SIZE_T size) {
    auto* actualNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image +
        reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(actualNt);
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i, ++section) {
        const DWORD span = (std::max)(section->Misc.VirtualSize, section->SizeOfRawData);
        if (rva >= section->VirtualAddress &&
            static_cast<std::uint64_t>(rva) + size <=
                static_cast<std::uint64_t>(section->VirtualAddress) + span &&
            (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) return true;
    }
    return false;
}

bool CaptureWindow(const BYTE* address, SIZE_T requested, const std::wstring& path,
                   const wchar_t* tag) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || !Readable(mbi.Protect)) {
        Log(L"VER09_WINDOW_SKIP: tag=%s address=%p reason=not_readable", tag, address);
        return false;
    }
    const BYTE* end = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
    if (end <= address) return false;
    SIZE_T size = (std::min)(requested, static_cast<SIZE_T>(end - address));
    if (size < 8) {
        Log(L"VER09_WINDOW_SKIP: tag=%s address=%p reason=short_region size=0x%llX",
            tag, address, static_cast<unsigned long long>(size));
        return false;
    }
    std::vector<BYTE> bytes(size);
    if (!ReadMemory(address, bytes.data(), size)) {
        Log(L"VER09_WINDOW_READ_ERROR: tag=%s address=%p error=%lu", tag, address, GetLastError());
        return false;
    }
    if (!WriteFileBytes(path, bytes.data(), bytes.size())) return false;
    std::uint64_t first = 0, second = 0;
    memcpy(&first, bytes.data(), 8);
    if (bytes.size() >= 16) memcpy(&second, bytes.data() + 8, 8);
    Log(L"VER09_WINDOW_OK: tag=%s address=%p size=0x%llX qword0=%016llX qword1=%016llX file=%s",
        tag, address, static_cast<unsigned long long>(size),
        static_cast<unsigned long long>(first), static_cast<unsigned long long>(second),
        path.c_str());
    return true;
}

void CaptureNativeCode(const BYTE* image, SIZE_T imageSize, const IMAGE_NT_HEADERS64& nt,
                       const std::wstring& dir) {
    Log(L"=== VER09 NATIVE LIFECYCLE CODE WINDOWS BEGIN (read-only) ===");
    const CodeTarget targets[] = {
        {L"destroy_current_vehicle_avatar_mode", 0x001FA430u, 0x180},
        {L"spawn_replica_vehicle",               0x001FA530u, 0x180},
        {L"avatar_vehicle_lifecycle_caller",     0x00204800u, 0x600},
        {L"vehicle_cleanup_helper",              0x00841100u, 0x240},
        {L"vehicle_cleanup_helper_inner",        0x00841780u, 0x300},
    };
    unsigned captured = 0, skipped = 0;
    for (const auto& target : targets) {
        if (target.rva > imageSize || target.size > imageSize - target.rva ||
            !IsExecutableRange(image, nt, target.rva, target.size)) {
            ++skipped;
            Log(L"VER09_CODE_SKIP: name=%s rva=0x%08X size=0x%llX reason=not_executable_or_outside_image",
                target.name, target.rva, static_cast<unsigned long long>(target.size));
            continue;
        }
        std::vector<BYTE> bytes(target.size);
        if (!ReadMemory(image + target.rva, bytes.data(), bytes.size())) {
            ++skipped;
            Log(L"VER09_CODE_READ_ERROR: name=%s rva=0x%08X error=%lu",
                target.name, target.rva, GetLastError());
            continue;
        }
        std::wstring file = std::wstring(target.name) + L"_rva_" +
            std::to_wstring(target.rva) + L".bin";
        if (!WriteFileBytes(dir + L"\\" + file, bytes.data(), bytes.size())) {
            ++skipped;
            continue;
        }
        std::uint64_t first = 0, second = 0;
        memcpy(&first, bytes.data(), 8);
        memcpy(&second, bytes.data() + 8, 8);
        Log(L"VER09_CODE_OK: name=%s rva=0x%08X address=%p size=0x%llX qword0=%016llX qword1=%016llX file=%s",
            target.name, target.rva, image + target.rva,
            static_cast<unsigned long long>(bytes.size()),
            static_cast<unsigned long long>(first), static_cast<unsigned long long>(second), file.c_str());
        ++captured;
    }
    Log(L"VER09_CODE_SUMMARY: targets=%llu captured=%u skipped=%u",
        static_cast<unsigned long long>(_countof(targets)), captured, skipped);
    Log(L"=== VER09 NATIVE LIFECYCLE CODE WINDOWS END ===");
}

void CaptureManagerCandidate(const BYTE* image, SIZE_T imageSize, const std::wstring& dir) {
    constexpr DWORD kGlobalPointerRva = 0x0343C630u;
    constexpr SIZE_T kManagerWindowSize = 0x2000;
    const DWORD fieldOffsets[] = {0x1DD8u, 0x1DE8u, 0x1E68u};
    Log(L"=== VER09 GLOBAL MANAGER CANDIDATE BEGIN (read-only) ===");
    if (kGlobalPointerRva > imageSize || sizeof(std::uint64_t) > imageSize - kGlobalPointerRva) {
        Log(L"VER09_MANAGER_ABORT: global slot outside image");
        return;
    }
    std::uint64_t manager = 0;
    if (!ReadMemory(image + kGlobalPointerRva, &manager, sizeof(manager))) {
        Log(L"VER09_MANAGER_ABORT: global slot read failed error=%lu", GetLastError());
        return;
    }
    WriteFileBytes(dir + L"\\manager_global_slot_rva_0343C630.bin",
                   reinterpret_cast<const BYTE*>(&manager), sizeof(manager));
    Log(L"VER09_MANAGER_GLOBAL_SLOT: rva=0x%08X address=%p value=%016llX",
        kGlobalPointerRva, image + kGlobalPointerRva, static_cast<unsigned long long>(manager));
    if (!PlausiblePointer(image, imageSize, manager)) {
        Log(L"VER09_MANAGER_ABORT: global value is not a plausible external pointer");
        return;
    }
    const BYTE* base = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(manager));
    if (!CaptureWindow(base, kManagerWindowSize, dir + L"\\manager_candidate_root.bin",
                       L"manager_candidate_root")) {
        Log(L"VER09_MANAGER_ABORT: root window unavailable");
        return;
    }
    for (DWORD offset : fieldOffsets) {
        std::uint64_t value = 0;
        const BYTE* field = base + offset;
        if (!ReadMemory(field, &value, sizeof(value))) {
            Log(L"VER09_MANAGER_FIELD_SKIP: offset=0x%X reason=read_failed error=%lu", offset, GetLastError());
            continue;
        }
        Log(L"VER09_MANAGER_FIELD: offset=0x%X address=%p value=%016llX",
            offset, field, static_cast<unsigned long long>(value));
        wchar_t name[96]{};
        swprintf_s(name, L"manager_field_%04X_target.bin", offset);
        if (PlausiblePointer(image, imageSize, value)) {
            CaptureWindow(reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(value)),
                          0x200, dir + L"\\" + name, L"manager_field_target");
        }
    }
    Log(L"=== VER09 GLOBAL MANAGER CANDIDATE END ===");
}

void RunCapture() {
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) { Log(L"TARGET_CAPTURE_ABORT: main module unavailable"); return; }
    const BYTE* image = reinterpret_cast<const BYTE*>(module);
    IMAGE_DOS_HEADER dos{};
    if (!ReadMemory(image, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        Log(L"TARGET_CAPTURE_ABORT: invalid DOS header"); return;
    }
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadMemory(image + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        Log(L"TARGET_CAPTURE_ABORT: invalid PE64/AMD64 image"); return;
    }
    Log(L"TARGET_CAPTURE_IMAGE: base=%p size=0x%08X timestamp=0x%08X machine=0x%04X",
        module, nt.OptionalHeader.SizeOfImage, nt.FileHeader.TimeDateStamp, nt.FileHeader.Machine);
    if (nt.OptionalHeader.SizeOfImage != kImageSize || nt.FileHeader.TimeDateStamp != kImageTimestamp) {
        Log(L"TARGET_CAPTURE_ABORT: executable fingerprint mismatch expected_size=0x%08X expected_timestamp=0x%08X",
            kImageSize, kImageTimestamp);
        return;
    }

    std::wstring dir = NewCaptureDirectory();
    if (dir.empty()) return;
    Log(L"TARGET_CAPTURE_DIRECTORY: %s", dir.c_str());
    Log(L"=== TARGETED TYPE DESCRIPTOR CAPTURE BEGIN (read-only) ===");
    const Target targets[] = {
        {L"avatar_tc2", 0x0344F430u},
        {L"human_player", 0x0342CCC8u},
        {L"ghost_player", 0x0342CDE0u},
        {L"avatar_parent_descriptor", 0x0386C3B0u},
        {L"player_shared_parent_descriptor", 0x038AE500u},
        {L"mission_spawn_registry", 0x038B7430u},
    };
    std::vector<ObjectRef> objects;
    for (const auto& target : targets) CaptureDescriptor(image, nt.OptionalHeader.SizeOfImage, target, dir, objects);
    CaptureObjects(objects, dir);
    Log(L"=== TARGETED TYPE DESCRIPTOR CAPTURE END ===");
    CaptureNativeCode(image, nt.OptionalHeader.SizeOfImage, nt, dir);
    CaptureManagerCandidate(image, nt.OptionalHeader.SizeOfImage, dir);
    Log(L"TARGET_CAPTURE_COMPLETE: directory=%s", dir.c_str());
}

DWORD WINAPI Worker(LPVOID) {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log(L"=== TC2 Walkmod diagnostic bootstrap 09 (native lifecycle capture) ===");
    Log(L"BOOTSTRAP_VERSION: 09");
    Log(L"Game executable: %s", exe);
    if (WaitForHotkey()) RunCapture();
    return 0;
}

bool LoadOriginal() {
    if (g_systemDinput8) return true;
    wchar_t systemDir[MAX_PATH]{};
    UINT n = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (!n || n >= MAX_PATH) { Log(L"PROXY_ERROR: GetSystemDirectoryW error=%lu", GetLastError()); return false; }
    std::wstring path(systemDir, n);
    path += L"\\dinput8.dll";
    g_systemDinput8 = LoadLibraryW(path.c_str());
    if (!g_systemDinput8) { Log(L"PROXY_ERROR: System32 dinput8.dll load failed error=%lu", GetLastError()); return false; }
    g_DirectInput8Create = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(g_systemDinput8, "DirectInput8Create"));
    g_DllCanUnloadNow = reinterpret_cast<DllCanUnloadNowFn>(GetProcAddress(g_systemDinput8, "DllCanUnloadNow"));
    g_DllGetClassObject = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(g_systemDinput8, "DllGetClassObject"));
    g_DllRegisterServer = reinterpret_cast<DllRegisterServerFn>(GetProcAddress(g_systemDinput8, "DllRegisterServer"));
    g_DllUnregisterServer = reinterpret_cast<DllUnregisterServerFn>(GetProcAddress(g_systemDinput8, "DllUnregisterServer"));
    g_GetdfDIJoystick = reinterpret_cast<GetdfDIJoystickFn>(GetProcAddress(g_systemDinput8, "GetdfDIJoystick"));
    bool ok = g_DirectInput8Create && g_DllCanUnloadNow && g_DllGetClassObject &&
              g_DllRegisterServer && g_DllUnregisterServer && g_GetdfDIJoystick;
    Log(L"PROXY: System32 dinput8.dll loaded; required_exports=%s", ok ? L"all_present" : L"missing");
    return ok;
}
} // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE a, DWORD b, REFIID c, LPVOID* d, LPUNKNOWN e) {
    if (!LoadOriginal() || !g_DirectInput8Create) return E_FAIL;
    return g_DirectInput8Create(a, b, c, d, e);
}
extern "C" HRESULT WINAPI DllCanUnloadNow() {
    if (!LoadOriginal() || !g_DllCanUnloadNow) return S_FALSE;
    return g_DllCanUnloadNow();
}
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID a, REFIID b, LPVOID* c) {
    if (!LoadOriginal() || !g_DllGetClassObject) return CLASS_E_CLASSNOTAVAILABLE;
    return g_DllGetClassObject(a, b, c);
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
        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
