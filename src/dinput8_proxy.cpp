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
    }    Log(L"TARGET_DESCRIPTOR_SUMMARY: name=%s heap_arrays_captured=%u", target.name, arrays);
}

bool WaitForHotkey(int key, const wchar_t* label) {
    Log(L"SNAPSHOT_WAIT: label=%s hotkey=Ctrl+Shift+F%d timeout=10min",
        label, key == VK_F10 ? 10 : (key == VK_F11 ? 11 : 12));
    ULONGLONG deadline = GetTickCount64() + 10ull * 60ull * 1000ull;
    bool wasDown = false;
    while (GetTickCount64() < deadline) {
        bool keyDown = (GetAsyncKeyState(key) & 0x8000) != 0;
        bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
                    (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (keyDown && !wasDown && mods) {
            Log(L"SNAPSHOT_TRIGGERED: label=%s key=%d", label, key);
            return true;
        }
        wasDown = keyDown;
        Sleep(50);
    }
    Log(L"SNAPSHOT_TIMEOUT: label=%s key=%d", label, key);
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
    Log(L"=== BOOTSTRAP13 NATIVE TRANSITION CODE CAPTURE BEGIN (read-only) ===");
    // Neutral capture labels only; these are not recovered native symbols.
    const CodeTarget targets[] = {
        {L"candidate_transition_helper_A", 0x001FA430u, 0x120},
        {L"candidate_transition_helper_B", 0x001FA530u, 0x420},
        {L"candidate_transition_update",   0x00204800u, 0xC00},
        {L"candidate_cleanup_helper",     0x00841100u, 0x240},
        {L"candidate_cleanup_inner",      0x00841780u, 0x180},
        {L"direct_callee_00796CC0",       0x00796CC0u, 0x200},
        {L"followup_callee_006E0560",     0x006E0560u, 0x200},
        {L"followup_callee_1DAB5B0",      0x1DAB5B0u, 0x200},
        {L"followup_callee_008F8DC0",     0x008F8DC0u, 0x200},
        {L"followup_callee_00904370",     0x00904370u, 0x200},
        {L"followup_callee_001CB6B0",     0x001CB6B0u, 0x200},
        {L"followup_callee_001E3B40",     0x001E3B40u, 0x200},
        {L"followup_callee_00772880",     0x00772880u, 0x200},
        {L"direct_callee_00796C70",       0x00796C70u, 0x200},
        {L"direct_callee_0076DED0",       0x0076DED0u, 0x200},
        {L"direct_callee_008D9CE0",       0x008D9CE0u, 0x200},
        {L"direct_callee_008E4D60",       0x008E4D60u, 0x200},
        {L"direct_callee_008DA550",       0x008DA550u, 0x200},
        {L"direct_callee_00A10980",       0x00A10980u, 0x200},
        {L"direct_callee_00A11300",       0x00A11300u, 0x200},
        {L"direct_callee_00A10470",       0x00A10470u, 0x200},
        {L"direct_callee_001F9250",       0x001F9250u, 0x200},
        {L"direct_callee_011DDA0",        0x011DDA0u,  0x200},
    };
    unsigned captured = 0, skipped = 0;
    for (const auto& target : targets) {
        if (target.rva > imageSize || target.size > imageSize - target.rva ||
            !IsExecutableRange(image, nt, target.rva, target.size)) {
            ++skipped;
            Log(L"BOOT13_CODE_SKIP: name=%s rva=0x%08X size=0x%llX reason=not_executable_or_outside_image",
                target.name, target.rva, static_cast<unsigned long long>(target.size));
            continue;
        }
        std::wstring file = std::wstring(target.name) + L"_rva_" +
            std::to_wstring(target.rva) + L".bin";
        const bool ok = CaptureWindow(image + target.rva, target.size,
                                      dir + L"\\" + file, L"boot13_native_code");
        if (!ok) {
            ++skipped;
            continue;
        }
        Log(L"BOOT13_CODE_OK: name=%s rva=0x%08X requested=0x%llX file=%s",
            target.name, target.rva, static_cast<unsigned long long>(target.size), file.c_str());
        ++captured;
    }
    Log(L"BOOT13_CODE_SUMMARY: targets=%llu captured=%u skipped=%u",
        static_cast<unsigned long long>(_countof(targets)), captured, skipped);
    Log(L"=== BOOTSTRAP13 NATIVE TRANSITION CODE CAPTURE END ===");
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



bool IsExecutableAddress(const BYTE* address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || !Readable(mbi.Protect)) return false;
    const DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

bool IsCanonicalAlignedPointer(std::uint64_t value) {
    return value >= 0x10000000000ull &&
           value < 0x0000800000000000ull &&
           (value & 7ull) == 0;
}

bool CaptureObjectAndVtable(const BYTE* image, SIZE_T imageSize,
                            const IMAGE_NT_HEADERS64& nt,
                            std::uint64_t objectValue, const wchar_t* label,
                            const std::wstring& dir,
                            std::vector<std::uint64_t>& capturedObjects,
                            std::vector<std::uint64_t>& capturedFunctions) {
    if (!PlausiblePointer(image, imageSize, objectValue)) {
        Log(L"BOOT15_OBJECT_SKIP: label=%s value=%016llX reason=not_plausible_external_pointer",
            label, static_cast<unsigned long long>(objectValue));
        return false;
    }
    if (std::find(capturedObjects.begin(), capturedObjects.end(), objectValue) != capturedObjects.end()) {
        Log(L"BOOT14_OBJECT_ALIAS: label=%s object=%p reason=already_captured",
            label, reinterpret_cast<void*>(static_cast<std::uintptr_t>(objectValue)));
        return true;
    }
    capturedObjects.push_back(objectValue);

    const BYTE* object = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(objectValue));
    wchar_t objectFile[192]{};
    swprintf_s(objectFile, L"%s_object.bin", label);
    const bool objectOk = CaptureWindow(object, 0x400, dir + L"\\" + objectFile, L"boot14_object");
    std::uint64_t vtableValue = 0;
    if (!ReadMemory(object, &vtableValue, sizeof(vtableValue))) {
        Log(L"BOOT14_OBJECT_VTABLE_SKIP: label=%s object=%p reason=first_qword_unreadable",
            label, object);
        return objectOk;
    }
    Log(L"BOOT14_OBJECT_VTABLE: label=%s object=%p vtable=%016llX",
        label, object, static_cast<unsigned long long>(vtableValue));
    if (!IsCanonicalAlignedPointer(vtableValue)) {
        Log(L"BOOT14_VTABLE_SKIP: label=%s value=%016llX reason=not_canonical_aligned_pointer",
            label, static_cast<unsigned long long>(vtableValue));
        return objectOk;
    }

    const BYTE* vtable = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(vtableValue));
    std::uint64_t entries[32]{};
    SIZE_T entryCount = 32;
    if (!ReadMemory(vtable, entries, sizeof(entries))) {
        entryCount = 16;
        if (!ReadMemory(vtable, entries, entryCount * sizeof(std::uint64_t))) {
            Log(L"BOOT14_VTABLE_READ_ERROR: label=%s vtable=%p reason=16_and_32_entry_reads_failed",
                label, vtable);
            return objectOk;
        }
    }

    wchar_t vtableFile[192]{};
    swprintf_s(vtableFile, L"%s_actual_vtable_%02llu.bin", label,
               static_cast<unsigned long long>(entryCount));
    CaptureWindow(vtable, entryCount * sizeof(std::uint64_t),
                  dir + L"\\" + vtableFile, L"boot14_actual_vtable");
    Log(L"BOOT14_VTABLE_OK: label=%s vtable=%p entries=%llu file=%s",
        label, vtable, static_cast<unsigned long long>(entryCount), vtableFile);

    const std::uint64_t imageBase = reinterpret_cast<std::uint64_t>(image);
    unsigned executable = 0, saved = 0, duplicate = 0, outside = 0;
    for (SIZE_T i = 0; i < entryCount; ++i) {
        const std::uint64_t target = entries[i];
        if (!target || target < imageBase || target - imageBase >= imageSize) {
            if (target) ++outside;
            continue;
        }
        const DWORD rva = static_cast<DWORD>(target - imageBase);
        if (!IsExecutableRange(image, nt, rva, 0x80)) continue;
        ++executable;
        if (std::find(capturedFunctions.begin(), capturedFunctions.end(), target) != capturedFunctions.end()) {
            ++duplicate;
            Log(L"BOOT14_VFUNC_DUPLICATE: label=%s slot=%llu address=%p rva=0x%08X",
                label, static_cast<unsigned long long>(i),
                reinterpret_cast<const void*>(static_cast<std::uintptr_t>(target)), rva);
            continue;
        }
        capturedFunctions.push_back(target);
        wchar_t codeFile[224]{};
        swprintf_s(codeFile, L"%s_vfunc_%02llu_rva_%08X.bin", label,
                   static_cast<unsigned long long>(i), rva);
        if (CaptureWindow(image + rva, 0x100, dir + L"\\" + codeFile, L"boot14_vfunc_code")) {
            ++saved;
        }
        Log(L"BOOT14_VFUNC: label=%s slot=%llu address=%p image_rva=0x%08X file=%s",
            label, static_cast<unsigned long long>(i),
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(target)), rva, codeFile);
    }
    Log(L"BOOT14_VFUNC_SUMMARY: label=%s executable=%u saved=%u duplicate=%u outside_image=%u",
        label, executable, saved, duplicate, outside);
    return objectOk;
}


