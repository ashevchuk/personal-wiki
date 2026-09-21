#include "util/Base64.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using wikicore::util::decodeBase64;
using wikicore::util::encodeBase64;
using wikicore::util::stripDataUrlPrefix;

TEST_CASE("encodeBase64 / decodeBase64 round-trip including binary NULs",
          "[Base64]") {
  const std::string raw{"hello\0world\xff\x00", 13};
  const auto encoded = encodeBase64(raw);
  const auto decoded = decodeBase64(encoded);
  REQUIRE(decoded.has_value());
  REQUIRE(*decoded == raw);
}

TEST_CASE("encodeBase64 matches RFC 4648 vectors", "[Base64]") {
  REQUIRE(encodeBase64("") == "");
  REQUIRE(encodeBase64("f") == "Zg==");
  REQUIRE(encodeBase64("fo") == "Zm8=");
  REQUIRE(encodeBase64("foo") == "Zm9v");
  REQUIRE(encodeBase64("foob") == "Zm9vYg==");
  REQUIRE(encodeBase64("fooba") == "Zm9vYmE=");
  REQUIRE(encodeBase64("foobar") == "Zm9vYmFy");
}

TEST_CASE("decodeBase64 accepts whitespace wrapping and URL-safe alphabet",
          "[Base64]") {
  REQUIRE(decodeBase64("Zm9 v\nYmFy") == std::string("foobar"));
  // URL-safe: '+' -> '-', '/' would appear in other payloads
  const std::string withPlus = encodeBase64(std::string("\xfb\xff", 2));
  REQUIRE(withPlus.find('+') != std::string::npos);
  std::string urlSafe = withPlus;
  for (char& c : urlSafe) {
    if (c == '+') c = '-';
    if (c == '/') c = '_';
  }
  REQUIRE(decodeBase64(urlSafe) == decodeBase64(withPlus));
}

TEST_CASE("decodeBase64 rejects truncated or garbage input", "[Base64]") {
  REQUIRE_FALSE(decodeBase64("Z").has_value());       // 1 leftover sextet
  REQUIRE_FALSE(decodeBase64("@@@@").has_value());    // invalid chars
  REQUIRE_FALSE(decodeBase64("Zm9v====").has_value());  // too much padding
  REQUIRE_FALSE(decodeBase64("Zm9v=YmE=").has_value());  // pad then data
}

TEST_CASE("stripDataUrlPrefix peels a data URL and leaves plain base64 alone",
          "[Base64]") {
  REQUIRE(stripDataUrlPrefix("Zm9v") == "Zm9v");
  REQUIRE(stripDataUrlPrefix("data:image/png;base64,QUJD") == "QUJD");
  REQUIRE(stripDataUrlPrefix("DATA:application/octet-stream;BASE64,QUJD") == "QUJD");
}
