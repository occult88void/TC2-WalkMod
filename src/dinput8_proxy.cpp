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



struct MsvcCompleteObjectLocator {
    std::uint32_t signature, offset, cdOffset, typeDescriptorRva, classDescriptorRva, selfRva;
};

bool ImageRangeContains(const BYTE* image, SIZE_T imageSize, std::uint64_t address, SIZE_T size) {
    const std::uint64_t base=reinterpret_cast<std::uint64_t>(image);
    if(address<base||size>imageSize) return false;
    const std::uint64_t offset=address-base;
    return offset<=imageSize-size;
}

bool ReadBoundedTypeName(const BYTE* address, std::wstring& name) {
    name.clear();
    for(SIZE_T i=0;i<256;++i) {
        char ch=0;
        if(!ReadMemory(address+0x10+i,&ch,1)) return false;
        if(ch=='\0') return !name.empty();
        name.push_back(static_cast<unsigned char>(ch));
    }
    return false;
}

void CaptureSlotRtti(const BYTE* image,SIZE_T imageSize,const IMAGE_NT_HEADERS64& nt,
                     const std::wstring& dir,const wchar_t* label,std::uint32_t index,
                     std::uint64_t objectAddress,std::vector<std::uint64_t>& capturedFunctions) {
    const BYTE* object=reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(objectAddress));
    std::uint64_t vtable=0;
    if(!ReadMemory(object,&vtable,sizeof(vtable))) {
        Log(L"BOOT12_OBJECT_ABORT: label=%s index=0x%X object=%p reason=vtable_read_failed error=%lu",label,index,object,GetLastError()); return;
    }
    Log(L"BOOT12_SLOT_OBJECT: label=%s index=0x%X object=%p vtable=%016llX",label,index,object,static_cast<unsigned long long>(vtable));
    wchar_t stem[96]{}; swprintf_s(stem,L"%s_index_%02X",label,index);
    CaptureWindow(object,0x100,dir+L"\\"+stem+L"_object.bin",L"bootstrap12_component_object");

    constexpr SIZE_T n=16, bytes=n*sizeof(std::uint64_t);
    if(!ImageRangeContains(image,imageSize,vtable,bytes)) {
        Log(L"BOOT12_VTABLE_ABORT: label=%s index=0x%X reason=vtable_outside_main_image address=%016llX",label,index,static_cast<unsigned long long>(vtable)); return;
    }
    std::vector<std::uint64_t> methods(n);
    if(!ReadMemory(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(vtable)),methods.data(),bytes)) {
        Log(L"BOOT12_VTABLE_ABORT: label=%s index=0x%X reason=vtable_read_failed error=%lu",label,index,GetLastError()); return;
    }
    std::wstring vf=std::wstring(stem)+L"_vtable_16.bin";
    if(WriteFileBytes(dir+L"\\"+vf,reinterpret_cast<const BYTE*>(methods.data()),bytes))
        Log(L"BOOT12_VTABLE_OK: label=%s index=0x%X address=%016llX entries=16 file=%s",label,index,static_cast<unsigned long long>(vtable),vf.c_str());

    const std::uint64_t imageBase=reinterpret_cast<std::uint64_t>(image);
    if(vtable<imageBase+8) {
        Log(L"BOOT12_RTTI_SKIP: label=%s index=0x%X reason=no_preceding_locator_slot",label,index);
    } else {
        std::uint64_t locator=0;
        const BYTE* slot=reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(vtable-8));
        if(ReadMemory(slot,&locator,8)&&ImageRangeContains(image,imageSize,locator,sizeof(MsvcCompleteObjectLocator))) {
            MsvcCompleteObjectLocator col{};
            const BYTE* cp=reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(locator));
            if(ReadMemory(cp,&col,sizeof(col))) {
                const std::uint64_t self=locator-imageBase;
                const bool valid=col.signature==1&&col.selfRva==self&&
                    ImageRangeContains(image,imageSize,imageBase+col.typeDescriptorRva,0x11)&&
                    ImageRangeContains(image,imageSize,imageBase+col.classDescriptorRva,1);
                Log(L"BOOT12_RTTI_LOCATOR: label=%s index=0x%X address=%p signature=%u offset=0x%X cdOffset=0x%X typeRva=0x%08X hierarchyRva=0x%08X selfRva=0x%08X self_check=%u valid=%u",
                    label,index,cp,col.signature,col.offset,col.cdOffset,col.typeDescriptorRva,col.classDescriptorRva,col.selfRva,col.selfRva==self?1:0,valid?1:0);
                CaptureWindow(cp,sizeof(col),dir+L"\\"+stem+L"_rtti_col.bin",L"bootstrap12_rtti_col");
                if(valid) {
                    const BYTE* td=image+col.typeDescriptorRva;
                    const BYTE* chd=image+col.classDescriptorRva;
                    CaptureWindow(td,0x100,dir+L"\\"+stem+L"_rtti_type_descriptor.bin",L"bootstrap12_rtti_type_descriptor");
                    CaptureWindow(chd,0x40,dir+L"\\"+stem+L"_rtti_class_hierarchy.bin",L"bootstrap12_rtti_class_hierarchy");
                    std::wstring typeName;
                    if(ReadBoundedTypeName(td,typeName)) Log(L"BOOT12_RTTI_TYPE_NAME: label=%s index=0x%X name=%s",label,index,typeName.c_str());
                    else Log(L"BOOT12_RTTI_TYPE_NAME_UNAVAILABLE: label=%s index=0x%X",label,index);
                }
            } else Log(L"BOOT12_RTTI_SKIP: label=%s index=0x%X reason=locator_read_failed",label,index);
        } else Log(L"BOOT12_RTTI_SKIP: label=%s index=0x%X reason=locator_pointer_not_in_main_image value=%016llX",label,index,static_cast<unsigned long long>(locator));
    }

    unsigned valid=0;
    for(SIZE_T i=0;i<methods.size();++i) {
        const std::uint64_t address=methods[i];
        if(!ImageRangeContains(image,imageSize,address,1)) {
            Log(L"BOOT12_VFUNC_SKIP: label=%s index=0x%X vslot=%llu target=%016llX reason=outside_main_image",label,index,static_cast<unsigned long long>(i),static_cast<unsigned long long>(address)); continue;
        }
        const DWORD rva=static_cast<DWORD>(address-imageBase);
        if(!IsExecutableRange(image,nt,rva,1)) {
            Log(L"BOOT12_VFUNC_SKIP: label=%s index=0x%X vslot=%llu target=%016llX reason=not_executable",label,index,static_cast<unsigned long long>(i),static_cast<unsigned long long>(address)); continue;
        }
        ++valid;
        Log(L"BOOT12_VFUNC: label=%s index=0x%X vslot=%llu rva=0x%08X address=%016llX",label,index,static_cast<unsigned long long>(i),rva,static_cast<unsigned long long>(address));
        if(std::find(capturedFunctions.begin(),capturedFunctions.end(),address)!=capturedFunctions.end()) continue;
        capturedFunctions.push_back(address);
        wchar_t fn[128]{}; swprintf_s(fn,L"%s_index_%02X_vfunc_%02llu_rva_%08X.bin",label,index,static_cast<unsigned long long>(i),rva);
        CaptureWindow(reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(address)),0x80,dir+L"\\"+fn,L"bootstrap12_virtual_function");
    }
    Log(L"BOOT12_VFUNC_SUMMARY: label=%s index=0x%X inspected=16 executable_in_image=%u",label,index,valid);
}

