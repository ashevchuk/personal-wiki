#include "auth/Crypto.h"

#include <catch2/catch_test_macros.hpp>

using namespace wikicore::auth;

TEST_CASE("constantTimeEquals: identical strings match", "[Crypto]") {
  REQUIRE(constantTimeEquals("abc123", "abc123"));
}

TEST_CASE("constantTimeEquals: same length, differing content does not match",
          "[Crypto]") {
  REQUIRE_FALSE(constantTimeEquals("abc123", "abc124"));
  // Differ only in the FIRST byte -- the case a naive `==` short-circuit
  // returns fastest on, exactly the timing signal this function exists
  // to not leak.
  REQUIRE_FALSE(constantTimeEquals("zbc123", "abc123"));
}

TEST_CASE("constantTimeEquals: different lengths never match, regardless of "
          "shared prefix",
          "[Crypto]") {
  REQUIRE_FALSE(constantTimeEquals("abc", "abcdef"));
  REQUIRE_FALSE(constantTimeEquals("abcdef", "abc"));
  REQUIRE_FALSE(constantTimeEquals("", "a"));
}

TEST_CASE("constantTimeEquals: two empty strings match", "[Crypto]") {
  REQUIRE(constantTimeEquals("", ""));
}

TEST_CASE("constantTimeEquals: real sha256Hex output round-trips against "
          "itself and rejects a one-character mutation",
          "[Crypto]") {
  const std::string h1 = sha256Hex("some-bearer-token-value");
  const std::string h2 = sha256Hex("some-bearer-token-value");
  const std::string h3 = sha256Hex("some-bearer-token-Value");  // 1 char differs
  REQUIRE(constantTimeEquals(h1, h2));
  REQUIRE_FALSE(constantTimeEquals(h1, h3));
}
