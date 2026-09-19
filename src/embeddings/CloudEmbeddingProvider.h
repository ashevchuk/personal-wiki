#pragma once

#include "embeddings/EmbeddingProvider.h"

#include <string>

namespace wikicore::embeddings {

// WIKI_ENABLE_CLOUD_EMBEDDINGS-gated: HTTP client against OpenAI's
// /v1/embeddings endpoint (text-embedding-3-small, 1536-dim). See
// docs/embeddings.md.
class CloudEmbeddingProvider : public EmbeddingProvider {
 public:
  // apiKeyEnvVar: the NAME of an environment variable holding the API
  // key — never the raw key itself, matching AppConfig.embeddingsApiKeyEnv's
  // own contract (config.toml is plain text; an env var doesn't travel
  // with it if the file is pasted somewhere). Throws std::runtime_error
  // if that variable isn't set.
  explicit CloudEmbeddingProvider(const std::string& apiKeyEnvVar);

  // Throws std::runtime_error on a network/HTTP/parse failure, or if the
  // API's response shape doesn't match what this provider expects.
  std::vector<float> embed(const std::string& text) override;
  std::size_t dimensions() const override;
  std::string modelIdentifier() const override;

 private:
  std::string apiKey_;
};

}  // namespace wikicore::embeddings