void CaptureComponentHostSnapshot(const BYTE* image,SIZE_T imageSize,const IMAGE_NT_HEADERS64& nt,
                                  const wchar_t* label,const std::wstring& dir) {
    constexpr DWORD globalRva=0x0343C630u;
    constexpr std::uint32_t indices[]={0x1A,0x1C};
    Log(L"=== BOOTSTRAP12 COMPONENT SLOT SNAPSHOT BEGIN label=%s (read-only) ===",label);
    if(globalRva>imageSize||8>imageSize-globalRva) { Log(L"BOOT12_HOST_ABORT: label=%s reason=global_slot_outside_image",label); return; }
    std::uint64_t host=0;
    if(!ReadMemory(image+globalRva,&host,8)||!PlausiblePointer(image,imageSize,host)) {
        Log(L"BOOT12_HOST_ABORT: label=%s reason=invalid_global_pointer value=%016llX",label,static_cast<unsigned long long>(host)); return;
    }
    const BYTE* root=reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(host));
    std::uint64_t table=0,meta=0;
    if(!ReadMemory(root+0x1DD8,&table,8)||!ReadMemory(root+0x1DE0,&meta,8)) {
        Log(L"BOOT12_HOST_ABORT: label=%s reason=table_fields_unreadable",label); return;
    }
    const std::uint32_t count=static_cast<std::uint32_t>(meta&0xFFFFFFFFu);
    Log(L"BOOT12_HOST: label=%s host=%p table=%016llX tableMeta=%016llX count=%u",label,root,static_cast<unsigned long long>(table),static_cast<unsigned long long>(meta),count);
    if(count==0||count>128||!PlausiblePointer(image,imageSize,table)) {
        Log(L"BOOT12_TABLE_ABORT: label=%s reason=invalid_count_or_pointer count=%u table=%016llX",label,count,static_cast<unsigned long long>(table)); return;
    }
    const SIZE_T tableBytes=static_cast<SIZE_T>(count)*8;
    const BYTE* tableAddress=reinterpret_cast<const BYTE*>(static_cast<std::uintptr_t>(table));
    MEMORY_BASIC_INFORMATION mbi{};
    if(VirtualQuery(tableAddress,&mbi,sizeof(mbi))!=sizeof(mbi)||mbi.State!=MEM_COMMIT||!Readable(mbi.Protect)) {
        Log(L"BOOT12_TABLE_ABORT: label=%s reason=table_not_readable",label); return;
    }
    const BYTE* regionEnd=static_cast<const BYTE*>(mbi.BaseAddress)+mbi.RegionSize;
    if(regionEnd<tableAddress||static_cast<SIZE_T>(regionEnd-tableAddress)<tableBytes) {
        Log(L"BOOT12_TABLE_ABORT: label=%s reason=table_exceeds_memory_region bytes=0x%llX",label,static_cast<unsigned long long>(tableBytes)); return;
    }
    // First read is authoritative. The second read only detects mutation; all lookups use snapshot.
    std::vector<std::uint64_t> snapshot(count),verify(count);
    if(!ReadMemory(tableAddress,snapshot.data(),tableBytes)) { Log(L"BOOT12_TABLE_ABORT: label=%s reason=snapshot_read_failed error=%lu",label,GetLastError()); return; }
    const bool verifyRead=ReadMemory(tableAddress,verify.data(),tableBytes);
    const bool stable=verifyRead&&memcmp(snapshot.data(),verify.data(),tableBytes)==0;
    Log(L"BOOT12_TABLE_STABILITY: label=%s count=%u second_read=%u identical=%u policy=use_first_snapshot",label,count,verifyRead?1:0,stable?1:0);
    wchar_t tableFile[96]{}; swprintf_s(tableFile,L"host_%s_component_table_snapshot.bin",label);
    if(WriteFileBytes(dir+L"\\"+tableFile,reinterpret_cast<const BYTE*>(snapshot.data()),tableBytes))
        Log(L"BOOT12_TABLE_SNAPSHOT_OK: label=%s entries=%u bytes=0x%llX file=%s",label,count,static_cast<unsigned long long>(tableBytes),tableFile);
    else Log(L"BOOT12_TABLE_WRITE_ERROR: label=%s",label);

    std::vector<std::uint64_t> capturedFunctions;
    for(std::uint32_t index:indices) {
        if(index>=snapshot.size()) { Log(L"BOOT12_SLOT_MISSING: label=%s index=0x%X count=%u",label,index,count); continue; }
        const std::uint64_t component=snapshot[index];
        if(!PlausiblePointer(image,imageSize,component)) {
            Log(L"BOOT12_SLOT_INVALID: label=%s index=0x%X pointer=%016llX",label,index,static_cast<unsigned long long>(component)); continue;
        }
        CaptureSlotRtti(image,imageSize,nt,dir,label,index,component,capturedFunctions);
    }
    Log(L"=== BOOTSTRAP12 COMPONENT SLOT SNAPSHOT END label=%s ===",label);
}

