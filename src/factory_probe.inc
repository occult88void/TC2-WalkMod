// Included inside the existing proxy namespace to reuse its bounded readers.
// Matches are evidence for offline analysis, never native entry points to call.
struct FactoryHashTarget {
    const wchar_t* name;
    std::uint32_t hash;
};

constexpr FactoryHashTarget kFactoryHashTargets[] = {
    {L"AiBotFreerideSpawner", 0x0F3F48DDu},
    {L"AIPlayerBotFreeride", 0x51E17990u},
    {L"AvatarTC2", 0x2155AD67u},
    {L"MissionAtomSpawn", 0x430810CAu}
};

void CaptureFactoryReference(const BYTE* image, SIZE_T imageSize,
                             const IMAGE_NT_HEADERS64& nt, DWORD rva,
                             const FactoryHashTarget& target, unsigned ordinal,
                             const std::wstring& dir) {
    DWORD64 unwindBase = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(
        reinterpret_cast<DWORD64>(image + rva), &unwindBase, nullptr);
    RUNTIME_FUNCTION entry{};
    DWORD start = rva > 0x80u ? rva - 0x80u : 0u;
    SIZE_T size = (std::min)(static_cast<SIZE_T>(0x180), imageSize - start);
    bool hasUnwind = false;
    if (function && unwindBase == reinterpret_cast<DWORD64>(image) &&
        ReadMemory(function, &entry, sizeof(entry)) &&
        entry.BeginAddress <= rva && rva < entry.EndAddress &&
        entry.EndAddress <= imageSize) {
        start = entry.BeginAddress;
        size = (std::min)(static_cast<SIZE_T>(entry.EndAddress - start),
                          static_cast<SIZE_T>(0x4000));
        // Keep the actual reference in the capture when an unusually large
        // unwind range would otherwise put it outside the bounded prefix.
        if (rva - start >= size) {
            start = rva > 0x80u ? rva - 0x80u : 0u;
            size = (std::min)(static_cast<SIZE_T>(0x180), imageSize - start);
        }
        hasUnwind = true;
    }
    wchar_t label[128]{};
    swprintf_s(label, L"factory_%s_%02u", target.name, ordinal);
    const bool captured = CaptureCodeRva15(image, imageSize, nt, start, size, label, dir);
    Log(L"FACTORY_HASH_REFERENCE: name=%s hash=0x%08X rva=0x%08X begin=0x%08X end=0x%08X unwind=%u capture_start=0x%08X capture_size=0x%llX captured=%u",
        target.name, target.hash, rva, entry.BeginAddress, entry.EndAddress,
        hasUnwind ? 1u : 0u, start, static_cast<unsigned long long>(size),
        captured ? 1u : 0u);
}

void CaptureFactoryReferences(const BYTE* image, SIZE_T imageSize,
                              const IMAGE_NT_HEADERS64& nt, const std::wstring& dir) {
    constexpr SIZE_T chunkSize = 0x10000;
    constexpr unsigned maxCaptures = 16;
    unsigned counts[_countof(kFactoryHashTargets)]{};
    SIZE_T scanned = 0;
    SIZE_T skipped = 0;
    IMAGE_DOS_HEADER dos{};
    if (!ReadMemory(image, &dos, sizeof(dos)) || dos.e_lfanew < 0) return;
    const auto tableOffset = static_cast<std::uint64_t>(dos.e_lfanew) +
        offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    const auto tableBytes = static_cast<std::uint64_t>(nt.FileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (tableOffset > imageSize || tableBytes > imageSize - tableOffset) return;
    const BYTE* sectionTable = image + tableOffset;
    Log(L"FACTORY_SCAN_BEGIN: executable_sections_only targets=4 max_captures_per_target=16");
    for (WORD sectionIndex = 0; sectionIndex < nt.FileHeader.NumberOfSections; ++sectionIndex) {
        IMAGE_SECTION_HEADER section{};
        if (!ReadMemory(sectionTable + sectionIndex * sizeof(section), &section, sizeof(section))) {
            Log(L"FACTORY_SCAN_ABORT: unreadable_section_header index=%u", sectionIndex);
            return;
        }
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const SIZE_T span = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
        if (section.VirtualAddress >= imageSize || span > imageSize - section.VirtualAddress) {
            Log(L"FACTORY_SCAN_ABORT: invalid_section_range index=%u", sectionIndex);
            return;
        }
        SIZE_T offset = 0;
        std::vector<BYTE> carry;
        while (offset < span) {
            const BYTE* address = image + section.VirtualAddress + offset;
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region)) {
                const SIZE_T step = (std::min)(static_cast<SIZE_T>(0x1000), span - offset);
                offset += step;
                skipped += step;
                carry.clear();
                continue;
            }
            const auto addressValue = reinterpret_cast<std::uintptr_t>(address);
            const auto regionStart = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
            if (regionStart > addressValue || addressValue - regionStart >= region.RegionSize) {
                Log(L"FACTORY_SCAN_ABORT: invalid_virtual_region");
                return;
            }
            const SIZE_T available = (std::min)(span - offset,
                region.RegionSize - (addressValue - regionStart));
            const SIZE_T amount = (std::min)(chunkSize, available);
            if (region.State != MEM_COMMIT || !Readable(region.Protect)) {
                offset += available;
                skipped += available;
                carry.clear();
                continue;
            }
            std::vector<BYTE> block(carry.size() + amount);
            std::copy(carry.begin(), carry.end(), block.begin());
            if (!ReadMemory(address, block.data() + carry.size(), amount)) {
                offset += amount;
                skipped += amount;
                carry.clear();
                continue;
            }
            const SIZE_T blockStart = section.VirtualAddress + offset - carry.size();
            for (SIZE_T i = 0; i + sizeof(std::uint32_t) <= block.size(); ++i) {
                std::uint32_t value = 0;
                memcpy(&value, block.data() + i, sizeof(value));
                for (SIZE_T targetIndex = 0; targetIndex < _countof(kFactoryHashTargets); ++targetIndex) {
                    const auto& target = kFactoryHashTargets[targetIndex];
                    if (value != target.hash) continue;
                    ++counts[targetIndex];
                    if (counts[targetIndex] <= maxCaptures) {
                        CaptureFactoryReference(image, imageSize, nt,
                            static_cast<DWORD>(blockStart + i), target,
                            counts[targetIndex], dir);
                    }
                }
            }
            const SIZE_T tailSize = (std::min)(static_cast<SIZE_T>(3), block.size());
            carry.assign(block.end() - tailSize, block.end());
            scanned += amount;
            offset += amount;
        }
    }
    for (SIZE_T i = 0; i < _countof(kFactoryHashTargets); ++i) {
        Log(L"FACTORY_SCAN_TARGET: name=%s hash=0x%08X matches=%u capture_attempts=%u",
            kFactoryHashTargets[i].name, kFactoryHashTargets[i].hash, counts[i],
            (std::min)(counts[i], maxCaptures));
    }
    Log(L"FACTORY_SCAN_END: scanned_bytes=0x%llX skipped_bytes=0x%llX",
        static_cast<unsigned long long>(scanned), static_cast<unsigned long long>(skipped));
}
