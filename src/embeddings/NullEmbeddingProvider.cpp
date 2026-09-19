#include "embeddings/NullEmbeddingProvider.h"

#include <stdexcept>

namespace wikicore::embeddings {

std::vector<float> NullEmbeddingProvider::embed(const std::string&) {
  throw std::runtime_error(
      "NullEmbeddingProvider::embed() called — the configured embeddings "
      "provider is \"none\"; check the provider kind before calling embed()");
}

std::size_t NullEmbeddingProvider::dimensions() const { return 0; }

std::string NullEmbeddingProvider::modelIdentifier() const { return "none"; }

}  // namespace wikicore::embeddings
