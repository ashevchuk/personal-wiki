// Real-API integration test — only built when WIKI_ENABLE_CLOUD_EMBEDDINGS
// is on (see tests/CMakeLists.txt), and skipped at RUNTIME (not build
// time) when OPENAI_API_KEY isn't actually set in the environment — a real
// key is a secret this project can't assume is present on every machine
// that builds it, so this can't be a hard build requirement the way
// LocalEmbeddingProviderTest.cpp's model-file CMake gate is.
#include "embeddings/CloudEmbeddingProvider.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <stdexcept>

using namespace wikicore::embeddings;

namespace {

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
  double dot = 0.0, na = 0.0, nb = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    dot += static_cast<double>(a[i]) * b[i];
    na += static_cast<double>(a[i]) * a[i];
    nb += static_cast<double>(b[i]) * b[i];
  }
  return dot / (std::sqrt(na) * std::sqrt(nb));
}

}  // namespace

TEST_CASE("CloudEmbeddingProvider: real OpenAI API call produces a usable "
          "1536-dim embedding, and related text scores higher similarity "
          "than unrelated text",
          "[CloudEmbeddingProvider][real-api]") {
  if (std::getenv("OPENAI_API_KEY") == nullptr) {
    SKIP("OPENAI_API_KEY not set in the environment — this test calls the "
         "real OpenAI API and needs a real key (source .env first)");
  }

  CloudEmbeddingProvider provider("OPENAI_API_KEY");
  REQUIRE(provider.dimensions() == 1536);
  REQUIRE(provider.modelIdentifier() ==
          "cloud:https://api.openai.com/v1:text-embedding-3-small");

  const auto cat = provider.embed("The cat sat on the mat.");
  REQUIRE(cat.size() == 1536);

  const auto kitten = provider.embed("A kitten was resting on the rug.");
  const auto unrelated = provider.embed("The stock market crashed yesterday.");

  const double related = cosine(cat, kitten);
  const double apart = cosine(cat, unrelated);

  INFO("related (cat/kitten): " << related);
  INFO("unrelated (cat/stock market): " << apart);

  REQUIRE(related > apart);
}

TEST_CASE("CloudEmbeddingProvider: OpenAI-compatible api_base/model/dimensions "
          "are reflected in dimensions() and modelIdentifier() without a "
          "network call",
          "[CloudEmbeddingProvider]") {
  CloudEmbeddingProvider provider(
      /*apiKeyEnvVar=*/"", "http://127.0.0.1:9/v1", "nomic-embed-text", 768);
  REQUIRE(provider.dimensions() == 768);
  REQUIRE(provider.modelIdentifier() ==
          "cloud:http://127.0.0.1:9/v1:nomic-embed-text");
}

TEST_CASE("CloudEmbeddingProvider: a non-http(s) api_base is rejected at "
          "construction, not at the first embed() call",
          "[CloudEmbeddingProvider]") {
  REQUIRE_THROWS_AS(
      CloudEmbeddingProvider("", "ftp://example.com/v1", "", 0),
      std::runtime_error);
  REQUIRE_THROWS_AS(CloudEmbeddingProvider("", "not-a-url", "", 0),
                    std::runtime_error);
}

TEST_CASE("CloudEmbeddingProvider: a named api_key_env whose variable is "
          "unset fails at construction",
          "[CloudEmbeddingProvider]") {
  REQUIRE_THROWS_AS(
      CloudEmbeddingProvider("WIKI_TEST_MISSING_EMBEDDINGS_KEY_XYZ"),
      std::runtime_error);
}
