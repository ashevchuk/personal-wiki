#pragma once

#include "embeddings/EmbeddingProvider.h"

#include <cstddef>
#include <string>

namespace wikicore::embeddings {

// WIKI_ENABLE_CLOUD_EMBEDDINGS-gated: HTTP client against any
// OpenAI-compatible POST {api_base}/embeddings endpoint. Defaults match
// OpenAI's own API (https://api.openai.com/v1, text-embedding-3-small,
// 1536-dim). See docs/embeddings.md.
class CloudEmbeddingProvider : public EmbeddingProvider {
 public:
  // apiKeyEnvVar: the NAME of an environment variable holding the API
  // key — never the raw key itself, matching AppConfig.embeddingsApiKeyEnv's
  // own contract (config.toml is plain text; an env var doesn't travel
  // with it if the file is pasted somewhere). Empty means no
  // Authorization header (local OpenAI-compatible servers that don't
  // authenticate). Throws std::runtime_error if a name IS given but that
  // variable isn't set.
  //
  // apiBase/model/dimensions empty-or-zero mean the OpenAI defaults
  // above. apiBase must be an absolute http(s) URL when set.
  explicit CloudEmbeddingProvider(const std::string& apiKeyEnvVar,
                                  std::string apiBase = {},
                                  std::string model = {},
                                  std::size_t dimensions = 0);

  // Throws std::runtime_error on a network/HTTP/parse failure, or if the
  // API's response shape doesn't match what this provider expects.
  std::vector<float> embed(const std::string& text) override;
  std::size_t dimensions() const override;
  std::string modelIdentifier() const override;

 private:
  std::string apiKey_;
  std::string origin_;
  std::string embeddingsPath_;
  std::string apiBase_;
  std::string model_;
  std::size_t dimensions_;
};

}  // namespace wikicore::embeddings
