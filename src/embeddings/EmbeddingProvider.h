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
  // caller could safely treat as "similar to nothing".
  virtual std::vector<float> embed(const std::string& text) = 0;

  // Fixed per provider instance (depends on the underlying model) — a
  // caller needs this up front to size whatever storage/index it builds.
  virtual std::size_t dimensions() const = 0;

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
