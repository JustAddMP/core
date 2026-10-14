#pragma once
// Module ranges, IDA-style pattern scanning ("48 8B ?? ?? E8 ? ? ? ?") and RIP-relative resolving.
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace mc::core::mem {

struct Range {
    uintptr_t start = 0;
    size_t size = 0;
    uintptr_t End() const { return start + size; }
    bool Contains(uintptr_t addr) const { return addr >= start && addr < End(); }
};

struct Module {
    uintptr_t base = 0;
    size_t size = 0;      // SizeOfImage
    Range text;           // .text section (or whole image if not found)
    uint32_t timeDateStamp = 0;
};

Module GetModule(const wchar_t* name);  // nullptr -> main exe
const Module& MainModule();             // cached main exe info

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;  // 1 = must match, 0 = wildcard
    bool Valid() const { return !bytes.empty(); }
};

// Parses "48 8B ?? ?? 05" ("?" and "??" are wildcards). Returns an invalid pattern on syntax error.
Pattern ParsePattern(std::string_view text);

uintptr_t FindPattern(const Range& range, const Pattern& pattern);
uintptr_t FindPattern(const Range& range, std::string_view pattern);
uintptr_t FindPattern(std::string_view pattern);  // main module .text
std::vector<uintptr_t> FindAll(const Range& range, const Pattern& pattern, size_t maxResults = 64);

// Resolves a RIP-relative operand: target = addr + instrLen + *(int32*)(addr + dispOffset).
// e.g. "48 8B 05 xx xx xx xx" -> ResolveRip(addr, 3, 7); "E8 xx xx xx xx" -> ResolveRip(addr, 1, 5).
uintptr_t ResolveRip(uintptr_t addr, size_t dispOffset, size_t instrLen);

// True if [addr, addr+size) is committed and readable.
bool IsReadable(const void* addr, size_t size);

} // namespace mc::core::mem
