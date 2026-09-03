#pragma once

#include "index/IndexBuilder.h"
#include "index/IndexUpdater.h"
#include "vault/VaultRepository.h"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace wikicore::vault {

class FolderNotFoundError : public std::runtime_error {
 public:
  explicit FolderNotFoundError(const std::string& path)
      : std::runtime_error("folder not found: " + path) {}
};

class FolderAlreadyExistsError : public std::runtime_error {
 public:
  explicit FolderAlreadyExistsError(const std::string& path)
      : std::runtime_error("a folder or document already exists at: " + path) {}
};

class InvalidFolderMoveError : public std::runtime_error {
 public:
  explicit InvalidFolderMoveError(const std::string& reason)
      : std::runtime_error(reason) {}
};

class FolderNotEmptyError : public std::runtime_error {
 public:
  explicit FolderNotEmptyError(const std::string& path)
      : std::runtime_error("folder not empty: " + path) {}
};

// Folders are NOT a first-class entity anywhere in the data model (see
// NavQueries) — they're purely implicit in document paths containing
// '/'. This is the one place that moves/renames an entire directory
// subtree as a single atomic filesystem operation, then reconciles every
// document that was under it in the SQLite index — which keys rows by
// path, not by a stable folder id (see IndexUpdater::upsertOne: it
// matches by path, NOT by the document's own uuid; only
// IndexBuilder::fullRescan's per-file pass does uuid-based reconciliation
// during a full walk. A folder move touches many paths at once with no
// full walk involved, so this class does the path-reindexing explicitly
// rather than relying on that).
class FolderService {
 public:
  FolderService(VaultRepository& vault, index::IndexUpdater& indexUpdater,
                index::IndexBuilder& indexBuilder)
      : vault_(vault), indexUpdater_(indexUpdater), indexBuilder_(indexBuilder) {}

  // Renames/moves the folder at `oldRelativePath` (and everything under
  // it — nested documents, their .assets folders, subfolders) to
  // `newRelativePath` via ONE atomic std::filesystem::rename of the whole
  // subtree — not a per-file loop, so there's no window where the
  // subtree is half-moved. Afterward, re-derives the index row for every
  // document that was under the old prefix from whatever's now on disk
  // at its new path — the exact same parse/fallback logic
  // IndexBuilder::fullRescan() uses per-file, via its public
  // reindexOneFile(). Document front matter itself (title, id, tags,
  // visibility, ...) is untouched; only the `path` column changes.
  //
  // Throws FolderNotFoundError if the source isn't an existing directory,
  // FolderAlreadyExistsError if the destination is already occupied,
  // InvalidFolderMoveError if the destination is the source itself or
  // nested inside it (moving a folder into itself) or either path is
  // empty (refuses to move the vault root).
  //
  // Returns the number of documents successfully reindexed at their new
  // paths — this is a best-effort count, not a transactional guarantee:
  // the filesystem move itself is atomic and always fully succeeds or
  // throws before touching anything, but if re-parsing one particular
  // moved file's front matter fails afterward, that one document is
  // simply left stale in the index (same fallback as everywhere else in
  // this codebase — the next full rescan / `--reindex` reconciles it; see
  // docs/architecture.md's note on VaultWatcher being best-effort for the
  // same reasoning).
  int64_t move(const std::string& oldRelativePath, const std::string& newRelativePath);

  // True only if the directory exists and contains nothing at all (no
  // files, no subdirectories — including dot-entries). The safety
  // condition this class enforces before allowing `remove()`.
  bool isEmpty(const std::string& relativePath) const;

  // Removes an EMPTY directory. Throws FolderNotFoundError if it doesn't
  // exist, FolderNotEmptyError otherwise. Deliberately no
  // cascade-delete-everything-inside option — that's a much
  // higher-blast-radius operation than this ever needs to expose; delete
  // the documents first (which already soft-deletes to .trash/), then the
  // now-empty folder.
  void remove(const std::string& relativePath);

 private:
  VaultRepository& vault_;
  index::IndexUpdater& indexUpdater_;
  index::IndexBuilder& indexBuilder_;
};

}  // namespace wikicore::vault
