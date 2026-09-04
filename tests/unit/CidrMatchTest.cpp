#include "auth/CidrMatch.h"

#include <catch2/catch_test_macros.hpp>

using namespace wikicore::auth;

TEST_CASE("cidrContains: bare IPv4 address acts as an exact /32 match", "[CidrMatch]") {
  REQUIRE(cidrContains("203.0.113.5", "203.0.113.5"));
  REQUIRE_FALSE(cidrContains("203.0.113.5", "203.0.113.6"));
}

TEST_CASE("cidrContains: IPv4 /24 matches the whole subnet, nothing outside it",
          "[CidrMatch]") {
  REQUIRE(cidrContains("203.0.113.0/24", "203.0.113.1"));
  REQUIRE(cidrContains("203.0.113.0/24", "203.0.113.254"));
  REQUIRE_FALSE(cidrContains("203.0.113.0/24", "203.0.114.1"));
}

TEST_CASE("cidrContains: IPv4 /0 matches everything", "[CidrMatch]") {
  REQUIRE(cidrContains("0.0.0.0/0", "1.2.3.4"));
  REQUIRE(cidrContains("0.0.0.0/0", "255.255.255.255"));
}

TEST_CASE("cidrContains: IPv4 /32 requires an exact match", "[CidrMatch]") {
  REQUIRE(cidrContains("203.0.113.5/32", "203.0.113.5"));
  REQUIRE_FALSE(cidrContains("203.0.113.5/32", "203.0.113.6"));
}

TEST_CASE("cidrContains: IPv6 bare address is an exact /128 match", "[CidrMatch]") {
  REQUIRE(cidrContains("2001:db8::1", "2001:db8::1"));
  REQUIRE_FALSE(cidrContains("2001:db8::1", "2001:db8::2"));
}

TEST_CASE("cidrContains: IPv6 prefix matches the subnet, not outside it", "[CidrMatch]") {
  REQUIRE(cidrContains("2001:db8::/32", "2001:db8:1234::5"));
  REQUIRE_FALSE(cidrContains("2001:db8::/32", "2001:db9::5"));
}

TEST_CASE("cidrContains: IPv6 prefix on a non-byte boundary", "[CidrMatch]") {
  // 2001:db8:8000::/33 -- the 33rd bit is the top bit of the third
  // 16-bit group (0x8000 has only its high bit set), a deliberately
  // awkward boundary to catch an off-by-one in the partial-byte mask.
  REQUIRE(cidrContains("2001:db8:8000::/33", "2001:db8:8fff::1"));
  REQUIRE_FALSE(cidrContains("2001:db8:8000::/33", "2001:db8:7fff::1"));
}

TEST_CASE("cidrContains: IPv4 CIDR never matches an IPv6 address and vice versa",
          "[CidrMatch]") {
  REQUIRE_FALSE(cidrContains("203.0.113.0/24", "2001:db8::1"));
  REQUIRE_FALSE(cidrContains("2001:db8::/32", "203.0.113.5"));
}

TEST_CASE("cidrContains: malformed input returns false, never throws", "[CidrMatch]") {
  REQUIRE_NOTHROW(cidrContains("not-an-ip", "203.0.113.5"));
  REQUIRE_FALSE(cidrContains("not-an-ip", "203.0.113.5"));
  REQUIRE_FALSE(cidrContains("203.0.113.0/33", "203.0.113.1"));    // prefix out of range
  REQUIRE_FALSE(cidrContains("203.0.113.0/-1", "203.0.113.1"));    // negative, unparseable as uint context
  REQUIRE_FALSE(cidrContains("203.0.113.0/abc", "203.0.113.1"));   // non-numeric prefix
  REQUIRE_FALSE(cidrContains("203.0.113.0/24trailing", "203.0.113.1"));  // trailing garbage
  REQUIRE_FALSE(cidrContains("", "203.0.113.1"));
  REQUIRE_FALSE(cidrContains("203.0.113.0/24", "not-an-ip"));
}
