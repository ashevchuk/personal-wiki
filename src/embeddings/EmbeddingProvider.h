#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace wikicore::embeddings {

// Abstraction over "however we turn text into a vector" — same shape as
// McpToolRegistry hiding cpp-mcp: callers never see a concrete provider
// or its vendored SDK directly. See docs/embeddings.md.
class EmbeddingProvider {
 public:
  virtual ~EmbeddingProvider() = default;

  // Throws on failure — a provider that can't embed `text` right now has
  // nothing useful to return; there's no valid all-zero-vector fallback a
  // caller could safely treat as "similar to nothing". Used for DOCUMENT
  // (passage) text — see embedQuery() below for the other half of an
  // asymmetric retrieval model.
  virtual std::vector<float> embed(const std::string& text) = 0;

  // Embeds a SEARCH QUERY specifically — defaults to embed(text) (correct
  // for a symmetric model, e.g. OpenAI's embeddings, where a query and a
  // passage are embedded identically), but overridable for an asymmetric
  // retrieval model that expects the query and passage sides to be
  // embedded differently. Found live to matter a lot: many small local
  // models (bge family included) are trained with an instruction PREFIX
  // on the query side only ("Represent this sentence for searching
  // relevant passages: ..." for bge) — embedding a bare one-word query
  // the same way a passage is embedded measurably weakens the
  // similarity-score gap between genuinely relevant and irrelevant
  // documents (see LocalEmbeddingProvider's own comment, and
  // docs/embeddings.md's "hybrid search relevance" writeup, for real
  // measured numbers). FtsSearch::tryHybridSearch() calls this, never
  // embed(), for the query side.
  virtual std::vector<float> embedQuery(const std::string& text) { return embed(text); }

  // Fixed per provider instance (depends on the underlying model) — a
  // caller needs this up front to size whatever storage/index it builds.
  virtual std::size_t dimensions() const = 0;

  // Model's trained context window in tokens (n_ctx), or 0 if unknown.
  // chunkForEmbedding() uses this to keep each passage under the real
  // limit (LocalEmbeddingProvider reports 512 for bge-small-en-v1.5);
  // 0 falls back to kEmbeddingChunkTargetTokens. Cloud endpoints that
  // accept 8k+ tokens still chunk at the retrieval-sized default —
  // a 4k-token mean-pooled vector is a worse search unit than overlapping
  // ~384-token passages, even when the API would accept the whole doc.
  virtual int maxInputTokens() const { return 0; }

  // A stable string identifying WHICH model/configuration this provider
  // instance actually embeds with — NOT just its dimensionality.
  // EmbeddingIndexer::ensureTable() compares this (see
  // docs/embeddings.md's "vec0's dimension is fixed at CREATE time" +
  // the model-identity follow-up) because two different models can
  // share the same dimensions() (384 is a common small-model size) while
  // producing genuinely incompatible vector spaces for the same text —
  // dimensions() alone would silently let stored vectors from an OLD
  // model get compared against fresh queries from a NEW one and produce
  // meaningless similarity scores, not an error. Must change whenever
  // the actual embedding behavior could change (a different model file,
  // a different cloud model name) — never needs to be human-readable,
  // only stable and distinct.
  virtual std::string modelIdentifier() const = 0;
};

}  // namespace wikicore::embeddings
