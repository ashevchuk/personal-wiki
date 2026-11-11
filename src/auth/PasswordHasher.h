#pragma once

#include <cstddef>
#include <string>

namespace wikicore::auth {

// Minimum for a NEW password, in characters (UTF-8 code points). Existing
// shorter passwords keep working until they are changed.
inline constexpr size_t kMinPasswordLength = 12;

// Maximum, in bytes, for any password a request submits. Nothing longer is
// ever hashed or compared.
inline constexpr size_t kMaxPasswordBytes = 1024;

// Empty when `password` is acceptable as a new password, otherwise the
// reason to show the person choosing it.
std::string newPasswordProblem(const std::string& password);

// argon2id password hashing. Parameters below are the OWASP-baseline
// minimums (m_cost=19 MiB, t_cost=2, p=1) — trivial memory/CPU cost for
// any Raspberry Pi 4/5, and this is a single-admin login gated by
// RateLimiter besides, so there's no reason to go higher and slow down
// every request on weaker hardware.
class PasswordHasher {
 public:
  // Returns a self-describing encoded hash (algorithm + params + salt +
  // hash, as a single string) suitable for storing in users.password_hash.
  // Throws std::runtime_error on failure (should not happen barring OOM).
  static std::string hash(const std::string& plaintextPassword);

  // Constant-time-verified via argon2's own comparison.
  static bool verify(const std::string& encodedHash,
                      const std::string& plaintextPassword);
};

}  // namespace wikicore::auth
