#include "vault/FrontMatter.h"

#include <catch2/catch_test_macros.hpp>
#include <yaml-cpp/yaml.h>

using wikicore::vault::FrontMatter;
using wikicore::vault::parseForImport;
using wikicore::vault::parseFrontMatter;
using wikicore::vault::serializeFrontMatter;
using wikicore::vault::replaceBody;
using wikicore::vault::withFrontMatterId;

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
  for (const std::string& content :
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

TEST_CASE("replaceFrontMatterId rewrites only the top-level id line",
          "[FrontMatter]") {
  using wikicore::vault::replaceFrontMatterId;
  const std::string newId = "99999999-2222-4333-8444-555555555555";

  SECTION("every other byte of the file is kept") {
    const std::string raw =
        "---\ntitle: Note\nid: old-id\naliases: [x, y]\nmeta:\n  id: nested\n---\nbody id: old-id\n";
    REQUIRE(replaceFrontMatterId(raw, newId) ==
            "---\ntitle: Note\nid: " + newId +
                "\naliases: [x, y]\nmeta:\n  id: nested\n---\nbody id: old-id\n");
  }
  SECTION("CRLF line endings survive") {
    REQUIRE(replaceFrontMatterId("---\r\nid: \"old-id\"\r\n---\r\nbody\r\n", newId) ==
            "---\r\nid: " + newId + "\r\n---\r\nbody\r\n");
  }
  SECTION("front matter it cannot rewrite in place is refused") {
    for (const std::string& raw :
         {std::string("no front matter, id: old-id\n"),
          std::string("---\n{id: old-id, title: Flow}\n---\nbody\n"),
          std::string("---\nid: >\n  folded-old-id\n---\nbody\n"),
          std::string("---\ntitle: No id here\n---\nbody\n")}) {
      CAPTURE(raw);
      REQUIRE_FALSE(replaceFrontMatterId(raw, newId).has_value());
    }
  }
}

TEST_CASE("serializeFrontMatter keeps keys of the existing file it does not model",
          "[FrontMatter]") {
  FrontMatter fm;
  fm.id = "doc-1";
  fm.title = "New title";
  fm.visibility = "public";

  SECTION("a file with only modeled keys serializes exactly as without one") {
    const std::string existing = "---\nid: doc-1\ntitle: Old\nvisibility: public\n---\nold";
    REQUIRE(serializeFrontMatter(fm, "body", existing) == serializeFrontMatter(fm, "body"));
  }
  SECTION("unmodeled keys follow the modeled ones, modeled keys come from fm") {
    const std::string existing =
        "---\ntitle: Old\naliases: [one, two]\nmeta:\n  source: obsidian\n---\nold";
    const std::string out = serializeFrontMatter(fm, "body", existing);
    const std::string base = serializeFrontMatter(fm, "body");
    const std::string baseBlock = base.substr(0, base.rfind("\n---\n"));
    REQUIRE(out.rfind(baseBlock, 0) == 0);
    const auto parsed = parseFrontMatter(out);
    REQUIRE(parsed.frontMatter.title == "New title");
    REQUIRE(parsed.body == "body");
    const YAML::Node block = YAML::Load(out.substr(4, out.rfind("\n---\n") - 4));
    REQUIRE(block["aliases"].as<std::vector<std::string>>() ==
            std::vector<std::string>{"one", "two"});
    REQUIRE(block["meta"]["source"].as<std::string>() == "obsidian");
  }
  SECTION("an existing file without usable front matter contributes nothing") {
    for (const std::string& existing :
         {std::string("plain text"), std::string("---\ntitle: [unclosed\n---\nx")}) {
      CAPTURE(existing);
      REQUIRE(serializeFrontMatter(fm, "body", existing) == serializeFrontMatter(fm, "body"));
    }
  }
}

TEST_CASE("replaceBody keeps the front-matter block byte for byte", "[FrontMatter]") {
  REQUIRE(replaceBody("---\r\n# note\r\ntitle: A\r\n---\r\nold\r\n", "new\r\n") ==
          "---\r\n# note\r\ntitle: A\r\n---\r\nnew\r\n");
  REQUIRE(replaceBody("no front matter", "new") == "new");
  REQUIRE(replaceBody("---\ntitle: [unclosed\n---\nold", "new") == "new");
}

TEST_CASE("withFrontMatterId replaces an id line or adds one", "[FrontMatter]") {
  const std::string newId = "99999999-2222-4333-8444-555555555555";
  REQUIRE(withFrontMatterId("---\nid: old\ntitle: A\n---\nbody", newId) ==
          "---\nid: " + newId + "\ntitle: A\n---\nbody");
  REQUIRE(withFrontMatterId("---\r\ntitle: A\r\n---\r\nbody", newId) ==
          "---\r\nid: " + newId + "\r\ntitle: A\r\n---\r\nbody");
  for (const std::string& raw :
       {std::string("no front matter"), std::string("---\n{title: Flow}\n---\nbody"),
        std::string("---\ntitle: [unclosed\n---\nbody")}) {
    CAPTURE(raw);
    REQUIRE_FALSE(withFrontMatterId(raw, newId).has_value());
  }
}

TEST_CASE("FrontMatter description: parsed, written only when non-empty, round-trips",
          "[FrontMatter]") {
  const auto parsed = parseFrontMatter(
      "---\ntitle: T\ntype: skill\ndescription: \"Use when: a # b\"\n---\nbody\n");
  REQUIRE(parsed.frontMatter.description == "Use when: a # b");

  FrontMatter fm = parsed.frontMatter;
  const std::string withDescription = serializeFrontMatter(fm, "body\n");
  REQUIRE(parseFrontMatter(withDescription).frontMatter.description == "Use when: a # b");

  fm.description.clear();
  const std::string withoutDescription = serializeFrontMatter(fm, "body\n");
  REQUIRE(withoutDescription.find("description") == std::string::npos);
}

TEST_CASE("parseForImport reads description only when the key is present", "[FrontMatter]") {
  REQUIRE(parseForImport("---\ndescription: Use when X\n---\nb\n").fields.description ==
          std::string("Use when X"));
  REQUIRE_FALSE(parseForImport("---\ntitle: T\n---\nb\n").fields.description.has_value());
}
