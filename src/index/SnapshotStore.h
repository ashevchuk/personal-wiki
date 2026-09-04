#pragma once

#include "index/Database.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wikicore::index {

struct SnapshotSummary {
  int64_t id;
  std::string snapshotAt;
};

// Phase 2 versioning — `document_snapshots` (schema.h) was created empty
// all the way back in migration 1; this is what actually starts using
// it. A snapshot captures a document's FULL raw file content (front
// matter + body, exactly what DocumentService reads off disk) right
// BEFORE an edit overwrites it — see DocumentService::update — so after
// N edits there are N-1 snapshots, one per PAST state; "the current
// state" is simply whatever's live on disk right now, never itself
// duplicated into this table.
//
// No `author` column populated despite the schema having one: this app
// has exactly one possible author (single admin account, no multi-user/
// roles — see docs/architecture.md), so a per-snapshot author field
// would carry zero actual information here. Left NULL rather than
// hardcoding a username string that isn't really an identity check.
class SnapshotStore {
 public:
  explicit SnapshotStore(Database& db) : db_(db) {}

  void record(int64_t documentRowId, const std::string& content);

  // Newest first.
  std::vector<SnapshotSummary> list(int64_t documentRowId) const;

  // nullopt if `snapshotId` doesn't exist OR belongs to a DIFFERENT
  // document than `documentRowId` — checked explicitly (not just "does
  // this id exist at all") so a caller who can see document A's history
  // can't read document B's snapshot content by guessing/incrementing
  // ids across documents (an IDOR-shaped bug, not a hypothetical one —
  // VersionRoutes.cpp resolves `documentRowId` from the URL's own path
  // via a separate visibility-gated lookup, then hands both to this
  // call together, never trusting snapshotId alone).
  std::optional<std::string> getContent(int64_t documentRowId, int64_t snapshotId) const;

 private:
  Database& db_;
};

}  // namespace wikicore::index
