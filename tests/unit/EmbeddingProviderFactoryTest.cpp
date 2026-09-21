#include "embeddings/EmbeddingProviderFactory.h"

#include "config/AppConfig.h"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

using namespace wikicore::config;
using namespace wikicore::embeddings;

namespace {

// Asserts `fn` throws std::runtime_error whose what() contains `needle` —
// same manual try/catch shape already used across this test suite rather
// than pulling in Catch2's matchers library (not declared as a vcpkg.json
// feature) just for substring checks.
template <typename Fn>
void requireThrowsContaining(Fn&& fn, const std::string& needle) {
  try {
    fn();
    FAIL("expected std::runtime_error containing \"" + needle + "\", nothing thrown");
  } catch (const std::runtime_error& e) {
    INFO("actual message: " << e.what());
    REQUIRE(std::string(e.what()).find(needle) != std::string::npos);
  }
}

}  // namespace

TEST_CASE("EmbeddingProviderFactory: provider=none always returns a usable "
          "NullEmbeddingProvider, regardless of build flags",
          "[EmbeddingProviderFactory]") {
  AppConfig cfg;
  cfg.embeddingsProvider = "none";
  auto provider = createEmbeddingProvider(cfg);
  REQUIRE(provider != nullptr);
  REQUIRE(provider->dimensions() == 0);
  REQUIRE_THROWS_AS(provider->embed("hello"), std::runtime_error);
}

TEST_CASE("EmbeddingProviderFactory: an empty provider string behaves the "
          "same as \"none\" — matches AppConfig's own default",
          "[EmbeddingProviderFactory]") {
  AppConfig cfg;
  cfg.embeddingsProvider = "";
  auto provider = createEmbeddingProvider(cfg);
  REQUIRE(provider != nullptr);
  REQUIRE(provider->dimensions() == 0);
}

TEST_CASE("EmbeddingProviderFactory: provider=local fails loudly — never "
          "silently falls back to NullEmbeddingProvider",
          "[EmbeddingProviderFactory]") {
  AppConfig cfg;
  cfg.embeddingsProvider = "local";
#ifdef WIKI_ENABLE_LOCAL_EMBEDDINGS
  // Compiled in and implemented — but AppConfig.embeddingsModelPath
  // defaults to empty, and LocalEmbeddingProvider genuinely can't load a
  // model from no path. This is the one branch of "local" this test can
  // exercise without a real GGUF file on disk; loading a real one and
  // actually embedding text is LocalEmbeddingProviderTest.cpp's job (real
  // model, opt-in — see docs/embeddings.md).
  requireThrowsContaining([&] { createEmbeddingProvider(cfg); }, "model_path");
#else
  // Not compiled in — the error must name the missing build flag so an
  // admin who set provider="local" and restarted gets a real reason,
  // not search quietly staying FTS5-only with no signal why.
  requireThrowsContaining([&] { createEmbeddingProvider(cfg); },
                          "WIKI_ENABLE_LOCAL_EMBEDDINGS");
#endif
}

TEST_CASE("EmbeddingProviderFactory: provider=cloud fails loudly — never "
          "silently falls back to NullEmbeddingProvider",
          "[EmbeddingProviderFactory]") {
  AppConfig cfg;
  cfg.embeddingsProvider = "cloud";
#ifdef WIKI_ENABLE_CLOUD_EMBEDDINGS
  // Compiled in — api_key_env is optional (local OpenAI-compatible
  // servers often have no auth). Construction with no key and the
  // OpenAI defaults must succeed; a real successful embed() still
  // needs a key against api.openai.com (see CloudEmbeddingProviderTest).
  auto provider = createEmbeddingProvider(cfg);
  REQUIRE(provider != nullptr);
  REQUIRE(provider->dimensions() == 1536);
  REQUIRE(provider->modelIdentifier().find("text-embedding-3-small") !=
          std::string::npos);
  REQUIRE(provider->modelIdentifier().find("api.openai.com") != std::string::npos);
#else
  requireThrowsContaining([&] { createEmbeddingProvider(cfg); },
                          "WIKI_ENABLE_CLOUD_EMBEDDINGS");
#endif
}

TEST_CASE("EmbeddingProviderFactory: an unrecognized provider value throws — "
          "not silently treated as \"none\"",
          "[EmbeddingProviderFactory]") {
  AppConfig cfg;
  cfg.embeddingsProvider = "carrier-pigeon";
  requireThrowsContaining([&] { createEmbeddingProvider(cfg); },
                          "unknown embeddings.provider");
}
