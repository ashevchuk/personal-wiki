#include "llm/ChatStreamParser.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using wikicore::llm::ChatStreamParser;

TEST_CASE("ChatStreamParser: concatenates content deltas", "[ChatStreamParser]") {
  ChatStreamParser parser;
  std::string seen;
  auto onDelta = [&](std::string_view chunk) { seen.append(chunk); };
  parser.feed(
      "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
      "data: [DONE]\n\n",
      onDelta);
  const auto out = parser.finish(onDelta);
  REQUIRE(out.content == "Hello");
  REQUIRE(seen == "Hello");
  REQUIRE(parser.receivedDone());
}

TEST_CASE("ChatStreamParser: assembles streamed tool_calls by index",
          "[ChatStreamParser]") {
  ChatStreamParser parser;
  parser.feed(
      "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":"
      "\"call_1\",\"function\":{\"name\":\"search_documents\",\"arguments\":\"\"}}]}}]}\n\n",
      {});
  parser.feed(
      "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":"
      "{\"arguments\":\"{\\\"query\\\":\"}}]}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":"
      "{\"arguments\":\"\\\"cpp\\\"}\"}}]}}]}\n\n"
      "data: {\"choices\":[{\"finish_reason\":\"tool_calls\"}]}\n\n",
      {});
  const auto out = parser.finish({});
  REQUIRE(out.toolCalls.size() == 1);
  REQUIRE(out.toolCalls[0].id == "call_1");
  REQUIRE(out.toolCalls[0].name == "search_documents");
  REQUIRE(out.toolCalls[0].arguments == R"({"query":"cpp"})");
  REQUIRE(out.finishReason == "tool_calls");
}

TEST_CASE("ChatStreamParser: splits across feed() calls in the middle of an event",
          "[ChatStreamParser]") {
  ChatStreamParser parser;
  std::string seen;
  auto onDelta = [&](std::string_view chunk) { seen.append(chunk); };
  parser.feed("data: {\"choices\":[{\"delta\":{\"content\":\"ab", onDelta);
  REQUIRE(seen.empty());
  parser.feed("c\"}}]}\n\n", onDelta);
  REQUIRE(seen == "abc");
  const auto out = parser.finish(onDelta);
  REQUIRE(out.content == "abc");
}

TEST_CASE("ChatStreamParser: accepts CRLF separators", "[ChatStreamParser]") {
  ChatStreamParser parser;
  parser.feed("data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\r\n\r\n", {});
  const auto out = parser.finish({});
  REQUIRE(out.content == "x");
}

TEST_CASE("ChatStreamParser: JSON object fallback when the provider ignores stream",
          "[ChatStreamParser]") {
  ChatStreamParser parser;
  std::string seen;
  auto onDelta = [&](std::string_view chunk) { seen.append(chunk); };
  parser.feed(
      R"({"choices":[{"message":{"role":"assistant","content":"plain"},)"
      R"("finish_reason":"stop"}]})",
      onDelta);
  const auto out = parser.finish(onDelta);
  REQUIRE(out.content == "plain");
  REQUIRE(seen == "plain");
  REQUIRE(out.finishReason == "stop");
}

TEST_CASE("ChatStreamParser: JSON error object throws", "[ChatStreamParser]") {
  ChatStreamParser parser;
  parser.feed(R"({"error":{"message":"nope"}})", {});
  REQUIRE_THROWS_WITH(parser.finish({}), Catch::Matchers::ContainsSubstring("nope"));
}
