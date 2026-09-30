#include "util/LinkItems.h"

#include <catch2/catch_test_macros.hpp>

using namespace wikicore::util;

TEST_CASE("extractExternalLinks finds http(s) markdown links", "[LinkItems]") {
  const auto items = extractExternalLinks(
      "- [Anthropic Docs](https://docs.anthropic.com/) -- API reference\n"
      "- [cppreference](http://cppreference.com) -- the classic\n");
  REQUIRE(items.size() == 2);
  REQUIRE(items[0].label == "Anthropic Docs");
  REQUIRE(items[0].url == "https://docs.anthropic.com/");
  REQUIRE(items[1].label == "cppreference");
  REQUIRE(items[1].url == "http://cppreference.com");
}

TEST_CASE("extractExternalLinks ignores internal/relative links", "[LinkItems]") {
  const auto items = extractExternalLinks(
      "[attachment](assets/photo.png) and [another note](d/other.md) and "
      "[anchor](#section)\n");
  REQUIRE(items.empty());
}

TEST_CASE("extractExternalLinks ignores image syntax but still finds a real "
          "link later on the same line",
          "[LinkItems]") {
  const auto items = extractExternalLinks(
      "![diagram](https://example.com/diagram.png) see also "
      "[the site](https://example.com/)\n");
  REQUIRE(items.size() == 1);
  REQUIRE(items[0].label == "the site");
  REQUIRE(items[0].url == "https://example.com/");
}

TEST_CASE("extractExternalLinks finds multiple links on one line", "[LinkItems]") {
  const auto items =
      extractExternalLinks("[a](https://a.example/) and [b](https://b.example/)\n");
  REQUIRE(items.size() == 2);
  REQUIRE(items[0].url == "https://a.example/");
  REQUIRE(items[1].url == "https://b.example/");
}

TEST_CASE("extractExternalLinks on plain text with no links returns empty",
          "[LinkItems]") {
  REQUIRE(extractExternalLinks("Just a normal paragraph, no links here.").empty());
}

TEST_CASE("extractExternalLinks does not hang or throw on an unterminated bracket",
          "[LinkItems]") {
  REQUIRE_NOTHROW(extractExternalLinks("[oops forgot to close this"));
  REQUIRE(extractExternalLinks("[oops forgot to close this").empty());
}
