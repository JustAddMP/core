#include "core/zip_writer.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <queue>

namespace mc::core::zip {

// ---- CRC-32 (slicing by 4) ----------------------------------------------------------------------------------------
namespace {
struct CrcTables {
    uint32_t t[4][256];
    CrcTables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 4; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};
const CrcTables& Tables() {
    static const CrcTables tables;
    return tables;
}
}  // namespace

uint32_t Crc32(uint32_t crc, const void* data, size_t n) {
    const auto& T = Tables().t;
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t c = ~crc;
    while (n >= 4) {
        uint32_t w;
        std::memcpy(&w, p, 4);
        c ^= w;
        c = T[3][c & 0xFF] ^ T[2][(c >> 8) & 0xFF] ^ T[1][(c >> 16) & 0xFF] ^ T[0][c >> 24];
        p += 4;
        n -= 4;
    }
    while (n--) c = T[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

// ---- deflate -----------------------------------------------------------------------------------------------------------
namespace {

constexpr int kWindow = 32768;
constexpr int kMinMatch = 3, kMaxMatch = 258;
constexpr int kMinLookahead = kMaxMatch + kMinMatch + 1;
constexpr int kHashBits = 15, kHashSize = 1 << kHashBits;
constexpr int kBufSize = 2 * kWindow;
constexpr size_t kBlockTokens = 32768;  // symbols per block (then a new Huffman table)
constexpr int kNice = 128;               // a match this long ends the search

// length 3..258 -> code 257..285, extra bits, base
struct LenCode {
    uint16_t code;
    uint8_t extra;
    uint16_t base;
};
constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct Codes {
    uint8_t lenSym[kMaxMatch + 1];   // length -> index into kLenBase
    uint8_t distSymLo[512];          // dist - 1 < 256 -> code; else (dist - 1) >> 7
    Codes() {
        for (int l = kMinMatch; l <= kMaxMatch; ++l) {
            int s = 28;
            while (s > 0 && kLenBase[s] > l) --s;
            lenSym[l] = static_cast<uint8_t>(s);
        }
        for (int d = 0; d < 512; ++d) {
            // d < 256: dist - 1 = d; else (dist - 1) >> 7 = d - 256 (dist up to 32768)
            const int dist = d < 256 ? d + 1 : ((d - 256) << 7) + 1;
            int s = 29;
            while (s > 0 && kDistBase[s] > dist) --s;
            distSymLo[d] = static_cast<uint8_t>(s);
        }
    }
    int DistSym(int dist) const {
        const int d = dist - 1;
        return d < 256 ? distSymLo[d] : distSymLo[256 + (d >> 7)];
    }
};
const Codes& C() {
    static const Codes c;
    return c;
}

// Huffman code lengths for freq[0..n) of at most `limit` bits (0 = unused). At least two codes when any is used.
void BuildLengths(const uint32_t* freq, int n, int limit, uint8_t* lens) {
    std::vector<uint32_t> f(freq, freq + n);
    for (;;) {
        std::fill(lens, lens + n, static_cast<uint8_t>(0));
        std::vector<int> used;
        for (int i = 0; i < n; ++i)
            if (f[i]) used.push_back(i);
        if (used.empty()) return;
        if (used.size() == 1) {  // a complete code needs two
            lens[used[0]] = 1;
            lens[used[0] == 0 ? 1 : 0] = 1;
            return;
        }
        // nodes: leaves 0..m-1, then internal ones; parent links give the depths
        const size_t m = used.size();
        std::vector<uint64_t> weight(2 * m);
        std::vector<int> parent(2 * m, -1);
        using Item = std::pair<uint64_t, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
        for (size_t i = 0; i < m; ++i) {
            weight[i] = f[static_cast<size_t>(used[i])];
            pq.push({weight[i], static_cast<int>(i)});
        }
        int next = static_cast<int>(m);
        while (pq.size() > 1) {
            const Item a = pq.top();
            pq.pop();
            const Item b = pq.top();
            pq.pop();
            weight[static_cast<size_t>(next)] = a.first + b.first;
            parent[static_cast<size_t>(a.second)] = next;
            parent[static_cast<size_t>(b.second)] = next;
            pq.push({weight[static_cast<size_t>(next)], next});
            ++next;
        }
        std::vector<int> depth(static_cast<size_t>(next), 0);
        int maxDepth = 0;
        for (int i = next - 2; i >= 0; --i) {  // parents have higher indices: top down
            depth[static_cast<size_t>(i)] = depth[static_cast<size_t>(parent[static_cast<size_t>(i)])] + 1;
        }
        for (size_t i = 0; i < m; ++i) {
            lens[used[i]] = static_cast<uint8_t>(depth[i]);
            maxDepth = std::max(maxDepth, depth[i]);
        }
        if (maxDepth <= limit) return;
        for (auto& x : f)  // too deep: flatten the frequencies and build again
            if (x) x = (x >> 1) | 1;
    }
}

// Canonical codes (bit-reversed for the LSB-first stream) from lengths.
void BuildCodes(const uint8_t* lens, int n, uint16_t* codes) {
    int count[16] = {};
    for (int i = 0; i < n; ++i) ++count[lens[i]];
    count[0] = 0;
    int nextCode[16] = {};
    int code = 0;
    for (int b = 1; b < 16; ++b) {
        code = (code + count[b - 1]) << 1;
        nextCode[b] = code;
    }
    for (int i = 0; i < n; ++i) {
        const int len = lens[i];
        if (!len) {
            codes[i] = 0;
            continue;
        }
        int c = nextCode[len]++, r = 0;
        for (int k = 0; k < len; ++k) {
            r = (r << 1) | (c & 1);
            c >>= 1;
        }
        codes[i] = static_cast<uint16_t>(r);
    }
}

}  // namespace

struct Deflater::Impl {
    Sink sink;
    int chain;
    std::vector<uint8_t> buf = std::vector<uint8_t>(kBufSize + kMaxMatch + 16, 0);
    std::vector<int32_t> head = std::vector<int32_t>(kHashSize, -1);
    std::vector<int32_t> prev = std::vector<int32_t>(kWindow, -1);
    int strstart = 0, lookahead = 0;
    uint32_t hash = 0;
    // tokens of the block: dist 0 = literal (lit in len)
    std::vector<uint16_t> tokLen, tokDist;
    uint32_t litFreq[286] = {}, distFreq[30] = {};
    // bit output
    uint64_t bits = 0;
    int nbits = 0;
    std::vector<uint8_t> out;
    uint64_t* outCount = nullptr;
    bool failed = false;

    Impl(Sink s, int c) : sink(std::move(s)), chain(std::max(1, c)) {
        tokLen.reserve(kBlockTokens);
        tokDist.reserve(kBlockTokens);
        out.reserve(1 << 17);
    }

    static uint32_t Hash3(const uint8_t* p) {
        return ((static_cast<uint32_t>(p[0]) << 10) ^ (static_cast<uint32_t>(p[1]) << 5) ^ p[2]) & (kHashSize - 1);
    }

    void Put(uint32_t value, int n) {
        bits |= static_cast<uint64_t>(value) << nbits;
        nbits += n;
        while (nbits >= 8) {
            out.push_back(static_cast<uint8_t>(bits));
            bits >>= 8;
            nbits -= 8;
        }
    }
    bool Drain(bool all) {
        if (all && nbits > 0) {
            out.push_back(static_cast<uint8_t>(bits));
            bits = 0;
            nbits = 0;
        }
        if (out.empty() || (!all && out.size() < (1 << 16))) return !failed;
        if (!failed && !sink(out.data(), out.size())) failed = true;
        *outCount += out.size();
        out.clear();
        return !failed;
    }

    void Insert(int pos) {  // pos has at least 3 bytes behind it in buf
        const uint32_t h = Hash3(&buf[static_cast<size_t>(pos)]);
        prev[static_cast<size_t>(pos & (kWindow - 1))] = head[h];
        head[h] = pos;
    }

    int Longest(int pos, int limit, int* matchPos) {
        int best = kMinMatch - 1;
        int cand = prev[static_cast<size_t>(pos & (kWindow - 1))];  // Insert(pos) was just done: skip pos itself
        const uint8_t* s = &buf[static_cast<size_t>(pos)];
        for (int n = chain; n > 0 && cand >= 0 && pos - cand <= kWindow - 1; --n) {
            const uint8_t* m = &buf[static_cast<size_t>(cand)];
            if (m[best] == s[best] && m[0] == s[0] && m[1] == s[1]) {
                int len = 2;
                while (len + 8 <= limit) {
                    uint64_t a, b;
                    std::memcpy(&a, s + len, 8);
                    std::memcpy(&b, m + len, 8);
                    if (a != b) {
                        unsigned long bit;
                        _BitScanForward64(&bit, a ^ b);
                        len += static_cast<int>(bit >> 3);
                        goto done;
                    }
                    len += 8;
                }
                while (len < limit && s[len] == m[len]) ++len;
            done:
                if (len > limit) len = limit;
                if (len > best) {
                    best = len;
                    *matchPos = cand;
                    if (len >= kNice || len >= limit) break;
                }
            }
            cand = prev[static_cast<size_t>(cand & (kWindow - 1))];
        }
        return best;
    }

    void Literal(uint8_t c) {
        tokLen.push_back(c);
        tokDist.push_back(0);
        ++litFreq[c];
    }
    void Match(int len, int dist) {
        tokLen.push_back(static_cast<uint16_t>(len));
        tokDist.push_back(static_cast<uint16_t>(dist));
        ++litFreq[257 + C().lenSym[len]];
        ++distFreq[C().DistSym(dist)];
    }

    void Slide() {
        std::memmove(buf.data(), buf.data() + kWindow, static_cast<size_t>(kWindow));
        strstart -= kWindow;
        for (auto& h : head) h = h >= kWindow ? h - kWindow : -1;
        for (auto& p : prev) p = p >= kWindow ? p - kWindow : -1;
    }

    // Emits the block of the tokens so far (dynamic Huffman).
    void FlushBlock(bool last) {
        ++litFreq[256];  // end of block
        uint8_t litLen[286] = {}, distLen[30] = {};
        BuildLengths(litFreq, 286, 15, litLen);
        BuildLengths(distFreq, 30, 15, distLen);
        int hlit = 286;
        while (hlit > 257 && litLen[hlit - 1] == 0) --hlit;
        int hdist = 30;
        while (hdist > 1 && distLen[hdist - 1] == 0) --hdist;
        if (distLen[0] == 0 && hdist == 1) {  // no distances at all: one unused code (decoders accept it)
            distLen[0] = 1;
        }
        // run-length coded lengths
        uint8_t all[316];
        std::memcpy(all, litLen, static_cast<size_t>(hlit));
        std::memcpy(all + hlit, distLen, static_cast<size_t>(hdist));
        const int total = hlit + hdist;
        std::vector<std::pair<uint8_t, uint8_t>> rle;  // symbol, extra value
        uint32_t clFreq[19] = {};
        for (int i = 0; i < total;) {
            const uint8_t l = all[i];
            int run = 1;
            while (i + run < total && all[i + run] == l) ++run;
            i += run;
            if (l == 0) {
                while (run >= 11) {
                    const int r = std::min(run, 138);
                    rle.push_back({static_cast<uint8_t>(18), static_cast<uint8_t>(r - 11)});
                    run -= r;
                }
                if (run >= 3) {
                    rle.push_back({static_cast<uint8_t>(17), static_cast<uint8_t>(run - 3)});
                    run = 0;
                }
                while (run-- > 0) rle.push_back({static_cast<uint8_t>(0), static_cast<uint8_t>(0)});
            } else {
                rle.push_back({l, static_cast<uint8_t>(0)});
                --run;
                while (run >= 3) {
                    const int r = std::min(run, 6);
                    rle.push_back({static_cast<uint8_t>(16), static_cast<uint8_t>(r - 3)});
                    run -= r;
                }
                while (run-- > 0) rle.push_back({l, static_cast<uint8_t>(0)});
            }
        }
        for (const auto& [sym, ex] : rle) ++clFreq[sym];
        uint8_t clLen[19] = {};
        BuildLengths(clFreq, 19, 7, clLen);
        uint16_t clCode[19], litCode[286], distCode[30];
        BuildCodes(clLen, 19, clCode);
        BuildCodes(litLen, 286, litCode);
        BuildCodes(distLen, 30, distCode);
        int hclen = 19;
        while (hclen > 4 && clLen[kClOrder[hclen - 1]] == 0) --hclen;

        Put(last ? 1 : 0, 1);
        Put(2, 2);  // dynamic Huffman
        Put(static_cast<uint32_t>(hlit - 257), 5);
        Put(static_cast<uint32_t>(hdist - 1), 5);
        Put(static_cast<uint32_t>(hclen - 4), 4);
        for (int i = 0; i < hclen; ++i) Put(clLen[kClOrder[i]], 3);
        for (const auto& [sym, ex] : rle) {
            Put(clCode[sym], clLen[sym]);
            if (sym == 16) Put(ex, 2);
            else if (sym == 17) Put(ex, 3);
            else if (sym == 18) Put(ex, 7);
        }
        const Codes& cs = C();
        for (size_t i = 0; i < tokLen.size(); ++i) {
            const int d = tokDist[i];
            if (d == 0) {
                Put(litCode[tokLen[i]], litLen[tokLen[i]]);
            } else {
                const int len = tokLen[i];
                const int ls = cs.lenSym[len];
                Put(litCode[257 + ls], litLen[257 + ls]);
                if (kLenExtra[ls]) Put(static_cast<uint32_t>(len - kLenBase[ls]), kLenExtra[ls]);
                const int ds = cs.DistSym(d);
                Put(distCode[ds], distLen[ds]);
                if (kDistExtra[ds]) Put(static_cast<uint32_t>(d - kDistBase[ds]), kDistExtra[ds]);
            }
            if ((i & 4095) == 0) Drain(false);
        }
        Put(litCode[256], litLen[256]);
        tokLen.clear();
        tokDist.clear();
        std::memset(litFreq, 0, sizeof litFreq);
        std::memset(distFreq, 0, sizeof distFreq);
        Drain(last);
    }

    // Tokenises from strstart while there is enough lookahead (all: until the end).
    void Process(bool all) {
        while (lookahead >= kMinLookahead || (all && lookahead > 0)) {
            int len = 0, matchPos = 0;
            if (lookahead >= kMinMatch) {
                Insert(strstart);
                len = Longest(strstart, std::min(kMaxMatch, lookahead), &matchPos);
            }
            if (len >= kMinMatch) {
                Match(len, strstart - matchPos);
                // the positions inside the match go into the hash too (short matches only: speed)
                if (len <= 32) {
                    for (int k = 1; k < len; ++k)
                        if (lookahead - k >= kMinMatch) Insert(strstart + k);
                }
                strstart += len;
                lookahead -= len;
            } else {
                Literal(buf[static_cast<size_t>(strstart)]);
                ++strstart;
                --lookahead;
            }
            if (tokLen.size() >= kBlockTokens) FlushBlock(false);
            if (strstart >= kBufSize - kMinLookahead) Slide();
        }
    }

    bool Write(const uint8_t* p, size_t n) {
        while (n > 0 && !failed) {
            const size_t room = static_cast<size_t>(kBufSize - (strstart + lookahead));
            const size_t take = std::min(room, n);
            std::memcpy(&buf[static_cast<size_t>(strstart + lookahead)], p, take);
            lookahead += static_cast<int>(take);
            p += take;
            n -= take;
            Process(false);
            if (strstart + lookahead >= kBufSize && strstart >= kWindow) Slide();  // full: make room
        }
        return !failed;
    }

    bool Finish() {
        Process(true);
        FlushBlock(true);
        return Drain(true);
    }
};

Deflater::Deflater(Sink sink, int chain) : impl_(new Impl(std::move(sink), chain)) { impl_->outCount = &out_; }
Deflater::~Deflater() { delete impl_; }

bool Deflater::Write(const void* data, size_t n) {
    in_ += n;
    return impl_->Write(static_cast<const uint8_t*>(data), n);
}

bool Deflater::Finish() { return impl_->Finish(); }

std::vector<uint8_t> DeflateBuffer(const void* data, size_t n, int chain) {
    std::vector<uint8_t> out;
    Deflater d([&](const uint8_t* p, size_t k) {
        out.insert(out.end(), p, p + k);
        return true;
    }, chain);
    d.Write(data, n);
    d.Finish();
    return out;
}

// ---- zip container -------------------------------------------------------------------------------------------------------
namespace {
void Le16(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void Le32(std::vector<uint8_t>& b, uint32_t v) {
    Le16(b, v & 0xFFFF);
    Le16(b, v >> 16);
}
void DosTime(const FILETIME& ft, uint16_t& time, uint16_t& date) {
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) {
        GetLocalTime(&st);
    }
    if (st.wYear < 1980) st.wYear = 1980;
    time = static_cast<uint16_t>((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    date = static_cast<uint16_t>(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
}
FILETIME NowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ft;
}
}  // namespace

ZipWriter::~ZipWriter() {
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
}

bool ZipWriter::Open(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    file_ = h;
    offset_ = 0;
    failed_ = false;
    central_.clear();
    info_.clear();
    return true;
}

bool ZipWriter::Raw(const void* data, size_t n) {
    if (failed_ || !file_) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    while (n > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(n, 1u << 24));
        DWORD w = 0;
        if (!WriteFile(static_cast<HANDLE>(file_), p, chunk, &w, nullptr) || w != chunk) {
            failed_ = true;
            return false;
        }
        p += chunk;
        n -= chunk;
        offset_ += chunk;
    }
    return true;
}

bool ZipWriter::Begin(const std::string& name, uint16_t time, uint16_t date, Central& c) {
    c.name = name;
    c.time = time;
    c.date = date;
    c.offset = static_cast<uint32_t>(offset_);
    std::vector<uint8_t> h;
    Le32(h, 0x04034b50);
    Le16(h, 20);      // version needed: deflate
    Le16(h, 0x0800);  // UTF-8 names
    Le16(h, 8);       // deflate
    Le16(h, time);
    Le16(h, date);
    Le32(h, 0);  // crc, sizes: patched in End
    Le32(h, 0);
    Le32(h, 0);
    Le16(h, static_cast<uint32_t>(name.size()));
    Le16(h, 0);
    h.insert(h.end(), name.begin(), name.end());
    return Raw(h.data(), h.size());
}

bool ZipWriter::End(Central& c, uint32_t crc, uint64_t in, uint64_t out) {
    if (failed_) return false;
    c.crc = crc;
    c.size = static_cast<uint32_t>(in);
    c.comp = static_cast<uint32_t>(out);
    std::vector<uint8_t> p;
    Le32(p, crc);
    Le32(p, c.comp);
    Le32(p, c.size);
    LARGE_INTEGER at, back;
    at.QuadPart = static_cast<LONGLONG>(c.offset) + 14;
    back.QuadPart = static_cast<LONGLONG>(offset_);
    DWORD w = 0;
    HANDLE h = static_cast<HANDLE>(file_);
    if (!SetFilePointerEx(h, at, nullptr, FILE_BEGIN) || !WriteFile(h, p.data(), 12, &w, nullptr) || w != 12 ||
        !SetFilePointerEx(h, back, nullptr, FILE_BEGIN)) {
        failed_ = true;
        return false;
    }
    central_.push_back(c);
    return true;
}

bool ZipWriter::AddFile(const std::wstring& source, const std::string& name, uint64_t maxBytes) {
    EntryInfo info;
    info.name = name;
    if (failed_) return false;
    if (offset_ >= kMaxArchiveBytes) {
        info.note = "left out: the archive is full (no zip64)";
        info_.push_back(info);
        return false;
    }
    HANDLE f = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        info.note = "left out: could not be read (error " + std::to_string(GetLastError()) + ")";
        info_.push_back(info);
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    FILETIME mtime{};
    GetFileTime(f, nullptr, nullptr, &mtime);
    uint64_t want = static_cast<uint64_t>(size.QuadPart);
    const uint64_t cap = std::min(maxBytes, kMaxEntryBytes);
    if (want > cap) {
        info.note = "cut to the first " + std::to_string(cap >> 20) + " MiB of " + std::to_string(want >> 20) + " MiB";
        want = cap;
    }
    uint16_t t = 0, d = 0;
    DosTime(mtime, t, d);
    Central c;
    if (!Begin(name, t, d, c)) {
        CloseHandle(f);
        return false;
    }
    uint32_t crc = 0;
    Deflater def([this](const uint8_t* p, size_t n) { return Raw(p, n); });
    std::vector<uint8_t> chunk(1 << 20);
    uint64_t left = want;
    while (left > 0 && !failed_) {
        DWORD got = 0;
        const DWORD ask = static_cast<DWORD>(std::min<uint64_t>(left, chunk.size()));
        if (!ReadFile(f, chunk.data(), ask, &got, nullptr) || got == 0) break;  // shorter than it said: what came
        crc = Crc32(crc, chunk.data(), got);
        def.Write(chunk.data(), got);
        left -= got;
    }
    CloseHandle(f);
    def.Finish();
    if (def.BytesOut() > 0xFFFFFFFFull) failed_ = true;
    info.in = def.BytesIn();
    info.out = def.BytesOut();
    info.crc = crc;
    const bool ok = End(c, crc, def.BytesIn(), def.BytesOut());
    info.written = ok;
    info_.push_back(info);
    return ok;
}

bool ZipWriter::AddData(const std::string& name, const void* data, size_t n) {
    EntryInfo info;
    info.name = name;
    if (failed_) return false;
    uint16_t t = 0, d = 0;
    DosTime(NowFileTime(), t, d);
    Central c;
    if (!Begin(name, t, d, c)) return false;
    Deflater def([this](const uint8_t* p, size_t k) { return Raw(p, k); });
    def.Write(data, n);
    def.Finish();
    const uint32_t crc = Crc32(0, data, n);
    info.in = def.BytesIn();
    info.out = def.BytesOut();
    info.crc = crc;
    const bool ok = End(c, crc, def.BytesIn(), def.BytesOut());
    info.written = ok;
    info_.push_back(info);
    return ok;
}

bool ZipWriter::Close() {
    if (!file_) return false;
    bool ok = !failed_;
    if (ok) {
        const uint32_t cdStart = static_cast<uint32_t>(offset_);
        std::vector<uint8_t> cd;
        for (const Central& c : central_) {
            Le32(cd, 0x02014b50);
            Le16(cd, 20);  // made by: MS-DOS / FAT attributes, spec 2.0
            Le16(cd, 20);
            Le16(cd, 0x0800);
            Le16(cd, 8);
            Le16(cd, c.time);
            Le16(cd, c.date);
            Le32(cd, c.crc);
            Le32(cd, c.comp);
            Le32(cd, c.size);
            Le16(cd, static_cast<uint32_t>(c.name.size()));
            Le16(cd, 0);  // extra
            Le16(cd, 0);  // comment
            Le16(cd, 0);  // disk
            Le16(cd, 0);  // internal attributes
            Le32(cd, 0);  // external attributes
            Le32(cd, c.offset);
            cd.insert(cd.end(), c.name.begin(), c.name.end());
        }
        Le32(cd, 0x06054b50);
        Le16(cd, 0);
        Le16(cd, 0);
        Le16(cd, static_cast<uint32_t>(central_.size()));
        Le16(cd, static_cast<uint32_t>(central_.size()));
        Le32(cd, static_cast<uint32_t>(cd.size() - 0));  // patched below (size of the directory only)
        Le32(cd, cdStart);
        Le16(cd, 0);
        const uint32_t dirSize = static_cast<uint32_t>(cd.size() - 22);
        cd[cd.size() - 10] = static_cast<uint8_t>(dirSize);
        cd[cd.size() - 9] = static_cast<uint8_t>(dirSize >> 8);
        cd[cd.size() - 8] = static_cast<uint8_t>(dirSize >> 16);
        cd[cd.size() - 7] = static_cast<uint8_t>(dirSize >> 24);
        ok = Raw(cd.data(), cd.size()) && offset_ < 0xFFFFFFFFull;
    }
    CloseHandle(static_cast<HANDLE>(file_));
    file_ = nullptr;
    return ok;
}

}  // namespace mc::core::zip