bool CaptureStableWindow(const BYTE* address, SIZE_T requested,
                         const std::wstring& path, const wchar_t* tag) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || !Readable(mbi.Protect)) {
        Log(L"BOOT15_STABLE_SKIP: tag=%s address=%p reason=not_readable", tag, address);
        return false;
    }
    const BYTE* end = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
    if (end <= address) return false;
    const SIZE_T size = (std::min)(requested, static_cast<SIZE_T>(end - address));
    if (size < 8) {
        Log(L"BOOT15_STABLE_SKIP: tag=%s address=%p reason=short_region size=0x%llX",
            tag, address, static_cast<unsigned long long>(size));
        return false;
    }
    std::vector<BYTE> first(size), second(size);
    bool stable = false;
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        if (!ReadMemory(address, first.data(), size) ||
            !ReadMemory(address, second.data(), size)) {
            Log(L"BOOT15_STABLE_READ_ERROR: tag=%s attempt=%u error=%lu",
                tag, attempt, GetLastError());
            continue;
        }
        if (memcmp(first.data(), second.data(), size) == 0) {
            stable = true;
            break;
        }
        Log(L"BOOT15_STABLE_CHANGED: tag=%s attempt=%u size=0x%llX",
            tag, attempt, static_cast<unsigned long long>(size));
        Sleep(10);
    }
    if (!stable) {
        Log(L"BOOT15_STABLE_UNCONFIRMED: tag=%s address=%p size=0x%llX",
            tag, address, static_cast<unsigned long long>(size));
        return false;
    }
    if (!WriteFileBytes(path, first.data(), first.size())) return false;
    std::uint64_t q0 = 0, q1 = 0;
    memcpy(&q0, first.data(), 8);
    if (first.size() >= 16) memcpy(&q1, first.data() + 8, 8);
    Log(L"BOOT15_STABLE_OK: tag=%s address=%p size=0x%llX qword0=%016llX qword1=%016llX file=%s",
        tag, address, static_cast<unsigned long long>(size),
        static_cast<unsigned long long>(q0), static_cast<unsigned long long>(q1),
        path.c_str());
    return true;
}

bool CaptureCodeRva15(const BYTE* image, SIZE_T imageSize, const IMAGE_NT_HEADERS64& nt,
                      DWORD rva, SIZE_T size, const wchar_t* label,
                      const std::wstring& dir) {
    if (rva >= imageSize || size > imageSize - rva ||
        !IsExecutableRange(image, nt, rva, size)) {
        Log(L"BOOT15_CODE_SKIP: label=%s rva=0x%08X size=0x%llX reason=not_executable_or_outside_image",
            label, rva, static_cast<unsigned long long>(size));
        return false;
    }
    wchar_t file[192]{};
    swprintf_s(file, L"%s_rva_%08X.bin", label, rva);
    return CaptureStableWindow(image + rva, size, dir + L"\\" + file, L"boot15_code");
}

