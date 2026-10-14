#define _CRT_RAND_S
#include <stdlib.h>

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "net/net_internal.h"
#include "net/transport.h"

namespace mc::net {

namespace {
std::atomic<LogSink> g_sink{nullptr};
}

void SetLogSink(LogSink sink) { g_sink.store(sink); }

const char* ToString(DisconnectReason r) {
    switch (r) {
        case DisconnectReason::None: return "none";
        case DisconnectReason::Local: return "local";
        case DisconnectReason::Remote: return "closed by remote";
        case DisconnectReason::Timeout: return "timed out";
        case DisconnectReason::ConnectFailed: return "connect failed";
        case DisconnectReason::Refused: return "refused";
        case DisconnectReason::Error: return "error";
    }
    return "?";
}

namespace detail {

void Log(int level, const char* fmt, ...) {
    LogSink sink = g_sink.load();
    if (!sink) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sink(level, buf);
}

double Now() {
    static const double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * inv;
}

void RandomBytes(void* out, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(out);
    while (len) {
        unsigned int v = 0;
        if (rand_s(&v) != 0) v = static_cast<unsigned int>(__rdtsc() ^ GetTickCount64() ^ reinterpret_cast<uintptr_t>(p));
        size_t n = len < 4 ? len : 4;
        std::memcpy(p, &v, n);
        p += n;
        len -= n;
    }
}

uint32_t Random32() {
    uint32_t v;
    RandomBytes(&v, 4);
    return v;
}

static inline uint64_t Rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

uint64_t SipHash24(const uint8_t key[16], const void* data, size_t len) {
    uint64_t k0, k1;
    std::memcpy(&k0, key, 8);
    std::memcpy(&k1, key + 8, 8);
    uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
    auto round = [&] {
        v0 += v1; v1 = Rotl(v1, 13); v1 ^= v0; v0 = Rotl(v0, 32);
        v2 += v3; v3 = Rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = Rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = Rotl(v1, 17); v1 ^= v2; v2 = Rotl(v2, 32);
    };
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t blocks = len / 8;
    for (size_t i = 0; i < blocks; ++i) {
        uint64_t m;
        std::memcpy(&m, p + i * 8, 8);
        v3 ^= m; round(); round(); v0 ^= m;
    }
    uint64_t b = static_cast<uint64_t>(len) << 56;
    const uint8_t* tail = p + blocks * 8;
    for (size_t i = 0; i < (len & 7); ++i) b |= static_cast<uint64_t>(tail[i]) << (8 * i);
    v3 ^= b; round(); round(); v0 ^= b;
    v2 ^= 0xff;
    round(); round(); round(); round();
    return v0 ^ v1 ^ v2 ^ v3;
}

}  // namespace detail
}  // namespace mc::net
