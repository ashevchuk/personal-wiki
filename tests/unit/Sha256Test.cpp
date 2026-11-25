#include "util/Sha256.h"

#include "auth/Crypto.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using wikicore::util::sha256Hex;

TEST_CASE("sha256Hex matches FIPS 180-4 / NIST vectors", "[Sha256]") {
  REQUIRE(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  REQUIRE(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // 56 bytes: the padding needs a second block.
  REQUIRE(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  REQUIRE(sha256Hex(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256Hex matches the Skills extension spec's example files", "[Sha256]") {
  // invoice.md / purchase-order.md from ext-skills specification/beta/skills.mdx.
  REQUIRE(sha256Hex("# Invoice\n\nCustomer:\nAmount:\n") ==
          "61f4ea6d2c75fde1b4977219e7e3107d491c3c26aefb6686e84d6281c088d9ee");
  REQUIRE(sha256Hex("# Purchase order\n\nSupplier:\nItems:\n") ==
          "f2ff774b1737ff3dec81c47946f9976f18a1a9f69dda0a81f22eabd95173c158");
}

TEST_CASE("sha256Hex agrees with OpenSSL for every length around the block boundaries",
          "[Sha256]") {
  for (std::size_t len = 0; len <= 200; ++len) {
    std::string data;
    for (std::size_t i = 0; i < len; ++i) data += static_cast<char>((i * 37 + len) & 0xff);
    REQUIRE(sha256Hex(data) == wikicore::auth::sha256Hex(data));
  }
}
