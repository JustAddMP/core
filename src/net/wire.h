// net: byte writer/reader for message encoding.
// Little-endian fixed-size ints, IEEE floats, LEB128 varints, length-prefixed strings.
// Reads are bounds-checked: on overrun the reader sets Failed() and returns zero values; it never throws.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mc::net {

class ByteWriter {
public:
    ByteWriter() = default;
    explicit ByteWriter(size_t reserve) { buf_.reserve(reserve); }

    void U8(uint8_t v) { buf_.push_back(v); }
    void U16(uint16_t v);
    void U32(uint32_t v);
    void U64(uint64_t v);
    void I8(int8_t v) { U8(static_cast<uint8_t>(v)); }
    void I16(int16_t v) { U16(static_cast<uint16_t>(v)); }
    void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
    void I64(int64_t v) { U64(static_cast<uint64_t>(v)); }
    void F32(float v);
    void F64(double v);
    void Bool(bool v) { U8(v ? 1 : 0); }
    void VarU32(uint32_t v) { VarU64(v); }
    void VarU64(uint64_t v);
    void VarI32(int32_t v);  // zig-zag
    // Varint byte length followed by the bytes (no terminator).
    void Str(std::string_view s);
    void Bytes(const void* data, size_t len);
    void Bytes(std::span<const uint8_t> s) { Bytes(s.data(), s.size()); }

    // Overwrite a previously written U16/U32 (e.g. a length placeholder).
    void PatchU16(size_t offset, uint16_t v);
    void PatchU32(size_t offset, uint32_t v);

    size_t Size() const { return buf_.size(); }
    const uint8_t* Data() const { return buf_.data(); }
    std::span<const uint8_t> Span() const { return {buf_.data(), buf_.size()}; }
    std::vector<uint8_t>& Buffer() { return buf_; }
    std::vector<uint8_t> Take() { return std::move(buf_); }
    void Clear() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
};

class ByteReader {
public:
    ByteReader() = default;
    ByteReader(const void* data, size_t len) : p_(static_cast<const uint8_t*>(data)), len_(len) {}
    explicit ByteReader(std::span<const uint8_t> s) : p_(s.data()), len_(s.size()) {}

    uint8_t U8();
    uint16_t U16();
    uint32_t U32();
    uint64_t U64();
    int8_t I8() { return static_cast<int8_t>(U8()); }
    int16_t I16() { return static_cast<int16_t>(U16()); }
    int32_t I32() { return static_cast<int32_t>(U32()); }
    int64_t I64() { return static_cast<int64_t>(U64()); }
    float F32();
    double F64();
    bool Bool() { return U8() != 0; }
    uint32_t VarU32();
    uint64_t VarU64();
    int32_t VarI32();
    // Fails if the encoded length exceeds maxLen or the remaining bytes.
    std::string Str(size_t maxLen = 4096);
    // Copies len bytes; fails (and zero-fills nothing) on overrun.
    bool Bytes(void* out, size_t len);
    // Returns a view of the next len bytes and advances; empty span + Failed() on overrun.
    std::span<const uint8_t> View(size_t len);
    // Returns the rest of the buffer and advances to the end.
    std::span<const uint8_t> Rest();
    void Skip(size_t len);

    bool Failed() const { return failed_; }
    bool Ok() const { return !failed_; }
    bool AtEnd() const { return pos_ >= len_; }
    size_t Remaining() const { return failed_ ? 0 : len_ - pos_; }
    size_t Position() const { return pos_; }
    void Fail() { failed_ = true; }

private:
    bool Need(size_t n);
    const uint8_t* p_ = nullptr;
    size_t len_ = 0;
    size_t pos_ = 0;
    bool failed_ = false;
};

}  // namespace mc::net