bool CreateBootstrap12CaptureSession(std::wstring& dir,IMAGE_NT_HEADERS64& ntOut,
                                     const BYTE*& imageOut,SIZE_T& imageSizeOut) {
    HMODULE module=GetModuleHandleW(nullptr);
    if(!module) { Log(L"BOOT12_ABORT: main_module_unavailable"); return false; }
    const BYTE* image=reinterpret_cast<const BYTE*>(module);
    IMAGE_DOS_HEADER dos{};
    if(!ReadMemory(image,&dos,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<=0||static_cast<SIZE_T>(dos.e_lfanew)>0x100000) {
        Log(L"BOOT12_ABORT: invalid_dos_header"); return false;
    }
    IMAGE_NT_HEADERS64 nt{};
    if(!ReadMemory(image+dos.e_lfanew,&nt,sizeof(nt))||nt.Signature!=IMAGE_NT_SIGNATURE||
       nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64) {
        Log(L"BOOT12_ABORT: invalid_pe64_amd64_image"); return false;
    }
    Log(L"BOOT12_IMAGE: base=%p size=0x%08X timestamp=0x%08X machine=0x%04X",module,nt.OptionalHeader.SizeOfImage,nt.FileHeader.TimeDateStamp,nt.FileHeader.Machine);
    if(nt.OptionalHeader.SizeOfImage!=kImageSize||nt.FileHeader.TimeDateStamp!=kImageTimestamp) {
        Log(L"BOOT12_ABORT: executable_fingerprint_mismatch expected_size=0x%08X expected_timestamp=0x%08X",kImageSize,kImageTimestamp); return false;
    }
    dir=NewCaptureDirectory();
    if(dir.empty()) return false;
    ntOut=nt; imageOut=image; imageSizeOut=nt.OptionalHeader.SizeOfImage;
    Log(L"BOOT12_CAPTURE_DIRECTORY: %s",dir.c_str());
    return true;
}

std::wstring RunCapture() {
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) { Log(L"TARGET_CAPTURE_ABORT: main module unavailable"); return L""; }
    const BYTE* image = reinterpret_cast<const BYTE*>(module);
    IMAGE_DOS_HEADER dos{};
    if (!ReadMemory(image, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        Log(L"TARGET_CAPTURE_ABORT: invalid DOS header"); return L"";
    }
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadMemory(image + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        Log(L"TARGET_CAPTURE_ABORT: invalid PE64/AMD64 image"); return L"";
    }
    Log(L"TARGET_CAPTURE_IMAGE: base=%p size=0x%08X timestamp=0x%08X machine=0x%04X",
        module, nt.OptionalHeader.SizeOfImage, nt.FileHeader.TimeDateStamp, nt.FileHeader.Machine);
    if (nt.OptionalHeader.SizeOfImage != kImageSize || nt.FileHeader.TimeDateStamp != kImageTimestamp) {
        Log(L"TARGET_CAPTURE_ABORT: executable fingerprint mismatch expected_size=0x%08X expected_timestamp=0x%08X",
            kImageSize, kImageTimestamp);
        return L"";
    }

    std::wstring dir = NewCaptureDirectory();
    if (dir.empty()) return L"";
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
    CaptureComponentHostSnapshot(image, nt.OptionalHeader.SizeOfImage,
                                 L"01_FREEDRIVE_VEHICLE", dir);
    Log(L"TARGET_CAPTURE_COMPLETE: directory=%s", dir.c_str());
    return dir;
}

DWORD WINAPI Worker(LPVOID) {
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr,exe,MAX_PATH);
    Log(L"=== TC2 Walkmod diagnostic bootstrap 12 (narrow slot/RTTI capture) ===");
    Log(L"BOOTSTRAP_VERSION: 12");
    Log(L"PROBE_MODE: read-only; no hooks; no code patches; no writes to game memory");
    Log(L"TABLE_POLICY: first table read is authoritative; second read is stability check only");
    Log(L"TARGETS: component table indices 0x1A and 0x1C only");
    Log(L"Game executable: %s",exe);
    std::wstring dir; IMAGE_NT_HEADERS64 nt{}; const BYTE* image=nullptr; SIZE_T imageSize=0;
    if(!CreateBootstrap12CaptureSession(dir,nt,image,imageSize)) return 0;
    if(WaitForHotkey(VK_F10,L"01_FREEDRIVE_VEHICLE"))
        CaptureComponentHostSnapshot(image,imageSize,nt,L"01_FREEDRIVE_VEHICLE",dir);
    if(WaitForHotkey(VK_F11,L"02_HQ_PEDESTRIAN"))
        CaptureComponentHostSnapshot(image,imageSize,nt,L"02_HQ_PEDESTRIAN",dir);
    if(WaitForHotkey(VK_F12,L"03_OPEN_WORLD_PEDESTRIAN"))
        CaptureComponentHostSnapshot(image,imageSize,nt,L"03_OPEN_WORLD_PEDESTRIAN",dir);
    Log(L"BOOTSTRAP12_SEQUENCE_COMPLETE: directory=%s",dir.c_str());
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
