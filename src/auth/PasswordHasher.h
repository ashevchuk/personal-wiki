#pragma once

#include <string>

namespace wikicore::auth {

// argon2id password hashing. Parameters below are the OWASP-baseline
// minimums (m_cost=19 MiB, t_cost=2, p=1) — trivial memory/CPU cost for
// any Raspberry Pi 4/5, and this is a single-admin login gated by
// RateLimiter besides, so there's no reason to go higher and slow down
// every request on weaker hardware. Bench against real target hardware
// before deploying if that assumption ever needs revisiting (per the
// plan's M1 note) — not done here, no RPi available in this environment.
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
