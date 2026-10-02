#include "util/HtmlEscape.h"

#include <catch2/catch_test_macros.hpp>

using namespace wikicore::util;

TEST_CASE("escapeHtml: escapes the four HTML-special characters", "[HtmlEscape]") {
  REQUIRE(escapeHtml("<cir a=\"1 & 2\">") == "&lt;cir a=&quot;1 &amp; 2&quot;&gt;");
}

TEST_CASE("escapeHtml: plain text passes through unchanged", "[HtmlEscape]") {
  REQUIRE(escapeHtml("plain text") == "plain text");
  REQUIRE(escapeHtml("") == "");
}
