// MafiaCoop net: crypto for the optional session password (docs/protocol.md "Session password").
// Windows BCrypt only (bcrypt.lib); no other libraries. The password itself never goes on the wire: the host sends a
// random challenge and the guest answers with HMAC-SHA256(key = UTF-8 password, msg = kAuthContext || challenge).
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace mc::net::auth {

constexpr size_t kChallengeSize = 32;
constexpr size_t kMacSize = 32;

// Cryptographically random bytes from the system RNG (BCryptGenRandom). False if the RNG failed (out untouched).
bool SecureRandom(void* out, size_t len);

// HMAC-SHA256. False if BCrypt failed (out zeroed).
bool HmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> msg, uint8_t out[kMacSize]);

// The guest's answer to a challenge (see the header comment).
bool Response(std::string_view password, const uint8_t challenge[kChallengeSize], uint8_t out[kMacSize]);

// Compares without an early exit, so the time taken does not depend on where the first difference is.
bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len);

}  // namespace mc::net::auth
