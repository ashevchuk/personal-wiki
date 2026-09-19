#pragma once

#include "embeddings/EmbeddingProvider.h"

#include <memory>

namespace wikicore::config {
struct AppConfig;
}

namespace wikicore::embeddings {

// Reads AppConfig.embeddingsProvider and returns the matching provider —
// NullEmbeddingProvider for "none" (always available). Throws a
// std::runtime_error, never silently falls back to NullEmbeddingProvider,
// for "local"/"cloud" when this binary wasn't built with the matching
// WIKI_ENABLE_LOCAL_EMBEDDINGS/WIKI_ENABLE_CLOUD_EMBEDDINGS flag. See
// docs/embeddings.md's "Fail loudly, not silently".
std::unique_ptr<EmbeddingProvider> createEmbeddingProvider(
    const config::AppConfig& config);

}  // namespace wikicore::embeddings
