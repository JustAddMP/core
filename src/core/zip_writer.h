#pragma once
// Zip files without zlib: CRC-32, a streaming deflate (RFC 1951: LZ77 over a 32 KiB window with hash chains,
// dynamic Huffman blocks) and the zip container (PKWARE APPNOTE: local headers, central directory; no zip64, so an
// entry stays under 2 GiB and the archive under 4 GiB). Used for the session bundles (core/bundle.h). No threads of
// its own; one writer per thread.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mc::core::zip {

// CRC-32 (IEEE, the zip / gzip one). Start with crc = 0; feed it back for the next part.
uint32_t Crc32(uint32_t crc, const void* data, size_t n);

// Streaming deflate. Everything written comes out through `sink` (in pieces); Finish ends the stream (last block).
class Deflater {
public:
    using Sink = std::function<bool(const uint8_t* data, size_t n)>;  // false: stop (an error)
    // chain: how many earlier positions with the same 3-byte hash a match search tries (more: smaller, slower).
    explicit Deflater(Sink sink, int chain = 32);
    ~Deflater();
    Deflater(const Deflater&) = delete;
    Deflater& operator=(const Deflater&) = delete;
    bool Write(const void* data, size_t n);
    bool Finish();
    uint64_t BytesIn() const { return in_; }
    uint64_t BytesOut() const { return out_; }

private:
    struct Impl;
    Impl* impl_;
    uint64_t in_ = 0, out_ = 0;
};

// One deflate stream of a whole buffer (tests, small entries).
std::vector<uint8_t> DeflateBuffer(const void* data, size_t n, int chain = 32);

constexpr uint64_t kMaxEntryBytes = (2ull << 30) - 1;  // an entry is cut here (no zip64)
constexpr uint64_t kMaxArchiveBytes = 3800ull << 20;   // no new entry once the archive is this big (< 4 GiB)

class ZipWriter {
public:
    struct EntryInfo {
        std::string name;
        uint64_t in = 0, out = 0;  // bytes before / after compression
        uint32_t crc = 0;
        std::string note;          // "" or why it was cut / left out
        bool written = false;
    };
    ZipWriter() = default;
    ~ZipWriter();
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    bool Open(const std::wstring& path);
    // A file from disk: what it holds now (it may be open for writing elsewhere), at most maxBytes (cut with a note).
    // False when it could not be read (the entry is left out; Info() says why) or writing failed.
    bool AddFile(const std::wstring& source, const std::string& name, uint64_t maxBytes = kMaxEntryBytes);
    bool AddData(const std::string& name, const void* data, size_t n);
    bool Close();  // central directory and end record; the file is complete after this
    bool Failed() const { return failed_; }
    uint64_t Size() const { return offset_; }
    const std::vector<EntryInfo>& Info() const { return info_; }

private:
    struct Central {
        std::string name;
        uint32_t crc = 0, comp = 0, size = 0, offset = 0;
        uint16_t time = 0, date = 0;
    };
    bool Raw(const void* data, size_t n);
    bool Begin(const std::string& name, uint16_t time, uint16_t date, Central& c);
    bool End(Central& c, uint32_t crc, uint64_t in, uint64_t out);
    void* file_ = nullptr;  // HANDLE
    uint64_t offset_ = 0;
    bool failed_ = false;
    std::vector<Central> central_;
    std::vector<EntryInfo> info_;
};

}  // namespace mc::core::zip
