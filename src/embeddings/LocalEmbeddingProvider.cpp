#include "embeddings/LocalEmbeddingProvider.h"

#include <llama.h>

#include <cmath>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace wikicore::embeddings {

namespace {

// llama_backend_init() is documented as a once-per-process call, not
// once-per-context — std::call_once guards against a second
// LocalEmbeddingProvider (or a future reload) re-initializing it.
// Deliberately never paired with llama_backend_free(): with more than one
// live provider, freeing the backend out from under the other would be
// worse than the small one-time leak the OS reclaims at process exit
// anyway — same "don't add cleanup a real caller doesn't need" restraint
// as the rest of this codebase.
std::once_flag g_backendInitFlag;

void ensureBackendInit() {
  std::call_once(g_backendInitFlag, [] { llama_backend_init(); });
}

// bge/BERT-family embedding models are trained to produce, and are
// compared via, unit-length vectors — cosine similarity on two
// non-normalized vectors from this same model would still rank pairs
// correctly (normalization doesn't change relative ordering), but every
// downstream consumer stops having to know or care that a provider was
// normalized instead of trusting dimensions()/embed()'s own contract to
// mean "ready to compare directly."
void l2Normalize(std::vector<float>& v) {
  double sumSq = 0.0;
  for (float x : v) sumSq += static_cast<double>(x) * x;
  const double norm = std::sqrt(sumSq);
  if (norm <= 0.0) return;  // an all-zero embedding stays all-zero
  for (float& x : v) x = static_cast<float>(x / norm);
}

}  // namespace

LocalEmbeddingProvider::LocalEmbeddingProvider(const std::string& modelPath) {
  ensureBackendInit();

  // Path + mtime + size, not a content hash of the whole (potentially
  // tens-of-MB) file — cheap to compute at every startup, and still
  // catches the realistic cases (a different model file, or the SAME
  // path pointing at a genuinely different file after being swapped)
  // that matter for EmbeddingIndexer's model-change detection. Computed
  // before the load attempt below so a failed load never leaves this
  // provider half-constructed with the identifier unset.
  {
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(modelPath, ec);
    const auto size = std::filesystem::file_size(modelPath, ec);
    modelIdentifier_ = "local:" + modelPath + ":" +
                        std::to_string(mtime.time_since_epoch().count()) + ":" +
                        std::to_string(size);
  }

  llama_model_params modelParams = llama_model_default_params();
  modelParams.n_gpu_layers = 0;  // CPU-only — no GPU backend assumed present
  // (real deployment targets here range from a dev x86_64 machine to a
  // Raspberry Pi; neither has a GPU llama.cpp would use).

  model_ = llama_model_load_from_file(modelPath.c_str(), modelParams);
  if (model_ == nullptr) {
    throw std::runtime_error("LocalEmbeddingProvider: failed to load model from '" +
                              modelPath + "'");
  }

  if (llama_model_has_encoder(model_) && llama_model_has_decoder(model_)) {
    llama_model_free(model_);
    model_ = nullptr;
    throw std::runtime_error(
        "LocalEmbeddingProvider: encoder-decoder models are not supported for "
        "embeddings ('" +
        modelPath + "')");
  }

  llama_context_params ctxParams = llama_context_default_params();
  ctxParams.embeddings = true;
  // pooling_type left at its default (LLAMA_POOLING_TYPE_UNSPECIFIED) so
  // llama.cpp reads whatever pooling strategy the GGUF's own metadata
  // declares for this model, rather than this code guessing/hardcoding
  // one that might be wrong for a model swapped in later.
  ctxParams.n_ctx = llama_model_n_ctx_train(model_);
  // Non-causal (encoder, e.g. BERT-family) models require batch == ubatch
  // — mirrors examples/embedding/embedding.cpp's own setup.
  if (ctxParams.n_batch < ctxParams.n_ctx) {
    ctxParams.n_batch = ctxParams.n_ctx;
  }
  ctxParams.n_ubatch = ctxParams.n_batch;

  ctx_ = llama_init_from_model(model_, ctxParams);
  if (ctx_ == nullptr) {
    llama_model_free(model_);
    model_ = nullptr;
    throw std::runtime_error("LocalEmbeddingProvider: failed to create context for '" +
                              modelPath + "'");
  }

  dimensions_ = static_cast<std::size_t>(llama_model_n_embd(model_));
  maxTokens_ = ctxParams.n_ctx;
}

