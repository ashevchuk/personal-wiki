#pragma once

#include <string>
#include <vector>

namespace wikicore::embeddings {

// Passage-window size for multi-vector embeddings. Chosen to sit
// comfortably under typical local embedding-model context windows
// (bge-small-en-v1.5 is 512 tokens) after a small special-token reserve
// and a title prefix on every chunk. See chunkForEmbedding().
inline constexpr int kEmbeddingChunkTargetTokens = 384;
inline constexpr int kEmbeddingChunkOverlapTokens = 64;
inline constexpr int kEmbeddingChunkMaxChunks = 32;
// Conservative bytes-per-token for mixed English/Cyrillic against
// BERT-family BPE: Cyrillic often tokenizes near 1 token per character
// (2 UTF-8 bytes), so 2 bytes/token keeps a packed chunk under a 512
// token n_ctx. English is over-estimated (smaller chunks), which is
// the safe direction — never the other way.
inline constexpr int kEmbeddingChunkBytesPerToken = 2;
inline constexpr int kEmbeddingChunkSpecialTokenReserve = 8;

// Splits title+body into overlapping passage windows, each prefixed with
// the document title so a chunk still carries document identity when
// compared to a query that names the topic rather than a buried
// paragraph.
//
// Token counts are ESTIMATED from UTF-8 byte length (see
// kEmbeddingChunkBytesPerToken), not the model's real tokenizer —
// EmbeddingProvider stays tokenizer-agnostic, and a remaining oversized
// chunk is still rejected by LocalEmbeddingProvider::embed() as a
// catchable failure rather than a process crash. `maxInputTokens` is
// the model's n_ctx (0 = unknown; uses kEmbeddingChunkTargetTokens).
//
// Short documents produce a single chunk identical to the old
// `title + "\n\n" + body` string IndexUpdater used to embed as one
// vector — same observable for anything that already fit in the window.
std::vector<std::string> chunkForEmbedding(const std::string& title,
                                           const std::string& body,
                                           int maxInputTokens = 0);

}  // namespace wikicore::embeddings