void CaptureVtableDepth15(const BYTE* image, SIZE_T imageSize, const IMAGE_NT_HEADERS64& nt,
                          std::uint64_t vtableValue, const wchar_t* label,
                          const std::wstring& dir,
                          std::vector<std::uint64_t>& capturedFunctions) {
    if (!IsCanonicalAlignedPointer(vtableValue)) {
        Log(L"BOOT15_TRUE_VTABLE_SKIP: label=%s value=%016llX reason=not_canonical_aligned",
            label, static_cast<unsigned long long>(vtableValue));
        return;
    }
    const BYTE* vtable = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(vtableValue));
    std::uint64_t entries[32]{};
    SIZE_T count = 32;
    if (!ReadMemory(vtable, entries, sizeof(entries))) {
        count = 16;
        if (!ReadMemory(vtable, entries, count * sizeof(std::uint64_t))) {
            Log(L"BOOT15_TRUE_VTABLE_READ_ERROR: label=%s address=%p", label, vtable);
            return;
        }
    }
    wchar_t file[192]{};
    swprintf_s(file, L"%s_true_vtable_%02llu.bin", label, static_cast<unsigned long long>(count));
    CaptureStableWindow(vtable, count * sizeof(std::uint64_t),
                        dir + L"\\" + file, L"boot15_true_vtable");
    Log(L"BOOT15_TRUE_VTABLE: label=%s address=%p entries=%llu file=%s",
        label, vtable, static_cast<unsigned long long>(count), file);
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(image);
    for (SIZE_T i = 0; i < count; ++i) {
        const std::uint64_t target = entries[i];
        if (!target || target < base || target - base >= imageSize) continue;
        const DWORD rva = static_cast<DWORD>(target - base);
        if (!IsExecutableRange(image, nt, rva, 0x40)) continue;
        if (std::find(capturedFunctions.begin(), capturedFunctions.end(), target) != capturedFunctions.end())
            continue;
        capturedFunctions.push_back(target);
        wchar_t codeLabel[192]{};
        swprintf_s(codeLabel, L"%s_vslot_%02llu", label, static_cast<unsigned long long>(i));
        wchar_t codeFile[224]{};
        swprintf_s(codeFile, L"%s_vslot_%02llu_rva_%08X.bin",
                   label, static_cast<unsigned long long>(i), rva);
        CaptureStableWindow(image + rva, 0x100, dir + L"\\" + codeFile, L"boot15_vtable_code");
        Log(L"BOOT15_TRUE_VTABLE_FUNCTION: label=%s slot=%llu rva=0x%08X file=%s",
            codeLabel, static_cast<unsigned long long>(i), rva, codeFile);
    }
}

void CaptureCodePointerFields15(const BYTE* image, SIZE_T imageSize, const IMAGE_NT_HEADERS64& nt,
                                const BYTE* object, SIZE_T requested, const wchar_t* label,
                                const std::wstring& dir,
                                std::vector<std::uint64_t>& capturedFunctions) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!object || VirtualQuery(object, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || !Readable(mbi.Protect)) return;
    const BYTE* end = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
    if (end <= object) return;
    const SIZE_T size = (std::min)(requested, static_cast<SIZE_T>(end - object));
    if (size < 8) return;
    std::vector<BYTE> bytes(size);
    if (!ReadMemory(object, bytes.data(), size)) return;
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(image);
    for (SIZE_T off = 0; off + 8 <= size; off += 8) {
        std::uint64_t value = 0;
        memcpy(&value, bytes.data() + off, 8);
        if (value < base || value - base >= imageSize) continue;
        const DWORD rva = static_cast<DWORD>(value - base);
        if (!IsExecutableRange(image, nt, rva, 0x40)) continue;
        if (std::find(capturedFunctions.begin(), capturedFunctions.end(), value) != capturedFunctions.end())
            continue;
        capturedFunctions.push_back(value);
        wchar_t file[224]{};
        swprintf_s(file, L"%s_field_%04llX_code_rva_%08X.bin",
                   label, static_cast<unsigned long long>(off), rva);
        CaptureStableWindow(image + rva, 0x100, dir + L"\\" + file, L"boot15_object_code_pointer");
        Log(L"BOOT15_CODE_POINTER_FIELD: label=%s byte_offset=0x%llX rva=0x%08X file=%s",
            label, static_cast<unsigned long long>(off), rva, file);
    }
}

void CaptureHostWrapperDepth15(const BYTE* image, SIZE_T imageSize, const IMAGE_NT_HEADERS64& nt,
                               const BYTE* root, DWORD offset, const wchar_t* state,
                               const std::wstring& dir,
                               std::vector<std::uint64_t>& capturedFunctions) {
    std::uint64_t wrapperValue = 0;
    if (!ReadMemory(root + offset, &wrapperValue, sizeof(wrapperValue)) ||
        !PlausiblePointer(image, imageSize, wrapperValue)) {
        Log(L"BOOT15_WRAPPER_SKIP: state=%s offset=0x%X value=%016llX",
            state, offset, static_cast<unsigned long long>(wrapperValue));
        return;
    }
    const BYTE* wrapper = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(wrapperValue));
    wchar_t label[128]{};
    swprintf_s(label, L"%s_host_%04X", state, offset);
    wchar_t file[192]{};
    swprintf_s(file, L"%s_wrapper.bin", label);
    CaptureStableWindow(wrapper, 0x200, dir + L"\\" + file, L"boot15_wrapper");
    CaptureCodePointerFields15(image, imageSize, nt, wrapper, 0x200, label, dir, capturedFunctions);

    std::uint64_t nestedValue = 0;
    if (!ReadMemory(wrapper, &nestedValue, sizeof(nestedValue)) ||
        !PlausiblePointer(image, imageSize, nestedValue)) {
        Log(L"BOOT15_WRAPPER_NESTED_SKIP: label=%s wrapper=%p first_qword=%016llX",
            label, wrapper, static_cast<unsigned long long>(nestedValue));
        return;
    }
    const BYTE* nested = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(nestedValue));
    wchar_t nestedLabel[160]{};
    swprintf_s(nestedLabel, L"%s_nested", label);
    swprintf_s(file, L"%s_nested_object.bin", label);
    CaptureStableWindow(nested, 0x800, dir + L"\\" + file, L"boot15_nested_object");
    CaptureCodePointerFields15(image, imageSize, nt, nested, 0x800, nestedLabel, dir, capturedFunctions);

    std::uint64_t trueVtable = 0;
    if (ReadMemory(nested, &trueVtable, sizeof(trueVtable))) {
        Log(L"BOOT15_POINTER_DEPTH: state=%s host_offset=0x%X wrapper=%p nested=%p true_vtable=%016llX",
            state, offset, wrapper, nested, static_cast<unsigned long long>(trueVtable));
        wchar_t vtableLabel[160]{};
        swprintf_s(vtableLabel, L"%s_nested", label);
        CaptureVtableDepth15(image, imageSize, nt, trueVtable, vtableLabel, dir, capturedFunctions);
    }
}

