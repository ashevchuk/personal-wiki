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

TEST_CASE("renderMarkdownToHtml: a ```query fenced block becomes pre.query, "
          "raw DSL text preserved for the client to send to /api/query",
          "[MarkdownRenderer]") {
  const std::string html = renderMarkdownToHtml("```query\ntag: cpp\nlimit: 5\n```");
  REQUIRE(html.find("<pre class=\"query\">") != std::string::npos);
  REQUIRE(html.find("<code class=\"language-query\">") == std::string::npos);
  REQUIRE(html.find("tag: cpp") != std::string::npos);
  REQUIRE(html.find("limit: 5") != std::string::npos);
}

TEST_CASE("renderMarkdownToHtml: a ```circuit fenced block becomes a view-only "
          "circuitjs1 iframe with the circuit XML in a data attribute",
          "[MarkdownRenderer]") {
  const std::string html = renderMarkdownToHtml("```circuit\n<cir f=\"1\">\n</cir>\n```");
  REQUIRE(html.find("<iframe class=\"circuit-embed\"") != std::string::npos);
  REQUIRE(html.find("src=\"js/circuitjs1/circuitjs.html?") != std::string::npos);
  REQUIRE(html.find("hideSidebar=true") != std::string::npos);
  REQUIRE(html.find("hideMenu=true") != std::string::npos);
  REQUIRE(html.find("hideInfoBox=true") != std::string::npos);
  REQUIRE(html.find("editable=false") != std::string::npos);
  REQUIRE(html.find("running=true") != std::string::npos);
  // The URL itself never carries a cct param -- static/js/circuit-embed.js
  // loads the circuit via CircuitJS1.importCircuit() instead (see
  // substituteCircuitBlocks' own comment on why: circuitjs1's own
  // query-string decoder never decodes '=' or '/', both of which this
  // XML format's attribute syntax needs).
  REQUIRE(html.find("cct=") == std::string::npos);
  // The circuit XML lands in data-circuit-xml, still HTML-escaped exactly
  // as md4c left it -- valid for an HTML attribute as-is, same reasoning
  // as substituteMermaidBlocks/substituteQueryBlocks copying their own
  // content through untouched.
  REQUIRE(html.find("data-circuit-xml=\"&lt;cir f=&quot;1&quot;&gt;") != std::string::npos);
  REQUIRE(html.find("<cir") == std::string::npos);
  // The intermediate md4c shape must never leak into the final output --
  // same discipline as the mermaid/query tests above.
  REQUIRE(html.find("<code class=\"language-circuit\">") == std::string::npos);
  REQUIRE(html.find("<pre><code") == std::string::npos);
}

TEST_CASE("renderMarkdownToHtml: a ```circuit block containing characters md4c "
          "HTML-escapes lands in data-circuit-xml still escaped",
          "[MarkdownRenderer]") {
  const std::string html = renderMarkdownToHtml("```circuit\n<r x=\"1 & 2\"/>\n```");
  REQUIRE(html.find("data-circuit-xml=\"&lt;r x=&quot;1 &amp; 2&quot;/&gt;") !=
          std::string::npos);
}
