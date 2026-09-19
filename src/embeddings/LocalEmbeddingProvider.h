#pragma once

#include "embeddings/EmbeddingProvider.h"

#include <string>

// Opaque forward declarations from llama.h — kept out of this header so
// nothing outside this one translation unit needs llama.cpp's include
// path, matching the same "hide the vendored SDK behind a small internal
// type" discipline as McpServer.cpp not leaking cpp-mcp's json type into
// wikicore's public API.
struct llama_model;
struct llama_context;

namespace wikicore::embeddings {

// WIKI_ENABLE_LOCAL_EMBEDDINGS-gated: llama.cpp-backed provider. Loads a
// GGUF model once at construction and reuses the same context for every
// embed() call — model loading is the expensive part, not per-call
// inference. See docs/embeddings.md.
class LocalEmbeddingProvider : public EmbeddingProvider {
 public:
  // Throws std::runtime_error if the model at `modelPath` fails to load.
  explicit LocalEmbeddingProvider(const std::string& modelPath);
  ~LocalEmbeddingProvider() override;

  LocalEmbeddingProvider(const LocalEmbeddingProvider&) = delete;
  LocalEmbeddingProvider& operator=(const LocalEmbeddingProvider&) = delete;

  // Throws std::runtime_error on tokenization/inference failure, OR if
  // `text` tokenizes to more than maxTokens_ tokens — see maxTokens_'s own
  // comment for why this check exists and must run before llama_decode(),
  // not after.
  std::vector<float> embed(const std::string& text) override;
  std::size_t dimensions() const override;
  std::string modelIdentifier() const override;

 private:
  llama_model* model_ = nullptr;
  llama_context* ctx_ = nullptr;
  std::size_t dimensions_ = 0;
  // The context's n_ctx (the model's own trained context window — 512 for
  // bge-small-en-v1.5), captured at construction. embed() throws a real,
  // catchable std::runtime_error when a text's token count exceeds this,
  // BEFORE calling llama_decode() — found live wiring up the admin
  // "needing attention" retry path (docs/embeddings.md's "Three real
  // bugs"): a text long enough to exceed llama.cpp's own internal
  // n_ubatch trips `GGML_ASSERT(cparams.n_ubatch >= n_tokens)` INSIDE
  // llama_decode(), which calls abort() directly — a hard process crash,
  // not a C++ exception IndexUpdater's existing catch(...) can do
  // anything about. Even below that harder limit, feeding a model more
  // tokens than it was trained on (n_ctx) produces a semantically
  // meaningless embedding, not just a slow one — this check makes BOTH
  // problems a normal, recorded, retry-able embedding failure instead.
  int32_t maxTokens_ = 0;
  // Captured once at construction (path + mtime + size, not a content
  // hash of the whole file — see modelIdentifier()'s own doc comment in
  // EmbeddingProvider.h for why this is enough without hashing tens of
  // MB on every startup).
  std::string modelIdentifier_;
};

}  // namespace wikicore::embeddings