void CaptureInteriorReference15(const BYTE* image, SIZE_T imageSize, const BYTE* owner,
                                SIZE_T offset, const wchar_t* label, const std::wstring& dir) {
    std::uint64_t value = 0;
    if (!ReadMemory(owner + offset, &value, sizeof(value))) {
        Log(L"BOOT15_INTERIOR_REF_SKIP: label=%s offset=0x%llX reason=unreadable",
            label, static_cast<unsigned long long>(offset));
        return;
    }
    Log(L"BOOT15_INTERIOR_REF: label=%s offset=0x%llX value=%016llX",
        label, static_cast<unsigned long long>(offset), static_cast<unsigned long long>(value));
    if (!PlausiblePointer(image, imageSize, value)) return;
    wchar_t file[192]{};
    swprintf_s(file, L"%s_ref_%04llX.bin", label, static_cast<unsigned long long>(offset));
    CaptureStableWindow(reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(value)),
                        0x200, dir + L"\\" + file, L"boot15_interior_reference");
}

void CaptureBootstrap15CodeTargets(const BYTE* image, SIZE_T imageSize,
                                   const IMAGE_NT_HEADERS64& nt, const std::wstring& dir) {
    const CodeTarget targets[] = {
        {L"host_6d0_vehicle_pointer_b2ff30", 0x00B2FF30u, 0x180},
        {L"host_6d0_vehicle_cleanup_b2ff50", 0x00B2FF50u, 0x180},
        {L"host_6d0_vehicle_pointer_b2faa0", 0x00B2FAA0u, 0x180},
        {L"host_6d0_vehicle_thunk_b2f430", 0x00B2F430u, 0x100},
        {L"host_6d0_thunk_b77fa0", 0x00B77FA0u, 0x100},
        {L"host_6d0_thunk_b78010", 0x00B78010u, 0x100},
        {L"host_6d0_thunk_b948e0", 0x00B948E0u, 0x100},
        {L"host_6d0_thunk_b78150", 0x00B78150u, 0x100},
        {L"host_6d0_thunk_b786a0", 0x00B786A0u, 0x100},
        {L"host_6d0_thunk_b78710", 0x00B78710u, 0x100},
        {L"host_6d0_thunk_b96590", 0x00B96590u, 0x100},
        {L"host_6d0_hq_thunk_e63e40", 0x00E63E40u, 0x100},
        {L"host_6d0_hq_target_eccee0", 0x00ECCEE0u, 0x180}
    };
    unsigned captured = 0, skipped = 0;
    for (const auto& target : targets) {
        if (CaptureCodeRva15(image, imageSize, nt, target.rva, target.size, target.name, dir))
            ++captured;
        else
            ++skipped;
    }
    Log(L"BOOT15_CODE_TARGET_SUMMARY: targets=%llu captured=%u skipped=%u",
        static_cast<unsigned long long>(_countof(targets)), captured, skipped);
}

