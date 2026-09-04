#include "util/YouTubeEmbed.h"

#include <catch2/catch_test_macros.hpp>

using namespace wikicore::util;

TEST_CASE("extractYouTubeVideoId: watch?v= URLs, with and without extra params",
          "[YouTubeEmbed]") {
  REQUIRE(extractYouTubeVideoId("https://www.youtube.com/watch?v=ofPgFuP7W3E") ==
          "ofPgFuP7W3E");
  REQUIRE(extractYouTubeVideoId("https://youtube.com/watch?v=ofPgFuP7W3E") == "ofPgFuP7W3E");
  REQUIRE(extractYouTubeVideoId("http://www.youtube.com/watch?v=ofPgFuP7W3E") ==
          "ofPgFuP7W3E");
  REQUIRE(extractYouTubeVideoId("https://m.youtube.com/watch?v=ofPgFuP7W3E") ==
          "ofPgFuP7W3E");
  // v= not first, and not last, in the query string.
  REQUIRE(extractYouTubeVideoId(
              "https://www.youtube.com/watch?si=abc123&v=ofPgFuP7W3E&t=42s") ==
          "ofPgFuP7W3E");
}

TEST_CASE("extractYouTubeVideoId: youtu.be short links", "[YouTubeEmbed]") {
  REQUIRE(extractYouTubeVideoId("https://youtu.be/ofPgFuP7W3E") == "ofPgFuP7W3E");
  REQUIRE(extractYouTubeVideoId("https://youtu.be/ofPgFuP7W3E?t=10") == "ofPgFuP7W3E");
}

TEST_CASE("extractYouTubeVideoId: Shorts links, with share-sheet query params",
          "[YouTubeEmbed]") {
  REQUIRE(extractYouTubeVideoId("https://www.youtube.com/shorts/hJMwZCdRFTE?feature=share") ==
          "hJMwZCdRFTE");
  REQUIRE(extractYouTubeVideoId("https://youtube.com/shorts/hJMwZCdRFTE") == "hJMwZCdRFTE");
}

TEST_CASE("extractYouTubeVideoId: already-embed URLs pass through too", "[YouTubeEmbed]") {
  REQUIRE(extractYouTubeVideoId("https://www.youtube.com/embed/ofPgFuP7W3E") ==
          "ofPgFuP7W3E");
}

TEST_CASE("extractYouTubeVideoId: rejects anything not recognized", "[YouTubeEmbed]") {
  REQUIRE_FALSE(extractYouTubeVideoId("https://example.com/watch?v=ofPgFuP7W3E"));
  REQUIRE_FALSE(extractYouTubeVideoId("https://www.youtube.com/watch"));  // no query at all
  REQUIRE_FALSE(extractYouTubeVideoId("https://www.youtube.com/watch?t=10"));  // no v=
  REQUIRE_FALSE(extractYouTubeVideoId("not a url"));
  REQUIRE_FALSE(extractYouTubeVideoId(""));
  // A path that merely starts with "watch" but isn't the watch endpoint.
  REQUIRE_FALSE(extractYouTubeVideoId("https://youtube.com/watch_later?v=ofPgFuP7W3E"));
}

TEST_CASE("extractYouTubeVideoId: rejects IDs that aren't exactly 11 characters",
          "[YouTubeEmbed]") {
  REQUIRE_FALSE(extractYouTubeVideoId("https://youtu.be/short"));         // too short
  REQUIRE_FALSE(extractYouTubeVideoId("https://youtu.be/wayTooLongToBeAnID"));  // too long
}

TEST_CASE("rewriteYouTubeEmbeds: recognized URL becomes the internal marker",
          "[YouTubeEmbed]") {
  const std::string out =
      rewriteYouTubeEmbeds("Before.\n\n![youtube](https://youtu.be/ofPgFuP7W3E)\n\nAfter.");
  REQUIRE(out == "Before.\n\n![](youtube-embed:ofPgFuP7W3E)\n\nAfter.");
}

TEST_CASE("rewriteYouTubeEmbeds: alt text match is case-insensitive", "[YouTubeEmbed]") {
  const std::string out = rewriteYouTubeEmbeds("![YouTube](https://youtu.be/ofPgFuP7W3E)");
  REQUIRE(out == "![](youtube-embed:ofPgFuP7W3E)");
}

TEST_CASE("rewriteYouTubeEmbeds: unrecognized URL is left completely untouched",
          "[YouTubeEmbed]") {
  const std::string original = "![youtube](https://example.com/not-a-video.png)";
  REQUIRE(rewriteYouTubeEmbeds(original) == original);
}

TEST_CASE("rewriteYouTubeEmbeds: a normal image with different alt text is untouched",
          "[YouTubeEmbed]") {
  const std::string original = "![a cat](https://example.com/cat.png)";
  REQUIRE(rewriteYouTubeEmbeds(original) == original);
}

TEST_CASE("rewriteYouTubeEmbeds: handles multiple embeds in one document", "[YouTubeEmbed]") {
  const std::string out = rewriteYouTubeEmbeds(
      "![youtube](https://youtu.be/ofPgFuP7W3E)\n\n"
      "![youtube](https://www.youtube.com/shorts/hJMwZCdRFTE?feature=share)");
  REQUIRE(out ==
          "![](youtube-embed:ofPgFuP7W3E)\n\n"
          "![](youtube-embed:hJMwZCdRFTE)");
}
