// net: internal helpers shared by the net sources (not part of the public API).
#pragma once
#include <cstdint>

namespace mc::net::detail {

enum LogLevel { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };
void Log(int level, const char* fmt, ...);

// Monotonic seconds since an arbitrary epoch (QueryPerformanceCounter).
double Now();

// Cryptographically random bytes (RtlGenRandom via rand_s).
void RandomBytes(void* out, size_t len);
uint32_t Random32();

// SipHash-2-4 (keyed 64-bit hash) for stateless cookies.
uint64_t SipHash24(const uint8_t key[16], const void* data, size_t len);

}  // namespace mc::net::detail
