#pragma once
// Test helper: a minimal inflate (RFC 1951: stored, fixed and dynamic Huffman blocks) and a zip reader (central
// directory, local headers, CRCs) to check what core/zip_writer.h wrote without zlib. Test code only.
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "core/zip_writer.h"

namespace mc::testzip {

struct Inflate {
    const uint8_t* in;
    size_t n, pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;
    bool bad = false;
    std::vector<uint8_t> out;

    int Bits(int need) {
        uint32_t val = bitbuf;
        while (bitcnt < need) {
            if (pos >= n) {
                bad = true;
                return 0;
            }
            val |= static_cast<uint32_t>(in[pos++]) << bitcnt;
            bitcnt += 8;
        }
        bitbuf = val >> need;
        bitcnt -= need;
        return static_cast<int>(val & ((1u << need) - 1));
    }
    struct Huff {
        short count[16] = {};
        short symbol[320] = {};
    };
    // returns 0 ok, <0 over-subscribed
    static int Build(Huff& h, const short* length, int n) {
        for (int i = 0; i < 16; ++i) h.count[i] = 0;
        for (int s = 0; s < n; ++s) h.count[length[s]]++;
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return -1;
        }
        short offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
        for (int s = 0; s < n; ++s)
            if (length[s] != 0) h.symbol[offs[length[s]]++] = static_cast<short>(s);
        return left;
    }
    int Decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= Bits(1);
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            if (bad) return -1;
        }
        bad = true;
        return -1;
    }
    bool Codes(const Huff& lencode, const Huff& distcode) {
        static const short lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = Decode(lencode);
            if (bad || sym < 0) return false;
            if (sym < 256) {
                out.push_back(static_cast<uint8_t>(sym));
            } else if (sym == 256) {
                return true;
            } else {
                sym -= 257;
                if (sym >= 29) return false;
                const int len = lbase[sym] + Bits(lext[sym]);
                const int ds = Decode(distcode);
                if (ds < 0 || ds >= 30) return false;
                const size_t dist = static_cast<size_t>(dbase[ds] + Bits(dext[ds]));
                if (dist > out.size()) return false;
                for (int k = 0; k < len; ++k) out.push_back(out[out.size() - dist]);
            }
        }
    }
    bool Run() {
        int last;
        do {
            last = Bits(1);
            const int type = Bits(2);
            if (bad) return false;
            if (type == 0) {
                bitbuf = 0;
                bitcnt = 0;
                if (pos + 4 > n) return false;
                const unsigned len = in[pos] | (in[pos + 1] << 8);
                pos += 4;
                if (pos + len > n) return false;
                out.insert(out.end(), in + pos, in + pos + len);
                pos += len;
            } else if (type == 1) {
                Huff l, d;
                short lengths[320];
                int s = 0;
                for (; s < 144; ++s) lengths[s] = 8;
                for (; s < 256; ++s) lengths[s] = 9;
                for (; s < 280; ++s) lengths[s] = 7;
                for (; s < 288; ++s) lengths[s] = 8;
                Build(l, lengths, 288);
                for (s = 0; s < 30; ++s) lengths[s] = 5;
                Build(d, lengths, 30);
                if (!Codes(l, d)) return false;
            } else if (type == 2) {
                static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                const int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
                if (nlen > 286 || ndist > 30) return false;
                short lengths[320] = {};
                for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<short>(Bits(3));
                Huff lc;
                if (Build(lc, lengths, 19) != 0) return false;  // must be complete
                int idx = 0;
                while (idx < nlen + ndist) {
                    int sym = Decode(lc);
                    if (sym < 0) return false;
                    if (sym < 16) {
                        lengths[idx++] = static_cast<short>(sym);
                    } else {
                        short len = 0;
                        int rep;
                        if (sym == 16) {
                            if (idx == 0) return false;
                            len = lengths[idx - 1];
                            rep = 3 + Bits(2);
                        } else if (sym == 17) {
                            rep = 3 + Bits(3);
                        } else {
                            rep = 11 + Bits(7);
                        }
                        if (idx + rep > nlen + ndist) return false;
                        while (rep--) lengths[idx++] = len;
                    }
                }
                if (lengths[256] == 0) return false;
                Huff l, d;
                const int el = Build(l, lengths, nlen);
                if (el < 0 || (el > 0 && nlen - l.count[0] != 1)) return false;  // incomplete only for one code
                const int ed = Build(d, lengths + nlen, ndist);
                if (ed < 0 || (ed > 0 && ndist - d.count[0] != 1)) return false;
                if (!Codes(l, d)) return false;
            } else {
                return false;
            }
        } while (!last);
        return !bad;
    }
};

inline bool InflateAll(const std::vector<uint8_t>& comp, std::vector<uint8_t>& out) {
    Inflate inf{comp.data(), comp.size()};
    const bool ok = inf.Run();
    out = std::move(inf.out);
    return ok;
}

inline uint32_t Rd32(const std::vector<uint8_t>& b, size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}
inline uint16_t Rd16(const std::vector<uint8_t>& b, size_t at) { return static_cast<uint16_t>(b[at] | (b[at + 1] << 8)); }

inline std::vector<uint8_t> ReadFileBytes(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Every entry of a zip: name -> inflated data (false when the structure or a CRC is wrong).
inline bool ReadZip(const std::vector<uint8_t>& z, std::vector<std::pair<std::string, std::vector<uint8_t>>>& out) {
    if (z.size() < 22) return false;
    const size_t eocd = z.size() - 22;
    if (Rd32(z, eocd) != 0x06054b50) return false;
    const unsigned entries = Rd16(z, eocd + 10);
    size_t cd = Rd32(z, eocd + 16);
    for (unsigned i = 0; i < entries; ++i) {
        if (cd + 46 > z.size() || Rd32(z, cd) != 0x02014b50) return false;
        const uint32_t crc = Rd32(z, cd + 16), comp = Rd32(z, cd + 20), size = Rd32(z, cd + 24);
        const unsigned nameLen = Rd16(z, cd + 28), extra = Rd16(z, cd + 30), comment = Rd16(z, cd + 32);
        const size_t local = Rd32(z, cd + 42);
        std::string name(reinterpret_cast<const char*>(&z[cd + 46]), nameLen);
        if (Rd32(z, local) != 0x04034b50 || Rd32(z, local + 14) != crc || Rd32(z, local + 18) != comp) return false;
        const size_t data = local + 30 + Rd16(z, local + 26) + Rd16(z, local + 28);
        std::vector<uint8_t> c(z.begin() + static_cast<std::ptrdiff_t>(data),
                               z.begin() + static_cast<std::ptrdiff_t>(data + comp));
        std::vector<uint8_t> plain;
        if (!InflateAll(c, plain) || plain.size() != size || mc::core::zip::Crc32(0, plain.data(), plain.size()) != crc)
            return false;
        out.push_back({name, std::move(plain)});
        cd += 46 + nameLen + extra + comment;
    }
    return true;
}


}  // namespace mc::testzip