void CaptureTransitionHostSnapshot(const BYTE* image, SIZE_T imageSize,
                                   const IMAGE_NT_HEADERS64& nt,
                                   const wchar_t* label, const std::wstring& dir) {
    constexpr DWORD kGlobalPointerRva = 0x0343C630u;
    constexpr DWORD wrapperOffsets[] = {0x650u, 0x658u, 0x6D0u, 0x6D8u};
    constexpr DWORD directObjectOffsets[] = {0x1E68u};
    constexpr DWORD stateReferenceOffsets[] = {0x538u, 0x548u, 0x5B8u, 0x5C8u};
    constexpr DWORD hostTailOffsets[] = {0x2BC8u, 0x2C08u, 0x2D08u};
    Log(L"=== BOOTSTRAP15 HOST + SPAWN BRIDGE SNAPSHOT BEGIN state=%s (read-only) ===", label);
    if (kGlobalPointerRva > imageSize || sizeof(std::uint64_t) > imageSize - kGlobalPointerRva) {
        Log(L"BOOT15_HOST_ABORT: state=%s reason=global_slot_outside_image", label);
        return;
    }
    std::uint64_t host = 0;
    if (!ReadMemory(image + kGlobalPointerRva, &host, sizeof(host)) ||
        !PlausiblePointer(image, imageSize, host)) {
        Log(L"BOOT15_HOST_ABORT: state=%s reason=invalid_host_pointer value=%016llX",
            label, static_cast<unsigned long long>(host));
        return;
    }
    const BYTE* root = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(host));
    Log(L"BOOT15_HOST: state=%s host=%p", label, root);
    wchar_t rootFile[128]{};
    swprintf_s(rootFile, L"%s_host_root_0x4000.bin", label);
    CaptureStableWindow(root, 0x4000, dir + L"\\" + rootFile, L"boot15_host_root");

    std::vector<std::uint64_t> capturedFunctions;
    for (DWORD offset : wrapperOffsets)
        CaptureHostWrapperDepth15(image, imageSize, nt, root, offset, label, dir, capturedFunctions);

    std::vector<std::uint64_t> directHostObjects;
    for (DWORD offset : directObjectOffsets) {
        std::uint64_t value = 0;
        if (!ReadMemory(root + offset, &value, sizeof(value))) continue;
        Log(L"BOOT15_HOST_DIRECT_OBJECT: state=%s offset=0x%X value=%016llX",
            label, offset, static_cast<unsigned long long>(value));
        wchar_t objectLabel[128]{};
        swprintf_s(objectLabel, L"%s_host_field_%04X", label, offset);
        CaptureObjectAndVtable(image, imageSize, nt, value, objectLabel, dir,
                               directHostObjects, capturedFunctions);
    }

    for (DWORD offset : stateReferenceOffsets) {
        std::uint64_t value = 0;
        if (!ReadMemory(root + offset, &value, sizeof(value))) continue;
        Log(L"BOOT15_HOST_STATE_REFERENCE: state=%s offset=0x%X value=%016llX",
            label, offset, static_cast<unsigned long long>(value));
        if (PlausiblePointer(image, imageSize, value)) {
            wchar_t file[128]{};
            swprintf_s(file, L"%s_host_state_ref_%04X.bin", label, offset);
            CaptureStableWindow(reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(value)),
                                0x200, dir + L"\\" + file, L"boot15_host_state_reference");
        }
    }

    for (DWORD offset : hostTailOffsets) {
        std::uint64_t value = 0;
        if (!ReadMemory(root + offset, &value, sizeof(value))) continue;
        Log(L"BOOT15_HOST_TAIL_FIELD: state=%s offset=0x%X address=%p qword=%016llX",
            label, offset, root + offset, static_cast<unsigned long long>(value));
        wchar_t file[128]{};
        swprintf_s(file, L"%s_host_tail_%04X.bin", label, offset);
        CaptureStableWindow(root + offset, 0x100, dir + L"\\" + file, L"boot15_host_tail_field");
        if (PlausiblePointer(image, imageSize, value)) {
            wchar_t targetFile[128]{};
            swprintf_s(targetFile, L"%s_host_tail_target_%04X.bin", label, offset);
            CaptureStableWindow(reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(value)),
                                0x200, dir + L"\\" + targetFile, L"boot15_host_tail_target");
        }
    }

    for (DWORD offset : {0x680u, 0x700u}) {
        std::uint64_t value = 0;
        if (ReadMemory(root + offset, &value, sizeof(value)))
            Log(L"BOOT15_HOST_SCALAR: state=%s offset=0x%X value=%016llX",
                label, offset, static_cast<unsigned long long>(value));
    }

    std::uint64_t tableValue = 0, tableMeta = 0, selectorValue = 0, selectorMeta = 0;
    if (!ReadMemory(root + 0x1DD8, &tableValue, sizeof(tableValue)) ||
        !ReadMemory(root + 0x1DE0, &tableMeta, sizeof(tableMeta)) ||
        !ReadMemory(root + 0x1DE8, &selectorValue, sizeof(selectorValue)) ||
        !ReadMemory(root + 0x1DF0, &selectorMeta, sizeof(selectorMeta))) {
        Log(L"BOOT15_TABLE_ABORT: state=%s reason=host_table_fields_unreadable", label);
        return;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(tableMeta & 0xFFFFFFFFu);
    Log(L"BOOT15_TABLE_FIELDS: state=%s table=%016llX table_meta=%016llX count=%u selector=%016llX selector_meta=%016llX",
        label, static_cast<unsigned long long>(tableValue),
        static_cast<unsigned long long>(tableMeta), count,
        static_cast<unsigned long long>(selectorValue),
        static_cast<unsigned long long>(selectorMeta));
    if (!PlausiblePointer(image, imageSize, tableValue) ||
        !PlausiblePointer(image, imageSize, selectorValue) || count < 0x24u || count > 128u) {
        Log(L"BOOT15_TABLE_ABORT: state=%s reason=invalid_table_selector_or_count", label);
        return;
    }

    const SIZE_T tableBytes = static_cast<SIZE_T>(count) * sizeof(std::uint64_t);
    std::vector<BYTE> tableSnapshot(tableBytes), tableVerify(tableBytes);
    std::vector<BYTE> selectorSnapshot(0x40), selectorVerify(0x40);
    bool stable = false;
    for (unsigned attempt = 1; attempt <= 4; ++attempt) {
        if (!ReadMemory(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(tableValue)),
                        tableSnapshot.data(), tableBytes) ||
            !ReadMemory(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(tableValue)),
                        tableVerify.data(), tableBytes) ||
            !ReadMemory(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(selectorValue)),
                        selectorSnapshot.data(), selectorSnapshot.size()) ||
            !ReadMemory(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(selectorValue)),
                        selectorVerify.data(), selectorVerify.size())) {
            Log(L"BOOT15_TABLE_READ_ERROR: state=%s attempt=%u error=%lu", label, attempt, GetLastError());
            continue;
        }
        if (memcmp(tableSnapshot.data(), tableVerify.data(), tableBytes) == 0 &&
            memcmp(selectorSnapshot.data(), selectorVerify.data(), selectorSnapshot.size()) == 0) {
            stable = true;
            Log(L"BOOT15_TABLE_STABLE: state=%s attempt=%u table_bytes=0x%llX selector_bytes=0x%llX",
                label, attempt, static_cast<unsigned long long>(tableBytes),
                static_cast<unsigned long long>(selectorSnapshot.size()));
            break;
        }
        Log(L"BOOT15_TABLE_CHANGED_DURING_SNAPSHOT: state=%s attempt=%u", label, attempt);
        Sleep(20);
    }
    if (!stable) {
        Log(L"BOOT15_TABLE_ABORT: state=%s reason=table_or_selector_not_stable", label);
        return;
    }

    wchar_t tableFile[128]{}, selectorFile[128]{};
    swprintf_s(tableFile, L"%s_component_table_%u_entries.bin", label, count);
    swprintf_s(selectorFile, L"%s_component_selector_0x40.bin", label);
    WriteFileBytes(dir + L"\\" + tableFile, tableSnapshot.data(), tableSnapshot.size());
    WriteFileBytes(dir + L"\\" + selectorFile, selectorSnapshot.data(), selectorSnapshot.size());

    constexpr unsigned logicalOffsets[] = {0x03u, 0x11u, 0x06u, 0x1Cu, 0x23u, 0x25u,
                                            0x1Au, 0x1Du, 0x22u, 0x20u, 0x08u};
    std::uint64_t logicalObjects[_countof(logicalOffsets)]{};
    unsigned logicalIndices[_countof(logicalOffsets)]{};
    for (unsigned i = 0; i < _countof(logicalOffsets); ++i) {
        logicalIndices[i] = selectorSnapshot[logicalOffsets[i]];
        if (logicalIndices[i] < count)
            memcpy(&logicalObjects[i], tableSnapshot.data() + static_cast<SIZE_T>(logicalIndices[i]) * 8, 8);
        Log(L"BOOT15_LOGICAL_COMPONENT: state=%s selector_offset=0x%02X table_index=0x%02X object=%016llX",
            label, logicalOffsets[i], logicalIndices[i],
            static_cast<unsigned long long>(logicalObjects[i]));
    }

    std::vector<std::uint64_t> capturedObjects;
    for (unsigned index = 0; index < count; ++index) {
        std::uint64_t component = 0;
        memcpy(&component, tableSnapshot.data() + static_cast<SIZE_T>(index) * 8, 8);
        if (!PlausiblePointer(image, imageSize, component)) continue;
        wchar_t objectLabel[128]{};
        swprintf_s(objectLabel, L"%s_component_%03u", label, index);
        CaptureObjectAndVtable(image, imageSize, nt, component, objectLabel, dir,
                               capturedObjects, capturedFunctions);
    }

    for (unsigned i = 0; i < _countof(logicalOffsets); ++i) {
        const std::uint64_t componentValue = logicalObjects[i];
        if (!PlausiblePointer(image, imageSize, componentValue)) continue;
        const BYTE* component = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(componentValue));
        SIZE_T requested = 0x400;
        if (logicalOffsets[i] == 0x03u || logicalOffsets[i] == 0x11u) requested = 0x1800;
        else if (logicalOffsets[i] == 0x06u || logicalOffsets[i] == 0x1Cu ||
                 logicalOffsets[i] == 0x23u) requested = 0x800;
        wchar_t logicalLabel[128]{};
        swprintf_s(logicalLabel, L"%s_logical_%02X", label, logicalOffsets[i]);
        wchar_t file[192]{};
        swprintf_s(file, L"%s_component_extended_0x%llX.bin",
                   logicalLabel, static_cast<unsigned long long>(requested));
        CaptureStableWindow(component, requested, dir + L"\\" + file, L"boot15_logical_component");

        if (logicalOffsets[i] == 0x03u) {
            CaptureInteriorReference15(image, imageSize, component, 0xF30, logicalLabel, dir);
            std::uint64_t descriptorValue = 0;
            if (ReadMemory(component + 0xDF8, &descriptorValue, sizeof(descriptorValue)) &&
                PlausiblePointer(image, imageSize, descriptorValue)) {
                const BYTE* descriptor = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(descriptorValue));
                wchar_t descriptorLabel[128]{};
                swprintf_s(descriptorLabel, L"%s_selector03_DF8_descriptor", label);
                swprintf_s(file, L"%s_wrapper.bin", descriptorLabel);
                CaptureStableWindow(descriptor, 0x240, dir + L"\\" + file, L"boot15_descriptor_wrapper");
                CaptureCodePointerFields15(image, imageSize, nt, descriptor, 0x240,
                                            descriptorLabel, dir, capturedFunctions);
                for (SIZE_T off : {static_cast<SIZE_T>(0x98), static_cast<SIZE_T>(0xA8)})
                    CaptureInteriorReference15(image, imageSize, descriptor, off, descriptorLabel, dir);
                for (SIZE_T off : {static_cast<SIZE_T>(0x18), static_cast<SIZE_T>(0x48), static_cast<SIZE_T>(0x78)}) {
                    std::uint64_t callback = 0;
                    if (!ReadMemory(descriptor + off, &callback, sizeof(callback))) continue;
                    const std::uint64_t base = reinterpret_cast<std::uint64_t>(image);
                    if (callback < base || callback - base >= imageSize) continue;
                    const DWORD rva = static_cast<DWORD>(callback - base);
                    wchar_t callbackFile[160]{};
                    swprintf_s(callbackFile, L"%s_callback_%03llX_rva_%08X.bin",
                               descriptorLabel, static_cast<unsigned long long>(off), rva);
                    CaptureStableWindow(image + rva, 0x100,
                                        dir + L"\\" + callbackFile, L"boot15_descriptor_callback");
                    Log(L"BOOT15_DESCRIPTOR_CALLBACK: state=%s offset=0x%llX rva=0x%08X",
                        label, static_cast<unsigned long long>(off), rva);
                }
            } else {
                Log(L"BOOT15_DESCRIPTOR_SKIP: state=%s selector03_component=%p field_DF8=%016llX",
                    label, component, static_cast<unsigned long long>(descriptorValue));
            }
        }
        if (logicalOffsets[i] == 0x11u)
            CaptureInteriorReference15(image, imageSize, component, 0x1258, logicalLabel, dir);
    }

    Log(L"BOOT15_CAPTURE_SUMMARY: state=%s table_entries=%u logical_components=%llu captured_objects=%llu code_functions=%llu",
        label, count, static_cast<unsigned long long>(_countof(logicalOffsets)),
        static_cast<unsigned long long>(capturedObjects.size()),
        static_cast<unsigned long long>(capturedFunctions.size()));
    Log(L"=== BOOTSTRAP15 HOST + SPAWN BRIDGE SNAPSHOT END state=%s ===", label);
}
void CaptureSlotVtableAndRtti(const BYTE* image, SIZE_T imageSize,
                              const BYTE* object, const wchar_t* label,
                              unsigned slot, const std::wstring& dir,
                              std::vector<std::uint64_t>& capturedFunctions) {
    std::uint64_t vtableValue = 0, owner10 = 0, owner18 = 0;
    if (!ReadMemory(object, &vtableValue, sizeof(vtableValue))) {
        Log(L"BOOT12_SLOT_ABORT: state=%s slot=0x%02X object=%p reason=object_read_failed error=%lu",
            label, slot, object, GetLastError());
        return;
    }
    ReadMemory(object + 0x10, &owner10, sizeof(owner10));
    ReadMemory(object + 0x18, &owner18, sizeof(owner18));
    Log(L"BOOT12_SLOT_OBJECT: state=%s slot=0x%02X object=%p vtable=%016llX field_10=%016llX field_18=%016llX",
        label, slot, object, static_cast<unsigned long long>(vtableValue),
        static_cast<unsigned long long>(owner10), static_cast<unsigned long long>(owner18));

    wchar_t objectFile[128]{};
    swprintf_s(objectFile, L"%s_slot_%02X_object.bin", label, slot);
    CaptureWindow(object, 0x100, dir + L"\\" + objectFile, L"boot12_component_object");

    const BYTE* vtable = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(vtableValue));
    constexpr SIZE_T kVtableEntries = 16;
    std::uint64_t functions[kVtableEntries]{};
    if (!vtableValue || !ReadMemory(vtable, functions, sizeof(functions))) {
        Log(L"BOOT12_VTABLE_ABORT: state=%s slot=0x%02X vtable=%p reason=read_failed error=%lu",
            label, slot, vtable, GetLastError());
        return;
    }

    wchar_t vtableFile[128]{};
    swprintf_s(vtableFile, L"%s_slot_%02X_vtable_16.bin", label, slot);
    CaptureWindow(vtable, sizeof(functions), dir + L"\\" + vtableFile, L"boot12_vtable");

    // On MSVC x64, the Complete Object Locator pointer is stored immediately
    // before the first virtual-function pointer. Keep the raw bytes and validate
    // the self-relative fields before interpreting them.
    std::uint64_t locatorValue = 0;
    if (vtableValue >= sizeof(std::uint64_t) &&
        ReadMemory(vtable - sizeof(std::uint64_t), &locatorValue, sizeof(locatorValue))) {
        const BYTE* locator = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(locatorValue));
        struct CompleteObjectLocator64 {
            std::uint32_t signature;
            std::uint32_t offset;
            std::uint32_t cdOffset;
            std::uint32_t typeDescriptorRva;
            std::uint32_t classDescriptorRva;
            std::uint32_t selfRva;
        } col{};
        if (locatorValue && ReadMemory(locator, &col, sizeof(col))) {
            const std::uint64_t imageBase = reinterpret_cast<std::uint64_t>(image);
            const std::uint64_t expectedSelf = imageBase + col.selfRva;
            const bool imageRelative = col.signature == 1;
            const bool selfMatches = imageRelative && expectedSelf == locatorValue;
            Log(L"BOOT12_RTTI_COL: state=%s slot=0x%02X locator=%p signature=%u offset=0x%X cdOffset=0x%X typeDescRva=0x%08X classDescRva=0x%08X selfRva=0x%08X self_matches=%u",
                label, slot, locator, col.signature, col.offset, col.cdOffset,
                col.typeDescriptorRva, col.classDescriptorRva, col.selfRva,
                selfMatches ? 1 : 0);
            wchar_t colFile[128]{};
            swprintf_s(colFile, L"%s_slot_%02X_rtti_col.bin", label, slot);
            CaptureWindow(locator, sizeof(col), dir + L"\\" + colFile, L"boot12_rtti_col");

            if (imageRelative && col.typeDescriptorRva < imageSize) {
                wchar_t typeFile[128]{};
                swprintf_s(typeFile, L"%s_slot_%02X_rtti_type_descriptor.bin", label, slot);
                CaptureWindow(image + col.typeDescriptorRva, 0x80,
                              dir + L"\\" + typeFile, L"boot12_rtti_type_descriptor");
            }
            if (imageRelative && col.classDescriptorRva < imageSize) {
                wchar_t classFile[128]{};
                swprintf_s(classFile, L"%s_slot_%02X_rtti_class_descriptor.bin", label, slot);
                CaptureWindow(image + col.classDescriptorRva, 0x40,
                              dir + L"\\" + classFile, L"boot12_rtti_class_descriptor");
            }
        } else {
            Log(L"BOOT12_RTTI_UNRESOLVED: state=%s slot=0x%02X locator=%016llX reason=unreadable_or_invalid",
                label, slot, static_cast<unsigned long long>(locatorValue));
        }
    } else {
        Log(L"BOOT12_RTTI_UNRESOLVED: state=%s slot=0x%02X reason=locator_pointer_unreadable",
            label, slot);
    }

    unsigned executableTargets = 0, duplicateTargets = 0;
    for (SIZE_T i = 0; i < kVtableEntries; ++i) {
        const std::uint64_t targetValue = functions[i];
        const BYTE* target = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(targetValue));
        if (!targetValue || !IsExecutableAddress(target)) continue;
        ++executableTargets;
        if (std::find(capturedFunctions.begin(), capturedFunctions.end(), targetValue) != capturedFunctions.end()) {
            ++duplicateTargets;
            Log(L"BOOT12_VFUNC_DUPLICATE: state=%s slot=0x%02X index=%llu address=%p",
                label, slot, static_cast<unsigned long long>(i), target);
            continue;
        }
        capturedFunctions.push_back(targetValue);
        wchar_t codeFile[160]{};
        swprintf_s(codeFile, L"%s_slot_%02X_vfunc_%02llu_%016llX.bin",
                   label, slot, static_cast<unsigned long long>(i),
                   static_cast<unsigned long long>(targetValue));
        const bool captured = CaptureWindow(target, 0x60, dir + L"\\" + codeFile, L"boot12_vfunc_code");
        std::uint64_t rva = 0;
        const std::uint64_t base = reinterpret_cast<std::uint64_t>(image);
        if (targetValue >= base && targetValue - base < imageSize) rva = targetValue - base;
        Log(L"BOOT12_VFUNC: state=%s slot=0x%02X index=%llu address=%p image_rva=0x%llX executable=1 captured=%u file=%s",
            label, slot, static_cast<unsigned long long>(i), target,
            static_cast<unsigned long long>(rva), captured ? 1 : 0, codeFile);
    }
    Log(L"BOOT12_VTABLE_SUMMARY: state=%s slot=0x%02X entries=%llu executable_targets=%u duplicate_targets=%u",
        label, slot, static_cast<unsigned long long>(kVtableEntries),
        executableTargets, duplicateTargets);
}

