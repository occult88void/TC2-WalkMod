#include "lifecycle_probe.h"

#include <windows.h>
#include <bcrypt.h>
#include <MinHook.h>
#include <intrin.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {
constexpr DWORD kImageSize = 0x05F94000u;
constexpr DWORD kImageTimestamp = 0x68D1CFB8u;
constexpr DWORD kDestroyRva = 0x001FA430u;
constexpr DWORD kSpawnRva = 0x001FA530u;
constexpr DWORD kCleanupRva = 0x00841780u;
constexpr char kExpectedSha256[] = "456025edebca9bf2eb345e31819540fbbc7dd50136e82c31827a49fedf8aba6e";

using DestroyFn = void(__fastcall*)(void*);
using SpawnFn = void(__fastcall*)(void*);
using CleanupFn = void(__fastcall*)(void*, std::uint32_t);
DestroyFn g_destroyOriginal = nullptr;
SpawnFn g_spawnOriginal = nullptr;
CleanupFn g_cleanupOriginal = nullptr;
SRWLOCK g_logLock = SRWLOCK_INIT;
std::wstring g_logPath;

bool ReadSafe(const void* p, void* out, SIZE_T n) {
    if (!p || !out || !n) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}

void ProbeLog(const wchar_t* fmt, ...) {
    wchar_t line[2048]{};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(line, _countof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return;
    std::vector<char> utf8(static_cast<size_t>(bytes));
    if (!WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytes, nullptr, nullptr)) return;
    AcquireSRWLockExclusive(&g_logLock);
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_logPath.c_str(), L"ab") == 0 && f) {
        fwrite(utf8.data(), 1, static_cast<size_t>(bytes - 1), f);
        fwrite("\r\n", 1, 2, f);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

bool HashExecutable(std::wstring& path, std::string& hex) {
    wchar_t filePath[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, filePath, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    path.assign(filePath, n);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, cb = 0, hashSize = 0;
    std::vector<BYTE> object, digest;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &cb, 0) >= 0;
    if (ok) ok = BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &cb, 0) >= 0;
    if (ok) { object.resize(objectSize); digest.resize(hashSize); ok = BCryptCreateHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0; }
    BYTE buffer[1 << 16];
    while (ok) {
        DWORD got = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &got, nullptr)) { ok = false; break; }
        if (!got) break;
        if (BCryptHashData(hash, buffer, got, 0) < 0) { ok = false; break; }
    }
    if (ok) ok = BCryptFinishHash(hash, digest.data(), hashSize, 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    if (!ok) return false;
    static const char digits[] = "0123456789abcdef";
    hex.clear();
    for (BYTE b : digest) { hex.push_back(digits[b >> 4]); hex.push_back(digits[b & 15]); }
    return true;
}

void LogFields(const wchar_t* tag, void* self, const DWORD* offsets, size_t count, void* caller) {
    wchar_t fields[1200]{};
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        std::uint64_t value = 0;
        bool ok = self && ReadSafe(static_cast<BYTE*>(self) + offsets[i], &value, sizeof(value));
        int wrote = _snwprintf_s(fields + used, _countof(fields) - used, _TRUNCATE,
                                 L" +%03X=%s%016llX", offsets[i], ok ? L"" : L"?", 
                                 static_cast<unsigned long long>(value));
        if (wrote > 0) used += static_cast<size_t>(wrote);
        if (used >= _countof(fields) - 64) break;
    }
    ProbeLog(L"PROBE %s tid=%lu this=%p ret=%p%s", tag, GetCurrentThreadId(), self,
             caller, fields);
}

void __fastcall HookDestroy(void* self) {
    const DWORD offsets[] = {0x18, 0x28, 0x54, 0x568, 0x570};
    void* caller = _ReturnAddress();\n    LogFields(L"DestroyCurrentVehicleAvatarMode ENTER", self, offsets, _countof(offsets), caller);
    g_destroyOriginal(self);
    LogFields(L"DestroyCurrentVehicleAvatarMode EXIT ", self, offsets, _countof(offsets), caller);
}

void __fastcall HookSpawn(void* self) {
    const DWORD offsets[] = {0x18, 0x28, 0x568, 0x570, 0x698, 0x6A8};
    void* caller = _ReturnAddress();\n    LogFields(L"SpawnReplicaVehicle ENTER", self, offsets, _countof(offsets), caller);
    g_spawnOriginal(self);
    LogFields(L"SpawnReplicaVehicle EXIT ", self, offsets, _countof(offsets), caller);
}

