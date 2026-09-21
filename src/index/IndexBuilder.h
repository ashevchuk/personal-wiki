#pragma once

#include "index/IndexUpdater.h"
#include "index/RescanProgress.h"
#include "vault/VaultRepository.h"

#include <cstdint>
#include <string>

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
// Embedding inference is queued by each upsert; this method waits for
// those jobs at the end so `--reindex` still means "embeddings are done".
// Ordinary HTTP Save does not wait (see IndexUpdater::upsertOne).
class IndexBuilder {
 public:
  IndexBuilder(vault::VaultRepository& vault, IndexUpdater& indexUpdater)
      : vault_(vault), indexUpdater_(indexUpdater) {}

  // `progress`: optional (nullptr — the default — means no progress
  // reporting, e.g. the CLI --reindex path, which prints its own final
  // RescanStats when done and has no concurrent reader to report to
  // anyway). When given, set inProgress=true for the duration and
  // documentsIndexed is incremented after each file — see
  // RescanProgress.h's own comment for who reads this and why.
  RescanStats fullRescan(RescanProgress* progress = nullptr);

  // Re-derives the index row for exactly one document from whatever's on
  // disk right now, the same parse/fallback-title/stat/upsert logic
  // fullRescan() uses per-file. Returns true if the file existed and was
  // (re-)indexed, false if it doesn't exist — in which case the caller
  // (VaultWatcher's change callback) should call
  // IndexUpdater::removeOne itself; this method deliberately doesn't do
  // that removal on the caller's behalf, since "file missing" and "file
  // unreadable" would otherwise be indistinguishable to the caller.
  bool reindexOneFile(const std::string& relativePath);

 private:
  vault::VaultRepository& vault_;
  IndexUpdater& indexUpdater_;
};

}  // namespace wikicore::index
