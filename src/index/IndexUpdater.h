#pragma once

#include "index/Database.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wikicore::index {

struct DocumentIndexEntry {
  std::string uuid;
  std::string path;  // vault-relative, unique
  std::string title;
  std::string docType;
  std::string visibility;  // "public" | "private"
  std::string createdAt;
  std::string updatedAt;
  int64_t fileMtime = 0;
  int64_t fileSize = 0;
  std::string excerpt;
  std::vector<std::string> tags;
  std::string body;  // fed into documents_fts, not stored as its own column
};

// Keeps the SQLite index in sync with one document at a time, as it's
// saved/deleted through the Web UI (DocumentService calls this after every
// write). This is deliberately NOT the full-vault rescan (that's
// IndexBuilder, Milestone 3) — it only ever touches the one row it's told
// about.
class IndexUpdater {
 public:
  explicit IndexUpdater(Database& db) : db_(db) {}

  // Inserts the row for entry.path if new, or updates it in place if the
  // path is already indexed (path is UNIQUE). Replaces the document's tag
  // set and FTS entry to match `entry` exactly. Returns the row's
  // rowid_id. Runs as a single transaction.
  int64_t upsertOne(const DocumentIndexEntry& entry);

  // Removes the row at `path` (tags/attachments/FTS entry cascade via the
  // schema's ON DELETE CASCADE / explicit FTS cleanup below). No-op if the
  // path isn't indexed.
  void removeOne(const std::string& path);

 private:
  Database& db_;
};

}  // namespace wikicore::index
