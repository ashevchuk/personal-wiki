#include "util/Uuid.h"

#include <sys/random.h>

#include <array>
#include <cstdio>
#include <stdexcept>

namespace wikicore::util {

std::string newUuidV4() {
  std::array<unsigned char, 16> bytes{};
  ssize_t got = 0;
  while (got < static_cast<ssize_t>(bytes.size())) {
    const ssize_t n = getrandom(bytes.data() + got, bytes.size() - got, 0);
    if (n < 0) throw std::runtime_error("getrandom() failed for uuid");
    got += n;
  }

  // RFC 4122 version 4 (random) / variant 1 bits.
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);

  char buf[37];
  std::snprintf(
      buf, sizeof(buf),
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
      bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12],
      bytes[13], bytes[14], bytes[15]);
  return std::string(buf, 36);
}

}  // namespace wikicore::util
