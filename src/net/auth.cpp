#include "net/auth.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <mutex>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace mc::net::auth {
namespace {

// Domain separation: an answer for MafiaCoop's challenge is useless as an HMAC of the same bytes anywhere else.
constexpr char kAuthContext[] = "MafiaCoop session auth v1";

BCRYPT_ALG_HANDLE HmacAlg() {
    static BCRYPT_ALG_HANDLE alg = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                        BCRYPT_ALG_HANDLE_HMAC_FLAG)))
            alg = nullptr;
    });
    return alg;
}

}  // namespace

bool SecureRandom(void* out, size_t len) {
    if (len > 0xFFFFFFFFu) return false;
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(len),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool HmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> msg, uint8_t out[kMacSize]) {
    std::memset(out, 0, kMacSize);
    BCRYPT_ALG_HANDLE alg = HmacAlg();
    if (!alg || key.size() > 0xFFFFFFFFu || msg.size() > 0xFFFFFFFFu) return false;
    BCRYPT_HASH_HANDLE h = nullptr;
    // BCrypt wants a non-null key pointer even for an empty key.
    static uint8_t empty = 0;
    PUCHAR k = key.empty() ? &empty : const_cast<PUCHAR>(key.data());
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, k, static_cast<ULONG>(key.size()), 0))) return false;
    bool ok = BCRYPT_SUCCESS(BCryptHashData(h, const_cast<PUCHAR>(msg.data()), static_cast<ULONG>(msg.size()), 0)) &&
              BCRYPT_SUCCESS(BCryptFinishHash(h, out, static_cast<ULONG>(kMacSize), 0));
    BCryptDestroyHash(h);
    if (!ok) std::memset(out, 0, kMacSize);
    return ok;
}

bool Response(std::string_view password, const uint8_t challenge[kChallengeSize], uint8_t out[kMacSize]) {
    std::vector<uint8_t> msg(kAuthContext, kAuthContext + sizeof(kAuthContext) - 1);
    msg.insert(msg.end(), challenge, challenge + kChallengeSize);
    std::span<const uint8_t> key(reinterpret_cast<const uint8_t*>(password.data()), password.size());
    return HmacSha256(key, msg, out);
}

bool ConstantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

}  // namespace mc::net::auth