void __fastcall HookCleanup(void* self, std::uint32_t flags) {
    const DWORD offsets[] = {0x568, 0x570};
    void* caller = _ReturnAddress();\n    LogFields(L"VehicleCleanupInner ENTER", self, offsets, _countof(offsets), caller);
    ProbeLog(L"PROBE VehicleCleanupInner args tid=%lu flags=%08X", GetCurrentThreadId(), flags);
    g_cleanupOriginal(self, flags);
    LogFields(L"VehicleCleanupInner EXIT ", self, offsets, _countof(offsets), caller);
}

bool CheckPrologue(const BYTE* address, const BYTE* expected, size_t size) {
    BYTE actual[16]{};
    return ReadSafe(address, actual, size) && memcmp(actual, expected, size) == 0;
}
} // namespace

void InstallLifecycleProbe() {
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return;
    BYTE* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage != kImageSize ||
        nt->FileHeader.TimeDateStamp != kImageTimestamp || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        ProbeLog(L"PROBE_ABORT executable PE fingerprint mismatch");
        return;
    }
    std::wstring exePath;
    std::string actualHash;
    if (!HashExecutable(exePath, actualHash) || actualHash != kExpectedSha256) {
        ProbeLog(L"PROBE_ABORT SHA256 mismatch path=%s actual=%S expected=%S",
                 exePath.c_str(), actualHash.c_str(), kExpectedSha256);
        return;
    }
    g_logPath = exePath.substr(0, exePath.find_last_of(L"\\/") + 1) + L"TC2_Walkmod.log";
    ProbeLog(L"=== BOOTSTRAP 10 PASSIVE LIFECYCLE PROBE ===");
    ProbeLog(L"PROBE_FINGERPRINT_OK path=%s sha256=%S base=%p", exePath.c_str(), actualHash.c_str(), base);

    const BYTE destroySig[] = {0x40,0x57,0x48,0x83,0xEC,0x50};
    const BYTE spawnSig[] = {0x40,0x55,0x57,0x41,0x56,0x48,0x8D,0xAC};
    const BYTE cleanupSig[] = {0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18};
    if (!CheckPrologue(base + kDestroyRva, destroySig, sizeof(destroySig)) ||
        !CheckPrologue(base + kSpawnRva, spawnSig, sizeof(spawnSig)) ||
        !CheckPrologue(base + kCleanupRva, cleanupSig, sizeof(cleanupSig))) {
        ProbeLog(L"PROBE_ABORT code signature mismatch; no hooks installed");
        return;
    }
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        ProbeLog(L"PROBE_ABORT MinHook initialize status=%d", status);
        return;
    }
    status = MH_CreateHook(base + kDestroyRva, &HookDestroy, reinterpret_cast<LPVOID*>(&g_destroyOriginal));
    if (status != MH_OK) { ProbeLog(L"PROBE_HOOK_FAIL target=Destroy status=%d", status); return; }
    status = MH_CreateHook(base + kSpawnRva, &HookSpawn, reinterpret_cast<LPVOID*>(&g_spawnOriginal));
    if (status != MH_OK) { ProbeLog(L"PROBE_HOOK_FAIL target=Spawn status=%d", status); MH_RemoveHook(base + kDestroyRva); return; }
    status = MH_CreateHook(base + kCleanupRva, &HookCleanup, reinterpret_cast<LPVOID*>(&g_cleanupOriginal));
    if (status != MH_OK) {
        ProbeLog(L"PROBE_HOOK_FAIL target=Cleanup status=%d", status);
        MH_RemoveHook(base + kSpawnRva); MH_RemoveHook(base + kDestroyRva); return;
    }
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        ProbeLog(L"PROBE_ABORT hook enable status=%d", status);
        MH_RemoveHook(base + kCleanupRva); MH_RemoveHook(base + kSpawnRva); MH_RemoveHook(base + kDestroyRva);
        return;
    }
    ProbeLog(L"PROBE_HOOKS_ACTIVE destroy=%p spawn=%p cleanup=%p",
             base + kDestroyRva, base + kSpawnRva, base + kCleanupRva);
}
