#pragma once

#include "embeddings/EmbeddingProvider.h"

#include <mutex>
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
  // `queryPrefix`: prepended to the text ONLY for embedQuery() calls, never
  // for plain embed() (document/passage) calls — see embedQuery()'s own
  // comment below and EmbeddingProvider.h's for why this asymmetry exists
  // and matters. Empty (the default) means no prefix, i.e. embedQuery()
  // behaves exactly like embed() — correct for a model with no documented
  // query-side instruction convention. For bge-small-en-v1.5 specifically,
  // the model's own card recommends
  // "Represent this sentence for searching relevant passages: " — see
  // config.example.toml's [embeddings] section and
  // docs/embeddings.md's "hybrid search relevance" writeup for real,
  // measured before/after numbers. Throws std::runtime_error if the model
  // at `modelPath` fails to load.
  explicit LocalEmbeddingProvider(const std::string& modelPath,
                                   const std::string& queryPrefix = "");
  ~LocalEmbeddingProvider() override;

  LocalEmbeddingProvider(const LocalEmbeddingProvider&) = delete;
  LocalEmbeddingProvider& operator=(const LocalEmbeddingProvider&) = delete;

  // Throws std::runtime_error on tokenization/inference failure, OR if
  // `text` tokenizes to more than maxTokens_ tokens — see maxTokens_'s own
  // comment for why this check exists and must run before llama_decode(),
  // not after.
  std::vector<float> embed(const std::string& text) override;

  // embed(queryPrefix_ + text) when queryPrefix_ is non-empty, else
  // identical to embed(text) — see the constructor's own comment. Adding
  // the prefix ONLY on this path (never to a document/passage embed()
  // call) is the whole point: an asymmetric retrieval model was trained
  // to expect the two sides to look different, not identically prefixed
  // or both bare.
  std::vector<float> embedQuery(const std::string& text) override;

  std::size_t dimensions() const override;
  std::string modelIdentifier() const override;

 private:
  llama_model* model_ = nullptr;
  llama_context* ctx_ = nullptr;
  // Guards every embed() call on this instance — llama_context (ctx_) is
  // mutable, shared, per-provider state (llama_memory_clear/llama_decode
  // both write into it), and this provider is itself shared across every
  // IndexUpdater in the process (main.cpp constructs ONE and hands the
  // same pointer to the HTTP-request-handling IndexUpdater AND
  // VaultWatcher's own separate one — see main.cpp's own comment on
  // "same embeddingProvider instance"). IndexUpdater::upsertOne calls
  // provider_->embed() deliberately OUTSIDE its own mutex_ (see
  // that class's comment — a slow embed() must never hold the lock other
  // threads' document saves need), which means nothing outside this class
  // was serializing concurrent embed() calls on the SAME provider at
  // all — two documents saved close together from two Drogon worker
  // threads could, and did, call embed() on this same ctx_ at the same
  // time. Found live: reproduced as a hard SIGSEGV under qemu-arm-static
  // with two threads calling embed() on one provider instance
  // concurrently (see docs/embeddings.md's "real bugs" — not a
  // hypothetical, a real crash caught before it could hit production).
  // Serializing embed() calls here (rather than pushing this requirement
  // onto every caller) is the right owner for it: this provider is the
  // one thing that actually knows its own inference isn't reentrant, and
  // llama.cpp inference on one context gains nothing from being called
  // "concurrently" anyway — ggml already parallelizes internally across
  // threads for a single decode.
  std::mutex embedMutex_;
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
  // See the constructor's own comment — empty means embedQuery() behaves
  // identically to embed().
  std::string queryPrefix_;
};

}  // namespace wikicore::embeddings