void CaptureComponentHostSnapshot(const BYTE* image, SIZE_T imageSize,
                                  const wchar_t* label, const std::wstring& dir,
                                  std::vector<std::uint64_t>& capturedFunctions) {
    constexpr DWORD kGlobalPointerRva = 0x0343C630u;
    constexpr unsigned kSlots[] = {0x1A, 0x1C};
    constexpr unsigned kRequiredEntries = 0x1D;
    Log(L"=== BOOTSTRAP12 TWO-STATE SLOT CAPTURE BEGIN state=%s (read-only) ===", label);

    if (kGlobalPointerRva > imageSize || sizeof(std::uint64_t) > imageSize - kGlobalPointerRva) {
        Log(L"BOOT12_ABORT: state=%s reason=global_slot_outside_image", label);
        return;
    }
    std::uint64_t host = 0;
    if (!ReadMemory(image + kGlobalPointerRva, &host, sizeof(host)) ||
        !PlausiblePointer(image, imageSize, host)) {
        Log(L"BOOT12_ABORT: state=%s reason=invalid_host_pointer value=%016llX",
            label, static_cast<unsigned long long>(host));
        return;
    }

    const BYTE* root = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(host));
    std::uint64_t table = 0, tableMeta = 0, selector = 0, selectorMeta = 0;
    if (!ReadMemory(root + 0x1DD8, &table, sizeof(table)) ||
        !ReadMemory(root + 0x1DE0, &tableMeta, sizeof(tableMeta))) {
        Log(L"BOOT12_ABORT: state=%s reason=component_table_fields_unreadable", label);
        return;
    }
    ReadMemory(root + 0x1DE8, &selector, sizeof(selector));
    ReadMemory(root + 0x1DF0, &selectorMeta, sizeof(selectorMeta));
    const std::uint32_t count = static_cast<std::uint32_t>(tableMeta & 0xFFFFFFFFu);
    if (!PlausiblePointer(image, imageSize, table) ||
        count < kRequiredEntries || count > 128) {
        Log(L"BOOT12_ABORT: state=%s reason=invalid_table_or_count table=%016llX count=%u tableMeta=%016llX",
            label, static_cast<unsigned long long>(table), count,
            static_cast<unsigned long long>(tableMeta));
        return;
    }

    Log(L"BOOT12_HOST: state=%s host=%p table=%p tableMeta=%016llX count=%u selector=%016llX selectorMeta=%016llX",
        label, root, reinterpret_cast<void*>(static_cast<std::uintptr_t>(table)),
        static_cast<unsigned long long>(tableMeta), count,
        static_cast<unsigned long long>(selector),
        static_cast<unsigned long long>(selectorMeta));
    wchar_t rootFile[128]{};
    swprintf_s(rootFile, L"%s_host_root.bin", label);
    CaptureWindow(root, 0x2000, dir + L"\\" + rootFile, L"boot12_host_root");

    const SIZE_T tableBytes = static_cast<SIZE_T>(count) * sizeof(std::uint64_t);
    std::vector<BYTE> stableSnapshot(tableBytes), verifySnapshot(tableBytes);
    bool stable = false;
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        if (!ReadMemory(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(table)),
                        stableSnapshot.data(), tableBytes) ||
            !ReadMemory(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(table)),
                        verifySnapshot.data(), tableBytes)) {
            Log(L"BOOT12_TABLE_READ_ERROR: state=%s attempt=%u error=%lu", label, attempt, GetLastError());
            continue;
        }
        if (memcmp(stableSnapshot.data(), verifySnapshot.data(), tableBytes) == 0) {
            stable = true;
            Log(L"BOOT12_TABLE_STABLE: state=%s attempt=%u bytes=0x%llX", label, attempt,
                static_cast<unsigned long long>(tableBytes));
            break;
        }
        Log(L"BOOT12_TABLE_CHANGED_DURING_READ: state=%s attempt=%u bytes=0x%llX",
            label, attempt, static_cast<unsigned long long>(tableBytes));
        Sleep(15);
    }
    if (!stable) {
        Log(L"BOOT12_ABORT: state=%s reason=component_table_did_not_stabilize", label);
        return;
    }

    wchar_t tableFile[128]{};
    swprintf_s(tableFile, L"%s_component_table_stable.bin", label);
    if (!WriteFileBytes(dir + L"\\" + tableFile, stableSnapshot.data(), stableSnapshot.size())) {
        Log(L"BOOT12_ABORT: state=%s reason=table_snapshot_write_failed", label);
        return;
    }

    // All subsequent slot lookups use this one immutable local copy. Never
    // reread individual live table entries after the snapshot is accepted.
    for (unsigned slot : kSlots) {
        std::uint64_t component = 0;
        memcpy(&component, stableSnapshot.data() + static_cast<SIZE_T>(slot) * 8, 8);
        if (!PlausiblePointer(image, imageSize, component)) {
            Log(L"BOOT12_SLOT_SKIP: state=%s slot=0x%02X value=%016llX reason=not_plausible_component_pointer",
                label, slot, static_cast<unsigned long long>(component));
            continue;
        }
        const BYTE* object = reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(component));
        Log(L"BOOT12_SLOT_SELECTED: state=%s slot=0x%02X object=%p table_snapshot=1",
            label, slot, object);
        CaptureSlotVtableAndRtti(image, imageSize, object, label, slot, dir, capturedFunctions);
    }
    Log(L"=== BOOTSTRAP12 TWO-STATE SLOT CAPTURE END state=%s ===", label);
}

