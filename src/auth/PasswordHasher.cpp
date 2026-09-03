#include "auth/PasswordHasher.h"

#include <argon2.h>
#include <sys/random.h>

#include <array>
#include <stdexcept>
#include <vector>

namespace wikicore::auth {

namespace {

constexpr uint32_t kTimeCost = 2;         // iterations
constexpr uint32_t kMemoryCostKiB = 19456;  // ~19 MiB
constexpr uint32_t kParallelism = 1;
constexpr size_t kSaltLen = 16;
constexpr size_t kHashLen = 32;

std::array<uint8_t, kSaltLen> randomSalt() {
  std::array<uint8_t, kSaltLen> salt{};
  // getrandom(2): blocks only if the kernel CSPRNG isn't yet seeded, which
  // in practice means "never, on a running system" — the right choice
  // here over std::random_device, whose quality is implementation-defined.
  ssize_t got = 0;
  while (got < static_cast<ssize_t>(salt.size())) {
    const ssize_t n = getrandom(salt.data() + got, salt.size() - got, 0);
    if (n < 0) {
      throw std::runtime_error("getrandom() failed while generating salt");
    }
    got += n;
  }
  return salt;
}

}  // namespace

std::string PasswordHasher::hash(const std::string& plaintextPassword) {
  const auto salt = randomSalt();

  const size_t encodedLen = argon2_encodedlen(
      kTimeCost, kMemoryCostKiB, kParallelism, static_cast<uint32_t>(kSaltLen),
      static_cast<uint32_t>(kHashLen), Argon2_id);
  std::vector<char> encoded(encodedLen);

  const int rc = argon2id_hash_encoded(
      kTimeCost, kMemoryCostKiB, kParallelism, plaintextPassword.data(),
      plaintextPassword.size(), salt.data(), salt.size(), kHashLen,
      encoded.data(), encoded.size());
  if (rc != ARGON2_OK) {
    throw std::runtime_error(std::string("argon2id_hash_encoded failed: ") +
                              argon2_error_message(rc));
  }

  return std::string(encoded.data());  // encoded is NUL-terminated
}

bool PasswordHasher::verify(const std::string& encodedHash,
                             const std::string& plaintextPassword) {
  const int rc = argon2id_verify(encodedHash.c_str(), plaintextPassword.data(),
                                  plaintextPassword.size());
  return rc == ARGON2_OK;
}

}  // namespace wikicore::auth
