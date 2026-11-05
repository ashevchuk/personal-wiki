#include <catch2/catch_test_macros.hpp>

#include "util/Excerpt.h"

namespace {
bool isValidUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
    if (len == 0 || i + len > s.size()) return false;
    for (size_t k = 1; k < len; ++k) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += len;
  }
  return true;
}
}  // namespace

TEST_CASE("plainTextExcerpt never cuts a multi-byte character in half") {
  std::string body;
  for (int i = 0; i < 200; ++i) body += "шифрування ";  // 2-byte characters
  for (size_t maxLen : {239u, 240u, 241u, 100u, 7u}) {
    const std::string excerpt = wikicore::util::plainTextExcerpt(body, maxLen);
    INFO("maxLen=" << maxLen);
    CHECK(isValidUtf8(excerpt));
    CHECK(excerpt.size() <= maxLen + 3);
  }
}

TEST_CASE("plainTextExcerpt leaves short and ASCII text alone") {
  CHECK(wikicore::util::plainTextExcerpt("hello   world\n") == "hello world");
  const std::string ascii(500, 'a');
  CHECK(wikicore::util::plainTextExcerpt(ascii, 240) == std::string(240, 'a') + "...");
}