std::wstring RunCapture() {
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) { Log(L"BOOT13_ABORT: main module unavailable"); return L""; }
    const BYTE* image = reinterpret_cast<const BYTE*>(module);
    IMAGE_DOS_HEADER dos{};
    if (!ReadMemory(image, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        Log(L"BOOT13_ABORT: invalid DOS header"); return L"";
    }
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadMemory(image + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        Log(L"BOOT13_ABORT: invalid PE64/AMD64 image"); return L"";
    }
    Log(L"BOOT13_IMAGE: base=%p size=0x%08X timestamp=0x%08X machine=0x%04X",
        module, nt.OptionalHeader.SizeOfImage, nt.FileHeader.TimeDateStamp, nt.FileHeader.Machine);
    if (nt.OptionalHeader.SizeOfImage != kImageSize || nt.FileHeader.TimeDateStamp != kImageTimestamp) {
        Log(L"BOOT13_ABORT: executable fingerprint mismatch expected_size=0x%08X expected_timestamp=0x%08X",
            kImageSize, kImageTimestamp);
        return L"";
    }
    std::wstring dir = NewCaptureDirectory();
    if (dir.empty()) return L"";
    Log(L"BOOT13_CAPTURE_DIRECTORY: %s", dir.c_str());
    CaptureNativeCode(image, nt.OptionalHeader.SizeOfImage, nt, dir);
    CaptureBootstrap15CodeTargets(image, nt.OptionalHeader.SizeOfImage, nt, dir);
    CaptureTransitionHostSnapshot(image, nt.OptionalHeader.SizeOfImage, nt,
                                  L"01_FREEDRIVE_VEHICLE", dir);
    return dir;}

