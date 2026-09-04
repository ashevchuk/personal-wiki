#include "auth/Crypto.h"

#include <openssl/evp.h>
#include <sys/random.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace wikicore::auth {

namespace {

std::string toHex(const unsigned char* data, size_t len) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (size_t i = 0; i < len; ++i) {
    oss << std::setw(2) << static_cast<int>(data[i]);
  }
  return oss.str();
}

}  // namespace

std::string randomHexToken() {
  std::array<unsigned char, 32> bytes{};
  ssize_t got = 0;
  while (got < static_cast<ssize_t>(bytes.size())) {
    const ssize_t n = getrandom(bytes.data() + got, bytes.size() - got, 0);
    if (n < 0) throw std::runtime_error("getrandom() failed for token");
    got += n;
  }
  return toHex(bytes.data(), bytes.size());
}

std::string sha256Hex(const std::string& input) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  size_t digestLen = 0;
  if (EVP_Q_digest(nullptr, "SHA256", nullptr, input.data(), input.size(),
                    digest, &digestLen) != 1) {
    throw std::runtime_error("EVP_Q_digest(SHA256) failed");
  }
  return toHex(digest, digestLen);
}

}  // namespace wikicore::auth
