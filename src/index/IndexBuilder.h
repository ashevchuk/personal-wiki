#pragma once

#include "index/IndexUpdater.h"
#include "vault/VaultRepository.h"

#include <cstdint>

namespace wikicore::index {

struct RescanStats {
  int64_t documentsIndexed = 0;
  int64_t staleRowsRemoved = 0;
};

// Full-vault rescan: walks every .md file under the vault root (skipping
// dotdirs like .git/.trash), parses its front matter, and upserts it via
// IndexUpdater — same fail-safe-private auto-repair as normal parsing
// (missing/malformed visibility -> private; missing title falls back to
// the filename). Finishes with a stale sweep that removes any indexed row
// whose file no longer exists on disk.
//
// This is deliberately NOT what DocumentService calls after every save
// (that's IndexUpdater::upsertOne, touching only the one document) — this
// is the "the index might not reflect reality at all" recovery path: run
// once at every wiki-server startup (the db is a disposable cache, never
// assumed correct on faith), and on demand via `wiki-server --reindex` /
// `POST /api/admin/reindex` after external edits or a corrupted/deleted db.
class IndexBuilder {
 public:
  IndexBuilder(vault::VaultRepository& vault, IndexUpdater& indexUpdater)
      : vault_(vault), indexUpdater_(indexUpdater) {}

  RescanStats fullRescan();

 private:
  vault::VaultRepository& vault_;
  IndexUpdater& indexUpdater_;
};

}  // namespace wikicore::index
