#include "util/MarkdownRenderer.h"

#include <catch2/catch_test_macros.hpp>

using namespace wikicore::util;

TEST_CASE("renderMarkdownToHtml: ordinary markdown renders as expected", "[MarkdownRenderer]") {
  const std::string html = renderMarkdownToHtml("# Title\n\nSome **bold** text.");
  REQUIRE(html.find("<h1>Title</h1>") != std::string::npos);
  REQUIRE(html.find("<strong>bold</strong>") != std::string::npos);
}

TEST_CASE("renderMarkdownToHtml: a recognized YouTube embed becomes a real iframe",
          "[MarkdownRenderer]") {
  const std::string html = renderMarkdownToHtml("![youtube](https://youtu.be/ofPgFuP7W3E)");
  REQUIRE(html.find("<iframe") != std::string::npos);
  REQUIRE(html.find("src=\"https://www.youtube.com/embed/ofPgFuP7W3E\"") != std::string::npos);
  REQUIRE(html.find("class=\"youtube-embed\"") != std::string::npos);
  // The intermediate marker must never leak into the final output.
  REQUIRE(html.find("youtube-embed:") == std::string::npos);
  REQUIRE(html.find("<img") == std::string::npos);
}

TEST_CASE("renderMarkdownToHtml: an unrecognized URL stays a plain, unembedded <img>",
          "[MarkdownRenderer]") {
  const std::string html = renderMarkdownToHtml("![youtube](https://example.com/x.png)");
  REQUIRE(html.find("<iframe") == std::string::npos);
  REQUIRE(html.find("<img src=\"https://example.com/x.png\"") != std::string::npos);
}

TEST_CASE("renderMarkdownToHtml: hand-typed HTML that mimics the internal marker is "
          "escaped, never turned into a real iframe",
          "[MarkdownRenderer]") {
  // A document body literally containing what LOOKS like the marker
  // shape this renderer produces internally -- must never be trusted as
  // if it came from rewriteYouTubeEmbeds. MD_FLAG_NOHTMLSPANS is what
  // actually guarantees this (see MarkdownRenderer.cpp's own comment);
  // this test exists so a future change to that flag would be caught
  // here, not discovered as a live XSS.
  const std::string html =
      renderMarkdownToHtml("<img src=\"youtube-embed:AAAAAAAAAAA\" alt=\"\">");
  REQUIRE(html.find("<iframe") == std::string::npos);
  REQUIRE(html.find("&lt;img") != std::string::npos);
}

TEST_CASE("renderMarkdownToHtml: a normal image (not the youtube sentinel) renders as "
          "a plain <img>",
          "[MarkdownRenderer]") {
  const std::string html = renderMarkdownToHtml("![a cat](https://example.com/cat.png)");
  REQUIRE(html.find("<iframe") == std::string::npos);
  REQUIRE(html.find("<img src=\"https://example.com/cat.png\" alt=\"a cat\">") !=
          std::string::npos);
}

TEST_CASE("renderMarkdownToHtml: a ```mermaid fenced block becomes pre.mermaid, "
          "not a plain code block",
          "[MarkdownRenderer]") {
  const std::string html = renderMarkdownToHtml("```mermaid\ngraph TD;\n  A-->B;\n```");
  REQUIRE(html.find("<pre class=\"mermaid\">") != std::string::npos);
  // The intermediate md4c shape must never leak into the final output --
  // same discipline as the YouTube marker test above.
  REQUIRE(html.find("<code class=\"language-mermaid\">") == std::string::npos);
  // Diagram source is preserved verbatim (still HTML-escaped, as md4c
  // left it -- the browser decodes entities via .textContent before
  // mermaid.js ever parses this).
  REQUIRE(html.find("graph TD;") != std::string::npos);
  REQUIRE(html.find("A--&gt;B;") != std::string::npos);
}

TEST_CASE("renderMarkdownToHtml: an ordinary fenced code block is untouched",
          "[MarkdownRenderer]") {
  const std::string html = renderMarkdownToHtml("```cpp\nint main() {}\n```");
  REQUIRE(html.find("<pre><code class=\"language-cpp\">") != std::string::npos);
  REQUIRE(html.find("pre class=\"mermaid\"") == std::string::npos);
}

TEST_CASE("renderMarkdownToHtml: a fence info string that only STARTS WITH "
          "\"mermaid\" is not mistaken for an exact match",
          "[MarkdownRenderer]") {
  // Guards the exact-match discipline substituteMermaidBlocks documents:
  // a language name that happens to share a prefix with "mermaid" must
  // degrade to an ordinary code block, not a guessed diagram.
  const std::string html = renderMarkdownToHtml("```mermaidjs\nnot a diagram\n```");
  REQUIRE(html.find("pre class=\"mermaid\"") == std::string::npos);
  REQUIRE(html.find("<code class=\"language-mermaidjs\">") != std::string::npos);
}
