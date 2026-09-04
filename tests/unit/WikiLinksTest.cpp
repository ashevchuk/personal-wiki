#include "util/WikiLinks.h"

#include <catch2/catch_test_macros.hpp>

using namespace wikicore::util;

TEST_CASE("extractWikiLinkTargets normalizes target paths", "[WikiLinks]") {
  const auto targets = extractWikiLinkTargets(
      "See [[notes/foo]] and [[notes/bar.md]] and [[/notes/baz]].");
  REQUIRE(targets.size() == 3);
  REQUIRE(targets[0] == "notes/foo.md");   // .md appended
  REQUIRE(targets[1] == "notes/bar.md");   // already had it, untouched
  REQUIRE(targets[2] == "notes/baz.md");   // leading '/' stripped, .md appended
}

TEST_CASE("extractWikiLinkTargets ignores the label half of [[target|label]]",
          "[WikiLinks]") {
  const auto targets = extractWikiLinkTargets("[[notes/foo|Friendly Name]]");
  REQUIRE(targets.size() == 1);
  REQUIRE(targets[0] == "notes/foo.md");
}

TEST_CASE("extractWikiLinkTargets de-duplicates repeated links", "[WikiLinks]") {
  const auto targets =
      extractWikiLinkTargets("[[notes/foo]] mentioned twice: [[notes/foo]]");
  REQUIRE(targets.size() == 1);
}

TEST_CASE("extractWikiLinkTargets on plain text with no links returns empty",
          "[WikiLinks]") {
  REQUIRE(extractWikiLinkTargets("Just a normal [markdown](link) here.").empty());
}

TEST_CASE("extractWikiLinkTargets does not hang or throw on an unterminated [[",
          "[WikiLinks]") {
  REQUIRE_NOTHROW(extractWikiLinkTargets("Oops, forgot to close this [[notes/foo"));
  REQUIRE(extractWikiLinkTargets("Oops, forgot to close this [[notes/foo").empty());
}

TEST_CASE("rewriteWikiLinksToMarkdownLinks produces plain CommonMark links",
          "[WikiLinks]") {
  const std::string out = rewriteWikiLinksToMarkdownLinks("See [[notes/foo]].");
  REQUIRE(out == "See [notes/foo](d/notes/foo.md).");
}

// A real bug, shipped and caught live: the href used to be the bare
// normalized target ("notes/foo.md") with no "d/" prefix at all — every
// test at the time asserted THAT as correct, because it matched what the
// code produced rather than what the URL actually needed to resolve to
// against shell.html's <base href="{basePath}/">. Document VIEWS are a
// route at /d/{path}, not the vault-relative path itself; the two are
// easy to conflate since both look like "notes/foo.md". This test
// exists specifically so that conflation can't silently come back —
// asserting "starts with d/", not just equality against one fixed
// string, so it stays meaningful even if normalizeTarget's own output
// shape changes later.
TEST_CASE("rewriteWikiLinksToMarkdownLinks hrefs are d/-prefixed, not the bare "
          "vault-relative path",
          "[WikiLinks]") {
  const std::string out = rewriteWikiLinksToMarkdownLinks("[[notes/foo]]");
  const size_t hrefStart = out.find('(');
  REQUIRE(hrefStart != std::string::npos);
  REQUIRE(out.compare(hrefStart + 1, 2, "d/") == 0);
}

TEST_CASE("rewriteWikiLinksToMarkdownLinks honors a custom |label", "[WikiLinks]") {
  const std::string out =
      rewriteWikiLinksToMarkdownLinks("[[notes/foo|Friendly Name]]");
  REQUIRE(out == "[Friendly Name](d/notes/foo.md)");
}

TEST_CASE("rewriteWikiLinksToMarkdownLinks passes non-wiki-link text through untouched",
          "[WikiLinks]") {
  const std::string body = "# Title\n\nA normal [markdown](link) and *emphasis*.";
  REQUIRE(rewriteWikiLinksToMarkdownLinks(body) == body);
}

TEST_CASE("rewriteWikiLinksToMarkdownLinks leaves an unterminated [[ verbatim",
          "[WikiLinks]") {
  const std::string body = "Oops, forgot to close this [[notes/foo";
  REQUIRE(rewriteWikiLinksToMarkdownLinks(body) == body);
}

TEST_CASE("rewriteWikiLinksToMarkdownLinks handles multiple links in one document",
          "[WikiLinks]") {
  const std::string out = rewriteWikiLinksToMarkdownLinks(
      "Start [[a/one]] middle [[b/two|Two]] end.");
  REQUIRE(out == "Start [a/one](d/a/one.md) middle [Two](d/b/two.md) end.");
}
