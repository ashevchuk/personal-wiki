#pragma once

#include "embeddings/EmbeddingProvider.h"

namespace wikicore::embeddings {

// provider = "none" (the default). Always compiled in regardless of
// WIKI_ENABLE_LOCAL_EMBEDDINGS/WIKI_ENABLE_CLOUD_EMBEDDINGS — "none" must
// always be a valid, always-buildable choice (see docs/embeddings.md).
// embed() always throws: nothing should ever actually call it when the
// configured provider is "none" — check dimensions()/the configured
// provider kind before calling embed(), not discover "none" the hard way
// at call time.
class NullEmbeddingProvider : public EmbeddingProvider {
 public:
  std::vector<float> embed(const std::string& text) override;
  std::size_t dimensions() const override;
  std::string modelIdentifier() const override;
};

}  // namespace wikicore::embeddings
