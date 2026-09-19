#include "embeddings/EmbeddingProviderFactory.h"

#include "config/AppConfig.h"
#include "embeddings/NullEmbeddingProvider.h"
#ifdef WIKI_ENABLE_LOCAL_EMBEDDINGS
#include "embeddings/LocalEmbeddingProvider.h"
#endif
#ifdef WIKI_ENABLE_CLOUD_EMBEDDINGS
#include "embeddings/CloudEmbeddingProvider.h"
#endif

#include <stdexcept>

namespace wikicore::embeddings {

std::unique_ptr<EmbeddingProvider> createEmbeddingProvider(
    const config::AppConfig& config) {
  const std::string& provider = config.embeddingsProvider;

  if (provider.empty() || provider == "none") {
    return std::make_unique<NullEmbeddingProvider>();
  }

  if (provider == "local") {
#ifdef WIKI_ENABLE_LOCAL_EMBEDDINGS
    if (config.embeddingsModelPath.empty()) {
      throw std::runtime_error(
          "embeddings.provider = \"local\" requires embeddings.model_path to "
          "be set (path to a GGUF model file)");
    }
    return std::make_unique<LocalEmbeddingProvider>(config.embeddingsModelPath);
#else
    throw std::runtime_error(
        "embeddings.provider = \"local\" requires this binary to be built "
        "with -DWIKI_ENABLE_LOCAL_EMBEDDINGS=ON; it was not");
#endif
  }

  if (provider == "cloud") {
#ifdef WIKI_ENABLE_CLOUD_EMBEDDINGS
    if (config.embeddingsApiKeyEnv.empty()) {
      throw std::runtime_error(
          "embeddings.provider = \"cloud\" requires embeddings.api_key_env to "
          "be set (the NAME of an environment variable holding the API key)");
    }
    return std::make_unique<CloudEmbeddingProvider>(config.embeddingsApiKeyEnv);
#else
    throw std::runtime_error(
        "embeddings.provider = \"cloud\" requires this binary to be built "
        "with -DWIKI_ENABLE_CLOUD_EMBEDDINGS=ON; it was not");
#endif
  }

  throw std::runtime_error("unknown embeddings.provider value: \"" + provider +
                            "\" (expected \"none\", \"local\", or \"cloud\")");
}

}  // namespace wikicore::embeddings
