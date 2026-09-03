#include "vault/FrontMatter.h"

#include <catch2/catch_test_macros.hpp>

using wikicore::vault::parseFrontMatter;

TEST_CASE("FrontMatter parses a well-formed block", "[FrontMatter]") {
  const auto parsed = parseFrontMatter(
      "---\n"
      "id: doc-1\n"
      "title: Hello\n"
      "tags: [a, b, c]\n"
      "visibility: public\n"
      "type: note\n"
      "created: 2026-01-01T00:00:00Z\n"
      "updated: 2026-01-02T00:00:00Z\n"
      "---\n"
      "Body text.\n");

  REQUIRE(parsed.frontMatter.id == "doc-1");
  REQUIRE(parsed.frontMatter.title == "Hello");
  REQUIRE(parsed.frontMatter.tags == std::vector<std::string>{"a", "b", "c"});
  REQUIRE(parsed.frontMatter.visibility == "public");
  REQUIRE(parsed.frontMatter.type == "note");
  REQUIRE(parsed.frontMatter.created == "2026-01-01T00:00:00Z");
  REQUIRE(parsed.body == "Body text.\n");
}

TEST_CASE("FrontMatter defaults to private when the key is absent",
          "[FrontMatter]") {
  const auto parsed = parseFrontMatter("---\ntitle: No visibility set\n---\nBody\n");
  REQUIRE(parsed.frontMatter.visibility == "private");
}

TEST_CASE("FrontMatter defaults to private for any value other than exactly "
          "'public'",
          "[FrontMatter]") {
  for (const std::string bogus : {"Public", "PUBLIC", "yes", "true", "1", ""}) {
    const auto parsed =
        parseFrontMatter("---\nvisibility: " + bogus + "\n---\nBody\n");
    REQUIRE(parsed.frontMatter.visibility == "private");
  }
}

TEST_CASE("FrontMatter treats content with no '---' block as pure body",
          "[FrontMatter]") {
  const std::string content = "# Just markdown\n\nNo front matter at all.\n";
  const auto parsed = parseFrontMatter(content);
  REQUIRE(parsed.body == content);
  REQUIRE(parsed.frontMatter.visibility == "private");
  REQUIRE(parsed.frontMatter.title.empty());
}

TEST_CASE("FrontMatter falls back to full-content-as-body on malformed YAML",
          "[FrontMatter]") {
  // Bad indentation makes this invalid YAML — must not half-trust it.
  const std::string content =
      "---\n"
      "title: Broken\n"
      "  tags:\n"
      "- not valid\n"
      "---\n"
      "Body\n";
  const auto parsed = parseFrontMatter(content);
  REQUIRE(parsed.frontMatter.visibility == "private");
  // The whole input becomes the body when the block can't be trusted.
  REQUIRE(parsed.body == content);
}

TEST_CASE("FrontMatter handles an empty tags list", "[FrontMatter]") {
  const auto parsed = parseFrontMatter("---\ntitle: No tags\n---\nBody\n");
  REQUIRE(parsed.frontMatter.tags.empty());
}

TEST_CASE("FrontMatter requires the opening delimiter to be the very first "
          "line",
          "[FrontMatter]") {
  const std::string content = "\n---\nvisibility: public\n---\nBody\n";
  const auto parsed = parseFrontMatter(content);
  // Leading blank line means there's no valid front matter at position 0.
  REQUIRE(parsed.body == content);
  REQUIRE(parsed.frontMatter.visibility == "private");
}
