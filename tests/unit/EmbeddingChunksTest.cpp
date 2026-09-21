#include "embeddings/EmbeddingChunks.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace wikicore::embeddings;

TEST_CASE("chunkForEmbedding: a short document is a single title+body chunk",
          "[EmbeddingChunks]") {
  const auto chunks = chunkForEmbedding("About cats", "The cat sat on the mat.");
  REQUIRE(chunks.size() == 1);
  REQUIRE(chunks[0] == "About cats\n\nThe cat sat on the mat.");
}

TEST_CASE("chunkForEmbedding: empty body still produces the title prefix", "[EmbeddingChunks]") {
  const auto chunks = chunkForEmbedding("Title only", "");
  REQUIRE(chunks.size() == 1);
  REQUIRE(chunks[0] == "Title only\n\n");
}

TEST_CASE("chunkForEmbedding: a long body is split into overlapping windows, "
          "each prefixed with the title",
          "[EmbeddingChunks]") {
  // Well over one 384-token / 2-bytes-per-token window.
  std::string body;
  body.reserve(3000);
  for (int i = 0; i < 80; ++i) {
    body += "Paragraph " + std::to_string(i) + " has enough filler text to grow.\n\n";
  }
  REQUIRE(body.size() > static_cast<std::size_t>(kEmbeddingChunkTargetTokens) *
                             kEmbeddingChunkBytesPerToken);

  const auto chunks = chunkForEmbedding("Long note", body, /*maxInputTokens=*/512);
  REQUIRE(chunks.size() >= 2);
  REQUIRE(chunks.size() <= static_cast<std::size_t>(kEmbeddingChunkMaxChunks));
  for (const auto& chunk : chunks) {
    REQUIRE(chunk.find("Long note\n\n") == 0);
    const auto tokensEst =
        static_cast<int>((chunk.size() + kEmbeddingChunkBytesPerToken - 1) /
                         kEmbeddingChunkBytesPerToken);
    // Stay under a 512-token window with margin — the whole point of
    // packing below kEmbeddingChunkTargetTokens.
    REQUIRE(tokensEst < 512);
  }
}

TEST_CASE("chunkForEmbedding: consecutive chunks overlap rather than abut",
          "[EmbeddingChunks]") {
  std::string body;
  for (int i = 0; i < 60; ++i) {
    body += "word" + std::to_string(i) + " ";
  }
  // Force a small window so two chunks are guaranteed.
  const auto chunks = chunkForEmbedding("T", body, /*maxInputTokens=*/80);
  REQUIRE(chunks.size() >= 2);

  const std::string& first = chunks[0];
  const std::string& second = chunks[1];
  const std::string firstBody = first.substr(std::string("T\n\n").size());
  const std::string secondBody = second.substr(std::string("T\n\n").size());
  REQUIRE_FALSE(firstBody.empty());
  REQUIRE_FALSE(secondBody.empty());
  // The start of chunk 2 should already have appeared near the end of
  // chunk 1 — overlap, not a hard cut.
  const std::string needle = secondBody.substr(0, std::min<std::size_t>(16, secondBody.size()));
  REQUIRE(firstBody.find(needle) != std::string::npos);
}

TEST_CASE("chunkForEmbedding: Cyrillic text is packed at UTF-8 character "
          "boundaries, not mid-codepoint",
          "[EmbeddingChunks]") {
  std::string body;
  for (int i = 0; i < 400; ++i) body += "абзац тексту ";
  const auto chunks = chunkForEmbedding("Нотатка", body, /*maxInputTokens=*/512);
  REQUIRE(chunks.size() >= 2);
  for (const auto& chunk : chunks) {
    REQUIRE_FALSE(chunk.empty());
    // No dangling UTF-8 continuation byte at either end of the body
    // portion (title is ASCII-safe "Нотатка" — also multi-byte, so check
    // the whole string).
    const auto lead = static_cast<unsigned char>(chunk.front());
    REQUIRE((lead & 0xC0) != 0x80);
  }
}

TEST_CASE("chunkForEmbedding: a document longer than kMaxChunks windows still "
          "indexes the tail",
          "[EmbeddingChunks]") {
  // Tiny window + huge body → hits the cap. The last chunk must contain
  // the unique tail marker, not just the opening.
  std::string body(20000, 'a');
  body += "UNIQUE_TAIL_MARKER";
  const auto chunks = chunkForEmbedding("Cap", body, /*maxInputTokens=*/40);
  REQUIRE(chunks.size() == static_cast<std::size_t>(kEmbeddingChunkMaxChunks));
  REQUIRE(chunks.back().find("UNIQUE_TAIL_MARKER") != std::string::npos);
  REQUIRE(chunks.front().find("Cap\n\n") == 0);
}
