#include "util/TodoItems.h"

#include <catch2/catch_test_macros.hpp>

using namespace wikicore::util;

TEST_CASE("extractTodoItems finds open and checked GFM task-list lines", "[TodoItems]") {
  const auto items = extractTodoItems(
      "# Notes\n"
      "\n"
      "- [ ] buy milk\n"
      "- [x] pay rent\n"
      "- [X] renew passport\n"
      "\n"
      "Not a task line.\n");
  REQUIRE(items.size() == 3);
  REQUIRE_FALSE(items[0].checked);
  REQUIRE(items[0].text == "buy milk");
  REQUIRE(items[1].checked);
  REQUIRE(items[1].text == "pay rent");
  REQUIRE(items[2].checked);  // uppercase X counts too
  REQUIRE(items[2].text == "renew passport");
}

TEST_CASE("extractTodoItems records 1-based line numbers", "[TodoItems]") {
  const auto items = extractTodoItems("line one\nline two\n- [ ] third line\n");
  REQUIRE(items.size() == 1);
  REQUIRE(items[0].lineNo == 3);
}

TEST_CASE("extractTodoItems accepts *, -, and + list markers", "[TodoItems]") {
  const auto items = extractTodoItems("- [ ] dash\n* [ ] star\n+ [ ] plus\n");
  REQUIRE(items.size() == 3);
}

TEST_CASE("extractTodoItems ignores malformed or empty checkbox lines", "[TodoItems]") {
  const auto items = extractTodoItems(
      "- [] missing the space\n"
      "-[ ] missing the space after the marker\n"
      "- [ ]\n"          // no text after it
      "- [y] not a valid state\n"
      "plain paragraph text\n");
  REQUIRE(items.empty());
}

TEST_CASE("extractTodoItems trims trailing whitespace and \\r from the text",
          "[TodoItems]") {
  const auto items = extractTodoItems("- [ ] trailing spaces   \r\n");
  REQUIRE(items.size() == 1);
  REQUIRE(items[0].text == "trailing spaces");
}

TEST_CASE("extractTodoItems on text with no checkboxes returns empty", "[TodoItems]") {
  REQUIRE(extractTodoItems("Just a normal document, no tasks here.").empty());
}