LocalEmbeddingProvider::~LocalEmbeddingProvider() {
  if (ctx_ != nullptr) llama_free(ctx_);
  if (model_ != nullptr) llama_model_free(model_);
}

std::vector<float> LocalEmbeddingProvider::embed(const std::string& text) {
  // See embedMutex_'s own comment in LocalEmbeddingProvider.h — this whole
  // function, not just the llama_decode() call, runs under the lock: the
  // model/vocab lookups below are read-only against model_ (safe to share),
  // but everything from here on on touches ctx_'s mutable KV-cache/sequence
  // state, and locking only part of the function would just move the race
  // instead of removing it.
  std::lock_guard<std::mutex> lock(embedMutex_);

  const llama_vocab* vocab = llama_model_get_vocab(model_);

  // First call sizes the token buffer; llama_tokenize returns a negative
  // count (its magnitude the required size) when the buffer is too
  // small, per its own documented contract — no separate "count only"
  // entry point exists.
  std::vector<llama_token> tokens(text.size() + 16);
  int32_t nTokens = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()),
                                    tokens.data(), static_cast<int32_t>(tokens.size()),
                                    /*add_special=*/true, /*parse_special=*/true);
  if (nTokens < 0) {
    tokens.resize(static_cast<std::size_t>(-nTokens));
    nTokens = llama_tokenize(vocab, text.c_str(), static_cast<int32_t>(text.size()),
                              tokens.data(), static_cast<int32_t>(tokens.size()),
                              /*add_special=*/true, /*parse_special=*/true);
  }
  if (nTokens < 0) {
    throw std::runtime_error("LocalEmbeddingProvider::embed: tokenization failed");
  }
  tokens.resize(static_cast<std::size_t>(nTokens));

  // Must run BEFORE llama_decode() — see maxTokens_'s own comment for why
  // this is a real crash-prevention check, not defensive paranoia: past
  // this point, llama_decode() would otherwise trip an internal
  // GGML_ASSERT and abort() the whole process for a text this long.
  if (nTokens > maxTokens_) {
    throw std::runtime_error(
        "LocalEmbeddingProvider::embed: text tokenizes to " + std::to_string(nTokens) +
        " tokens, exceeding this model's " + std::to_string(maxTokens_) +
        "-token context window — shorten the document");
  }

  llama_batch batch = llama_batch_init(nTokens, /*embd=*/0, /*n_seq_max=*/1);
  for (int32_t i = 0; i < nTokens; ++i) {
    batch.token[i] = tokens[static_cast<std::size_t>(i)];
    batch.pos[i] = i;
    batch.n_seq_id[i] = 1;
    batch.seq_id[i][0] = 0;
    batch.logits[i] = 1;  // every token contributes to the pooled output
  }
  batch.n_tokens = nTokens;

  // Clears KV-cache/sequence state left over from a PRIOR embed() call on
  // this same context — irrelevant to the embedding itself, but a stale
  // sequence-0 state from the last call would otherwise bleed into this
  // one (see examples/embedding/embedding.cpp's batch_decode, same call
  // before every decode for the same reason).
  llama_memory_clear(llama_get_memory(ctx_), true);

  const int32_t decodeResult = llama_decode(ctx_, batch);
  if (decodeResult < 0) {
    llama_batch_free(batch);
    throw std::runtime_error("LocalEmbeddingProvider::embed: llama_decode failed (code " +
                              std::to_string(decodeResult) + ")");
  }

  const float* embd = llama_get_embeddings_seq(ctx_, /*seq_id=*/0);
  if (embd == nullptr) {
    // LLAMA_POOLING_TYPE_NONE models have no per-sequence embedding —
    // this provider requires a model whose GGUF metadata declares real
    // pooling (mean/CLS/last), which every sentence-embedding model
    // (bge, gte, e5, nomic-embed, ...) does.
    llama_batch_free(batch);
    throw std::runtime_error(
        "LocalEmbeddingProvider::embed: model has no sequence-pooled embeddings "
        "(pooling_type is NONE) — not a sentence-embedding model");
  }

  std::vector<float> result(embd, embd + dimensions_);
  llama_batch_free(batch);

  l2Normalize(result);
  return result;
}

std::size_t LocalEmbeddingProvider::dimensions() const { return dimensions_; }

std::string LocalEmbeddingProvider::modelIdentifier() const { return modelIdentifier_; }

}  // namespace wikicore::embeddings
