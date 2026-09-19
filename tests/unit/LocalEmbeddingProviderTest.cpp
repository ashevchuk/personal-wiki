// Real-model semantic-similarity proof — only built when
// WIKI_TEST_EMBEDDING_MODEL_PATH points at a real GGUF file (see
// tests/CMakeLists.txt). Not part of the default unit_tests binary: this
// exercises actual model inference (tens of ms per embed() call, not the
// microseconds the rest of this project's unit tests take), and needs a
// real model file this project doesn't vendor.
#include "embeddings/LocalEmbeddingProvider.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <sstream>
#include <thread>
#include <vector>

using namespace wikicore::embeddings;

namespace {

// Both vectors are already L2-normalized by LocalEmbeddingProvider::embed(),
// so plain dot product IS cosine similarity here — no separate division by
// magnitudes needed.
double cosine(const std::vector<float>& a, const std::vector<float>& b) {
  double dot = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
  }
  return dot;
}

}  // namespace

TEST_CASE("LocalEmbeddingProvider: dimensions() matches the model's real "
          "embedding size",
          "[LocalEmbeddingProvider][real-model]") {
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  // bge-small-en-v1.5 specifically is 384-dim — if a different model is
  // pointed at via WIKI_TEST_EMBEDDING_MODEL_PATH this assertion is the
  // one expected to need updating, not a sign of a real regression.
  REQUIRE(provider.dimensions() == 384);
}

TEST_CASE("LocalEmbeddingProvider: identical text embeds to a vector with "
          "cosine similarity ~1.0 against itself",
          "[LocalEmbeddingProvider][real-model]") {
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);
  const auto a = provider.embed("The cat sat on the mat.");
  const auto b = provider.embed("The cat sat on the mat.");
  REQUIRE(cosine(a, b) > 0.999);
}

TEST_CASE("LocalEmbeddingProvider: semantically related sentences score "
          "meaningfully higher cosine similarity than unrelated ones — the "
          "actual proof this produces usable semantic vectors, not just "
          "that it runs without crashing",
          "[LocalEmbeddingProvider][real-model]") {
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);

  const auto cat = provider.embed("The cat sat on the mat.");
  const auto kitten = provider.embed("A kitten was resting on the rug.");
  const auto unrelated = provider.embed("The stock market crashed yesterday.");

  const double related = cosine(cat, kitten);
  const double apart = cosine(cat, unrelated);

  INFO("related (cat/kitten): " << related);
  INFO("unrelated (cat/stock market): " << apart);

  // Observed live against bge-small-en-v1.5 (f16 GGUF,
  // CompendiumLabs/bge-small-en-v1.5-gguf): related ~0.78, unrelated
  // ~0.44 — a 0.34 gap. Thresholds below leave real margin under those
  // observed numbers rather than pinning to them exactly, so a harmless
  // model-version bump doesn't turn into test flakiness.
  REQUIRE(related > 0.6);
  REQUIRE(apart < 0.6);
  REQUIRE(related - apart > 0.15);
}

TEST_CASE("LocalEmbeddingProvider: a second, independent related/unrelated "
          "pair confirms the first result wasn't a coincidence of one "
          "specific sentence pair",
          "[LocalEmbeddingProvider][real-model]") {
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);

  const auto wikiA =
      provider.embed("Personal wiki with markdown storage and full-text search.");
  const auto wikiB =
      provider.embed("A self-hosted note-taking app that indexes markdown files.");
  const auto unrelated = provider.embed("Quarterly financial report for the fiscal year.");

  const double related = cosine(wikiA, wikiB);
  const double apart = cosine(wikiA, unrelated);

  INFO("related (wiki description pair): " << related);
  INFO("unrelated (wiki vs. financial report): " << apart);

  REQUIRE(related > 0.6);
  REQUIRE(apart < 0.6);
  REQUIRE(related - apart > 0.15);
}

TEST_CASE("LocalEmbeddingProvider: a text exceeding the model's context "
          "window throws a real, catchable std::runtime_error instead of "
          "crashing the process",
          "[LocalEmbeddingProvider][real-model]") {
  // Real bug, found live testing the admin embeddings-status endpoint
  // (docs/embeddings.md's "Three real bugs" section): before this check
  // existed, a text this long reached llama_decode() and tripped an
  // internal GGML_ASSERT — a hard abort(), not a C++ exception
  // IndexUpdater's own catch(...) could do anything about. A real
  // wiki-server process was crashed this way saving one oversized
  // document over real HTTP before the fix landed.
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);

  std::ostringstream huge;
  for (int i = 0; i < 5000; ++i) huge << "word ";

  REQUIRE_THROWS_AS(provider.embed(huge.str()), std::runtime_error);

  // The provider must stay usable afterward — a rejected oversized text
  // is this ONE call's problem, not grounds to leave the context/model in
  // a broken state for every future embed() on the same instance (exactly
  // how IndexUpdater reuses one long-lived provider across many
  // documents).
  const auto ok = provider.embed("A short sentence.");
  REQUIRE(ok.size() == provider.dimensions());
}

TEST_CASE("LocalEmbeddingProvider: concurrent embed() calls on the SAME "
          "provider instance from multiple threads don't corrupt each "
          "other's results or crash",
          "[LocalEmbeddingProvider][real-model]") {
  // Real bug, found live while ARM-cross-compiling for a production
  // deploy (docs/embeddings.md): IndexUpdater::upsertOne() deliberately
  // calls provider_->embed() OUTSIDE any lock (so a slow embed doesn't
  // block other threads' document saves) — and main.cpp shares ONE
  // LocalEmbeddingProvider instance across every IndexUpdater in the
  // process (the HTTP-request one AND VaultWatcher's own). Nothing was
  // serializing two threads calling embed() on that same instance at the
  // same time, because llama_context's mutable KV-cache/sequence state
  // (touched by llama_memory_clear/llama_decode) isn't reentrant.
  // Reproduced live under qemu-arm-static as a hard SIGSEGV with just two
  // threads racing one embed() call each — not a hypothetical, a real
  // crash caught before a production deploy. Fixed with a mutex owned by
  // LocalEmbeddingProvider itself (embedMutex_) rather than pushing the
  // requirement onto every caller.
  LocalEmbeddingProvider provider(WIKI_TEST_EMBEDDING_MODEL_PATH);

  constexpr int kThreads = 8;
  constexpr int kCallsPerThread = 3;
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&provider, &failures, t] {
      for (int i = 0; i < kCallsPerThread; ++i) {
        try {
          const auto v = provider.embed("Thread " + std::to_string(t) + " call " +
                                         std::to_string(i) + ": the cat sat on the mat.");
          if (v.size() != provider.dimensions()) {
            ++failures;
            continue;
          }
          for (float x : v) {
            if (std::isnan(x) || std::isinf(x)) {
              ++failures;
              break;
            }
          }
        } catch (...) {
          ++failures;
        }
      }
    });
  }
  for (auto& th : threads) th.join();

  REQUIRE(failures.load() == 0);
}
