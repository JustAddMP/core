#include "core/mem.h"

#include <windows.h>
#include <cstring>

namespace mc::core::mem {

Module GetModule(const wchar_t* name) {
    Module m;
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(name));
    if (!base) return m;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    m.base = base;
    m.size = nt->OptionalHeader.SizeOfImage;
    m.timeDateStamp = nt->FileHeader.TimeDateStamp;
    m.text = {base, m.size};
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text", 5) == 0) {
            m.text = {base + sec->VirtualAddress, sec->Misc.VirtualSize};
            break;
        }
    }
    return m;
}

const Module& MainModule() {
    static Module m = GetModule(nullptr);
    return m;
}

namespace {

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

Pattern ParsePattern(std::string_view text) {
    Pattern p;
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if (c == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(0);
            i += (i + 1 < text.size() && text[i + 1] == '?') ? 2 : 1;
            continue;
        }
        if (i + 1 >= text.size()) return {};
        int hi = HexValue(c), lo = HexValue(text[i + 1]);
        if (hi < 0 || lo < 0) return {};
        p.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
        p.mask.push_back(1);
        i += 2;
    }
    return p;
}

std::vector<uintptr_t> FindAll(const Range& range, const Pattern& pattern, size_t maxResults) {
    std::vector<uintptr_t> out;
    const size_t n = pattern.bytes.size();
    if (!n || range.size < n) return out;
    // Anchor on the first fixed byte for a fast memchr-style skip.
    size_t anchor = 0;
    while (anchor < n && !pattern.mask[anchor]) ++anchor;
    const auto* data = reinterpret_cast<const uint8_t*>(range.start);
    const size_t last = range.size - n;
    for (size_t i = 0; i <= last; ++i) {
        if (anchor < n) {
            const void* hit = std::memchr(data + i + anchor, pattern.bytes[anchor], last - i + 1);
            if (!hit) break;
            i = static_cast<const uint8_t*>(hit) - data - anchor;
        }
        bool ok = true;
        for (size_t j = 0; j < n; ++j) {
            if (pattern.mask[j] && data[i + j] != pattern.bytes[j]) {
                ok = false;
                break;
            }
        }
        if (ok) {
            out.push_back(range.start + i);
            if (out.size() >= maxResults) break;
        }
    }
    return out;
}

uintptr_t FindPattern(const Range& range, const Pattern& pattern) {
    auto all = FindAll(range, pattern, 1);
    return all.empty() ? 0 : all.front();
}

uintptr_t FindPattern(const Range& range, std::string_view pattern) {
    return FindPattern(range, ParsePattern(pattern));
}

uintptr_t FindPattern(std::string_view pattern) {
    return FindPattern(MainModule().text, pattern);
}

uintptr_t ResolveRip(uintptr_t addr, size_t dispOffset, size_t instrLen) {
    if (!addr) return 0;
    int32_t disp = 0;
    std::memcpy(&disp, reinterpret_cast<const void*>(addr + dispOffset), sizeof(disp));
    return addr + instrLen + static_cast<intptr_t>(disp);
}

bool IsReadable(const void* addr, size_t size) {
    auto p = reinterpret_cast<uintptr_t>(addr);
    const uintptr_t end = p + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        p = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

} // namespace mc::core::mem
