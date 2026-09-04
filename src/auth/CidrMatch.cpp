#include "auth/CidrMatch.h"

#include <arpa/inet.h>

#include <cstdint>
#include <cstring>

namespace wikicore::auth {

namespace {

bool matchV4(const std::string& network, int prefixLen, const std::string& ip) {
  in_addr netAddr{};
  if (inet_pton(AF_INET, network.c_str(), &netAddr) != 1) return false;
  in_addr ipAddr{};
  if (inet_pton(AF_INET, ip.c_str(), &ipAddr) != 1) return false;

  const int bits = prefixLen < 0 ? 32 : prefixLen;
  if (bits < 0 || bits > 32) return false;

  // 1u << 32 is undefined behavior for a 32-bit type -- bits == 0 (match
  // everything, e.g. "0.0.0.0/0") is handled as its own case rather than
  // letting the general shift formula reach that width.
  const uint32_t mask = bits == 0 ? 0 : htonl(~((1u << (32 - bits)) - 1));
  return (netAddr.s_addr & mask) == (ipAddr.s_addr & mask);
}

bool matchV6(const std::string& network, int prefixLen, const std::string& ip) {
  in6_addr netAddr{};
  if (inet_pton(AF_INET6, network.c_str(), &netAddr) != 1) return false;
  in6_addr ipAddr{};
  if (inet_pton(AF_INET6, ip.c_str(), &ipAddr) != 1) return false;

  const int bits = prefixLen < 0 ? 128 : prefixLen;
  if (bits < 0 || bits > 128) return false;

  for (int i = 0; i < 16; ++i) {
    const int remainingBits = bits - i * 8;
    uint8_t maskByte;
    if (remainingBits >= 8) {
      maskByte = 0xFF;
    } else if (remainingBits <= 0) {
      maskByte = 0x00;
    } else {
      maskByte = static_cast<uint8_t>(0xFF << (8 - remainingBits));
    }
    if ((netAddr.s6_addr[i] & maskByte) != (ipAddr.s6_addr[i] & maskByte)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool cidrContains(const std::string& cidr, const std::string& ip) {
  std::string network = cidr;
  int prefixLen = -1;  // sentinel: "no '/' in the input at all"
  const size_t slash = cidr.find('/');
  if (slash != std::string::npos) {
    network = cidr.substr(0, slash);
    try {
      size_t consumed = 0;
      const int parsed = std::stoi(cidr.substr(slash + 1), &consumed);
      if (slash + 1 + consumed != cidr.size()) return false;  // trailing garbage
      // A literal "/-1" (or any negative prefix) must be rejected here,
      // not silently reinterpreted as "no prefix given" -- both matchV4
      // and matchV6 treat prefixLen < 0 as that sentinel, so an
      // unvalidated negative parse would collapse "/-1" into "match
      // everything at /32 or /128" instead of the malformed input it is.
      if (parsed < 0) return false;
      prefixLen = parsed;
    } catch (const std::exception&) {
      return false;
    }
  }

  // Try both families rather than sniffing which one `network` looks
  // like first -- inet_pton itself is the authority on validity, and
  // trying IPv4 then IPv6 costs nothing measurable for a handful of
  // allowlist entries checked per request.
  if (matchV4(network, prefixLen, ip)) return true;
  return matchV6(network, prefixLen, ip);
}

}  // namespace wikicore::auth
