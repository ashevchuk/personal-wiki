#include "vault/FrontMatter.h"

#include <catch2/catch_test_macros.hpp>

using wikicore::vault::FrontMatter;
using wikicore::vault::parseForImport;
using wikicore::vault::parseFrontMatter;
using wikicore::vault::serializeFrontMatter;

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

TEST_CASE("parseForImport reads back whatever serializeFrontMatter writes",
          "[FrontMatter][import]") {
  const std::vector<std::string> values = {
      "Plain title", "Note: it's here", "'quoted'", "say \"hi\"", "back\\slash",
      "line1\nline2", "tab\there", "ctl\x01x", "# not a comment", " leading space",
      "trailing space ", "yes", "null", "~", "123", "a, b", "x]y", "[x]", "---",
      "key: value", "it''s", "a\\\"b'c", "ünïcode — dash ✓", "emoji 😀"};
  for (const auto& value : values) {
    CAPTURE(value);
    FrontMatter fm;
    fm.id = "id-1";
    fm.title = value;
    fm.type = value;
    fm.tags = {value, "plain"};
    fm.visibility = "public";

    const auto imported = parseForImport(serializeFrontMatter(fm, "body\n"));

    REQUIRE(imported.fields.title == value);
    REQUIRE(imported.fields.type == value);
    REQUIRE(imported.fields.tags == std::vector<std::string>{value, "plain"});
    REQUIRE(imported.fields.visibility == "public");
    REQUIRE(imported.body == "body\n");
  }
}

TEST_CASE("parseForImport reads front matter written by other tools",
          "[FrontMatter][import]") {
  SECTION("single-quoted scalar with a doubled apostrophe") {
    const auto imported = parseForImport("---\ntitle: 'Note: it''s here'\n---\nBody\n");
    REQUIRE(imported.fields.title == "Note: it's here");
    REQUIRE(imported.body == "Body\n");
  }
  SECTION("CRLF line endings") {
    const auto imported =
        parseForImport("---\r\ntitle: Hello\r\ntags: [a]\r\n---\r\nBody\r\n");
    REQUIRE(imported.fields.title == "Hello");
    REQUIRE(imported.fields.tags == std::vector<std::string>{"a"});
    REQUIRE(imported.body == "Body\r\n");
  }
  SECTION("block-style tag list") {
    const auto imported = parseForImport("---\ntags:\n  - a\n  - b\n---\nBody\n");
    REQUIRE(imported.fields.tags == std::vector<std::string>{"a", "b"});
  }
  SECTION("'...' closes the block") {
    const auto imported = parseForImport("---\ntitle: T\n...\nBody\n");
    REQUIRE(imported.fields.title == "T");
    REQUIRE(imported.body == "Body\n");
  }
}

TEST_CASE("parseForImport distinguishes absent keys from empty ones",
          "[FrontMatter][import]") {
  SECTION("keys missing from the block stay unset") {
    const auto imported = parseForImport("---\ntitle: Only\n---\nBody\n");
    REQUIRE(imported.fields.title == "Only");
    REQUIRE_FALSE(imported.fields.type.has_value());
    REQUIRE_FALSE(imported.fields.visibility.has_value());
    REQUIRE_FALSE(imported.fields.due.has_value());
    REQUIRE_FALSE(imported.fields.recur.has_value());
    REQUIRE_FALSE(imported.fields.tags.has_value());
  }
  SECTION("a key with an empty value is set to empty") {
    const auto imported = parseForImport("---\ntitle:\ntags:\n---\nBody\n");
    REQUIRE(imported.fields.title == "");
    REQUIRE(imported.fields.tags == std::vector<std::string>{});
  }
  SECTION("a tags value that is not a list leaves tags unset") {
    const auto imported = parseForImport("---\ntags: just-one\n---\nBody\n");
    REQUIRE_FALSE(imported.fields.tags.has_value());
  }
  SECTION("visibility other than exactly 'public' collapses to private") {
    const auto imported = parseForImport("---\nvisibility: PUBLIC\n---\nBody\n");
    REQUIRE(imported.fields.visibility == "private");
  }
}

TEST_CASE("parseForImport treats a file without usable front matter as pure body",
          "[FrontMatter][import]") {
  for (const std::string content :
       {std::string("Just text.\n"), std::string("---\ntitle: [unclosed\n---\nBody\n"),
        std::string("---\n- a\n- b\n---\nBody\n")}) {
    CAPTURE(content);
    const auto imported = parseForImport(content);
    REQUIRE(imported.body == content);
    REQUIRE_FALSE(imported.fields.title.has_value());
    REQUIRE_FALSE(imported.fields.tags.has_value());
    REQUIRE_FALSE(imported.fields.visibility.has_value());
  }
}