DWORD WINAPI Worker(LPVOID) {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log(L"=== TC2 Walkmod diagnostic bootstrap 15 (Host pointer depth and spawn bridge) ===");
    Log(L"BOOTSTRAP_VERSION: 15");
    Log(L"PROBE_MODE: read-only; no hooks; no code patches; no game-memory writes");
    Log(L"BOOT15_SCOPE: Host wrapper->nested object->true vtable; logical components; spawn bridge references");
    Log(L"TEST_SCENARIOS: 01_FREEDRIVE_VEHICLE then 02_HQ_PEDESTRIAN only");
    Log(L"EXCLUDED: Leave action and open-world pedestrian scenario are not part of this capture");
    Log(L"Game executable: %s", exe);

    if (!WaitForHotkey(VK_F10, L"01_FREEDRIVE_VEHICLE")) return 0;
    std::wstring dir = RunCapture();
    if (dir.empty()) return 0;

    if (!WaitForHotkey(VK_F11, L"02_HQ_PEDESTRIAN")) {
        Log(L"BOOT15_SEQUENCE_INCOMPLETE: HQ snapshot was not triggered; directory=%s", dir.c_str());
        return 0;
    }
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) {
        Log(L"BOOT13_HQ_ABORT: main module unavailable");
        return 0;
    }
    IMAGE_DOS_HEADER hqDos{};
    IMAGE_NT_HEADERS64 hqNt{};
    const BYTE* hqImage = reinterpret_cast<const BYTE*>(module);
    if (!ReadMemory(hqImage, &hqDos, sizeof(hqDos)) ||
        hqDos.e_magic != IMAGE_DOS_SIGNATURE ||
        !ReadMemory(hqImage + hqDos.e_lfanew, &hqNt, sizeof(hqNt)) ||
        hqNt.Signature != IMAGE_NT_SIGNATURE ||
        hqNt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        hqNt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        Log(L"BOOT15_HQ_ABORT: invalid main module headers");
        return 0;
    }
    CaptureTransitionHostSnapshot(hqImage, hqNt.OptionalHeader.SizeOfImage, hqNt,
                                   L"02_HQ_PEDESTRIAN", dir);
    Log(L"BOOTSTRAP15_SEQUENCE_COMPLETE: directory=%s",
        dir.c_str());
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