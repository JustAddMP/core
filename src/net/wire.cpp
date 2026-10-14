#include "net/wire.h"

#include <cstring>

namespace mc::net {

void ByteWriter::U16(uint16_t v) {
    buf_.push_back(static_cast<uint8_t>(v));
    buf_.push_back(static_cast<uint8_t>(v >> 8));
}
void ByteWriter::U32(uint32_t v) {
    for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void ByteWriter::U64(uint64_t v) {
    for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void ByteWriter::F32(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    U32(u);
}
void ByteWriter::F64(double v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    U64(u);
}
void ByteWriter::VarU64(uint64_t v) {
    while (v >= 0x80) {
        buf_.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    buf_.push_back(static_cast<uint8_t>(v));
}
void ByteWriter::VarI32(int32_t v) {
    uint32_t zz = (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
    VarU32(zz);
}
void ByteWriter::Str(std::string_view s) {
    VarU64(s.size());
    Bytes(s.data(), s.size());
}
void ByteWriter::Bytes(const void* data, size_t len) {
    if (!len) return;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    buf_.insert(buf_.end(), p, p + len);
}
void ByteWriter::PatchU16(size_t o, uint16_t v) {
    if (o + 2 > buf_.size()) return;
    buf_[o] = static_cast<uint8_t>(v);
    buf_[o + 1] = static_cast<uint8_t>(v >> 8);
}
void ByteWriter::PatchU32(size_t o, uint32_t v) {
    if (o + 4 > buf_.size()) return;
    for (int i = 0; i < 4; ++i) buf_[o + i] = static_cast<uint8_t>(v >> (8 * i));
}

bool ByteReader::Need(size_t n) {
    if (failed_ || n > len_ - pos_) {
        failed_ = true;
        return false;
    }
    return true;
}
uint8_t ByteReader::U8() {
    if (!Need(1)) return 0;
    return p_[pos_++];
}
uint16_t ByteReader::U16() {
    if (!Need(2)) return 0;
    uint16_t v = static_cast<uint16_t>(p_[pos_] | (p_[pos_ + 1] << 8));
    pos_ += 2;
    return v;
}
uint32_t ByteReader::U32() {
    if (!Need(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p_[pos_ + i]) << (8 * i);
    pos_ += 4;
    return v;
}
uint64_t ByteReader::U64() {
    if (!Need(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p_[pos_ + i]) << (8 * i);
    pos_ += 8;
    return v;
}
float ByteReader::F32() {
    uint32_t u = U32();
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
double ByteReader::F64() {
    uint64_t u = U64();
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}
uint64_t ByteReader::VarU64() {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        uint8_t b = U8();
        if (failed_) return 0;
        v |= static_cast<uint64_t>(b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
    }
    failed_ = true;  // over-long encoding
    return 0;
}
uint32_t ByteReader::VarU32() {
    uint64_t v = VarU64();
    if (v > 0xFFFFFFFFull) {
        failed_ = true;
        return 0;
    }
    return static_cast<uint32_t>(v);
}
int32_t ByteReader::VarI32() {
    uint32_t zz = VarU32();
    return static_cast<int32_t>((zz >> 1) ^ (0u - (zz & 1)));
}
std::string ByteReader::Str(size_t maxLen) {
    uint64_t n = VarU64();
    if (failed_) return {};
    if (n > maxLen || !Need(static_cast<size_t>(n))) {
        failed_ = true;
        return {};
    }
    std::string s(reinterpret_cast<const char*>(p_ + pos_), static_cast<size_t>(n));
    pos_ += static_cast<size_t>(n);
    return s;
}
bool ByteReader::Bytes(void* out, size_t len) {
    if (!Need(len)) return false;
    if (len) std::memcpy(out, p_ + pos_, len);
    pos_ += len;
    return true;
}
std::span<const uint8_t> ByteReader::View(size_t len) {
    if (!Need(len)) return {};
    std::span<const uint8_t> s(p_ + pos_, len);
    pos_ += len;
    return s;
}
std::span<const uint8_t> ByteReader::Rest() {
    if (failed_) return {};
    std::span<const uint8_t> s(p_ + pos_, len_ - pos_);
    pos_ = len_;
    return s;
}
void ByteReader::Skip(size_t len) {
    if (Need(len)) pos_ += len;
}

}  // namespace mc::net
