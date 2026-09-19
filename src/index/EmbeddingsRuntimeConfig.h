#pragma once

#include "index/Database.h"

namespace wikicore::index {

// Runtime-mutable toggle for whether search() actually attempts hybrid
// (semantic) ranking — deliberately SQLite-backed, not config.toml, same
// "no restart" reasoning as auth::McpRemoteConfig: an admin needs to be
// able to kill vector search live if the configured provider is
// misbehaving (see docs/embeddings.md), without waiting on a restart to
// pick up a config.toml edit. FtsSearch checks this on EVERY search() call
// (not just at construction) for exactly that reason.
//
// Only a bool, no crypto/auth concern here — lives in wikicore (unlike
// McpRemoteConfig, which is auth/-only and OpenSSL-backed for its bearer
// token) so FtsSearch/EmbeddingIndexer can use it without pulling in the
// auth dependency wikicore is deliberately free of (see CLAUDE.md's
// architecture section).
class EmbeddingsRuntimeConfig {
 public:
  explicit EmbeddingsRuntimeConfig(Database& db) : db_(db) {}

  // Defaults to true (matches the schema's own DEFAULT 1) when no row
  // exists yet — i.e. before any admin has ever touched the toggle,
  // vector search stays on whenever a provider is actually configured.
  bool isVectorSearchEnabled() const;
  void setVectorSearchEnabled(bool enabled);

 private:
  Database& db_;
};

}  // namespace wikicore::index
