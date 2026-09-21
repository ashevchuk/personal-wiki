#pragma once

#include "index/IndexUpdater.h"
#include "index/SnapshotStore.h"
#include "vault/FrontMatter.h"
#include "vault/VaultRepository.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace wikicore::vault {

class DocumentNotFoundError : public std::runtime_error {
 public:
  explicit DocumentNotFoundError(const std::string& path)
      : std::runtime_error("document not found: " + path) {}
};

class DocumentAlreadyExistsError : public std::runtime_error {
 public:
  explicit DocumentAlreadyExistsError(const std::string& path)
      : std::runtime_error("document already exists: " + path) {}
};

class InvalidDocumentMoveError : public std::runtime_error {
 public:
  explicit InvalidDocumentMoveError(const std::string& reason)
      : std::runtime_error(reason) {}
};

struct DocumentInput {
  std::string title;
  std::vector<std::string> tags;
  std::string visibility;  // "public" | "private"; anything else -> private
  std::string type;
  std::string body;
};

struct DocumentRecord {
  std::string path;
  FrontMatter frontMatter;
  std::string body;
};

// Orchestrates a document write: assemble front matter, serialize, write
// atomically through VaultRepository, then sync the SQLite FTS/tags index
// via IndexUpdater — in that order, so the index is never updated for
// content that failed to actually land on disk. Embedding inference is
// queued in the background after that FTS write (see IndexUpdater::
// upsertOne) so a Save is not held open for ARM-speed model inference.
// Front matter fields not under the
// caller's control (id, created/updated timestamps) are managed here, not
// by the caller.
class DocumentService {
 public:
  DocumentService(VaultRepository& vault, index::IndexUpdater& indexUpdater,
                   index::SnapshotStore& snapshots)
      : vault_(vault), indexUpdater_(indexUpdater), snapshots_(snapshots) {}

  // Throws DocumentAlreadyExistsError if `relativePath` is already
  // occupied, PathTraversalError if it escapes the vault. Assigns a fresh
  // UUID; created == updated == now. A path whose last segment has no
  // `.md` extension gets `.md` appended (and a case-insensitive `.MD`
  // is canonicalized to `.md`) — IndexBuilder/VaultWatcher only walk
  // `.md` files, so creating `notes/foo` without the suffix would
  // otherwise vanish from the index.
  DocumentRecord create(const std::string& relativePath, const DocumentInput& input);

  // Throws DocumentNotFoundError if `relativePath` doesn't exist yet.
  // Preserves the existing id/created timestamp; sets updated = now.
  DocumentRecord update(const std::string& relativePath, const DocumentInput& input);

  // Reads + parses one document (front matter + body). Throws
  // DocumentNotFoundError if it doesn't exist.
  DocumentRecord get(const std::string& relativePath) const;

  // Moves the document (and its co-located `<stem>.assets/` folder, if
  // any) from `oldRelativePath` to `newRelativePath`. Both paths get the
  // same ".md" suffixing as create() (forgotten extension is appended;
  // `.MD` is canonicalized to `.md`). PathGuard on both. After the
  // filesystem move: the index row is re-pathed in place (rowid kept, so
  // snapshots/history survive), inbound `[[wiki-link]]`s in other
  // documents are rewritten to the new path (label preserved), markdown
  // hrefs into the old `.assets/` folder are rewritten, and every
  // rewritten source is reindexed. Front-matter `updated` is not bumped
  // — this is a path change, not an edit.
  //
  // Throws DocumentNotFoundError if the source isn't there,
  // DocumentAlreadyExistsError if the destination is occupied,
  // InvalidDocumentMoveError if either path is empty after trimming
  // (refusing to invent a document named ".md"), PathTraversalError if
  // either path escapes the vault. Same-path after normalization is a
  // no-op and returns the existing record.
  DocumentRecord rename(const std::string& oldRelativePath,
                        const std::string& newRelativePath);

  // Moves the document to .trash/ and removes it from the index. Throws
  // DocumentNotFoundError if it doesn't exist.
  void softDelete(const std::string& relativePath);

 private:
  DocumentRecord writeAndIndex(const std::string& relativePath,
                                const DocumentInput& input, FrontMatter fm);

  VaultRepository& vault_;
  index::IndexUpdater& indexUpdater_;
  index::SnapshotStore& snapshots_;
};

}  // namespace wikicore::vault
