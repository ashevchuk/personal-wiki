#include "index/IndexUpdater.h"

#include "index/Statement.h"
#include "util/WikiLinks.h"

#ifdef WIKI_ENABLE_SQLITE_VEC
#include "index/EmbeddingIndexer.h"

#include <functional>
#include <iomanip>
#include <sstream>
#endif

#include <optional>

namespace wikicore::index {

namespace {

std::string joinTags(const std::vector<std::string>& tags) {
  std::string flat;
  for (const auto& t : tags) {
    if (!flat.empty()) flat += ' ';
    flat += t;
  }
  return flat;
}

std::optional<int64_t> findRowIdByPath(Database& db, const std::string& path) {
  Statement stmt(db.handle(), "SELECT rowid_id FROM documents WHERE path = ?1;");
  stmt.bind(1, path);
  if (stmt.step()) return stmt.columnInt64(0);
  return std::nullopt;
}

#ifdef WIKI_ENABLE_SQLITE_VEC
// Deliberately std::hash, not a cryptographic hash (e.g.
// auth::sha256Hex) — this exists purely to detect "did the text used
// for embedding change since last time", not anything
// security-sensitive, and pulling in OpenSSL here would violate
// wikicore's own "no Drogon/OpenSSL/auth dependency" boundary (see
// CLAUDE.md) for a change-detection checksum that has no adversarial
// model at all. A collision would at worst skip a re-embed that should
// have happened — recoverable by any future edit to the document (which
// changes the hash again) or a --reindex forcing model/dimension
// tracking to reset, not a security hole.
std::string contentHashForEmbedding(const std::string& title, const std::string& body) {
  const std::size_t h = std::hash<std::string>{}(title + "\n\n" + body);
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(sizeof(std::size_t) * 2) << h;
  return out.str();
}
#endif

int64_t findOrCreateTagId(Database& db, const std::string& name) {
  {
    Statement select(db.handle(), "SELECT id FROM tags WHERE name = ?1;");
    select.bind(1, name);
    if (select.step()) return select.columnInt64(0);
  }
  Statement insert(db.handle(), "INSERT INTO tags(name) VALUES (?1);");
  insert.bind(1, name);
  insert.run();
  return insert.lastInsertRowId();
}

void replaceTagLinks(Database& db, int64_t documentRowId,
                      const std::vector<std::string>& tags) {
  Statement clear(db.handle(),
                   "DELETE FROM document_tags WHERE document_rowid = ?1;");
  clear.bind(1, documentRowId);
  clear.run();

  for (const auto& tag : tags) {
    const int64_t tagId = findOrCreateTagId(db, tag);
    Statement link(db.handle(),
                    "INSERT INTO document_tags(document_rowid, tag_id) "
                    "VALUES (?1, ?2);");
    link.bind(1, documentRowId).bind(2, tagId);
    link.run();
  }
}

// Mirrors replaceTagLinks below/above -- delete-then-reinsert the full
// set rather than diffing, same reasoning: this only ever runs on a
// single document's own save/rescan, never a hot path worth optimizing
// for incremental updates.
void replaceLinkRows(Database& db, int64_t documentRowId, const std::string& body) {
  Statement clear(db.handle(),
                   "DELETE FROM document_links WHERE source_rowid = ?1;");
  clear.bind(1, documentRowId);
  clear.run();

  for (const auto& target : wikicore::util::extractWikiLinkTargets(body)) {
    Statement link(db.handle(),
                    "INSERT INTO document_links(source_rowid, target_path) "
                    "VALUES (?1, ?2);");
    link.bind(1, documentRowId).bind(2, target);
    link.run();
  }
}

void replaceFtsEntry(Database& db, int64_t documentRowId,
                      const DocumentIndexEntry& entry) {
  Statement clear(db.handle(), "DELETE FROM documents_fts WHERE rowid = ?1;");
  clear.bind(1, documentRowId);
  clear.run();

  Statement insert(db.handle(),
                    "INSERT INTO documents_fts(rowid, title, body, tags_flat) "
                    "VALUES (?1, ?2, ?3, ?4);");
  insert.bind(1, documentRowId)
      .bind(2, entry.title)
      .bind(3, entry.body)
      .bind(4, joinTags(entry.tags));
  insert.run();
}

}  // namespace

int64_t IndexUpdater::upsertOne(const DocumentIndexEntry& entry) {
  // rowId is set inside the locked block below and used again afterward
  // (by the embedding step) once the lock has already been released —
  // see the comment right after this scope closes for why that step
  // deliberately runs unlocked.
  int64_t rowId;

  {
  // See mutex_'s doc comment in IndexUpdater.h -- this whole
  // BEGIN...COMMIT/ROLLBACK sequence must run as one unit against `db_`,
  // never interleaved with another thread's own BEGIN on the same
  // connection.
  std::lock_guard<std::mutex> lock(mutex_);

  Statement begin(db_.handle(), "BEGIN IMMEDIATE;");
  begin.run();

  try {
    const auto existingRowId = findRowIdByPath(db_, entry.path);

    if (existingRowId) {
      rowId = *existingRowId;
      Statement update(db_.handle(),
                        "UPDATE documents SET uuid=?1, title=?2, doc_type=?3, "
                        "visibility=?4, created_at=?5, updated_at=?6, "
                        "file_mtime=?7, file_size=?8, excerpt=?9 "
                        "WHERE rowid_id = ?10;");
      update.bind(1, entry.uuid)
          .bind(2, entry.title)
          .bind(3, entry.docType)
          .bind(4, entry.visibility)
          .bind(5, entry.createdAt)
          .bind(6, entry.updatedAt)
          .bind(7, entry.fileMtime)
          .bind(8, entry.fileSize)
          .bind(9, entry.excerpt)
          .bind(10, rowId);
      update.run();
    } else {
      Statement insert(
          db_.handle(),
          "INSERT INTO documents(uuid, path, title, doc_type, visibility, "
          "created_at, updated_at, file_mtime, file_size, excerpt) "
          "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);");
      insert.bind(1, entry.uuid)
          .bind(2, entry.path)
          .bind(3, entry.title)
          .bind(4, entry.docType)
          .bind(5, entry.visibility)
          .bind(6, entry.createdAt)
          .bind(7, entry.updatedAt)
          .bind(8, entry.fileMtime)
          .bind(9, entry.fileSize)
          .bind(10, entry.excerpt);
      insert.run();
      rowId = insert.lastInsertRowId();
    }

    replaceTagLinks(db_, rowId, entry.tags);
    replaceFtsEntry(db_, rowId, entry);
    replaceLinkRows(db_, rowId, entry.body);

    Statement commit(db_.handle(), "COMMIT;");
    commit.run();
  } catch (...) {
    Statement rollback(db_.handle(), "ROLLBACK;");
    rollback.run();
    throw;
  }
  }  // lock released here — FTS/tags/documents are committed and

  // authoritative regardless of what happens below.

#ifdef WIKI_ENABLE_SQLITE_VEC
  // Deliberately best-effort throughout: an embedding failure (API down,
  // rate limited, model error) must never fail the document save itself
  // — the FTS5 index committed just above already IS the authoritative
  // search path (see docs/architecture.md's storage model), semantic
  // search is strictly additive to it. Unlike the very first version of
  // this code, a failure now ALSO records itself (EmbeddingIndexer's
  // document_embedding_state) so an admin can see and manually retry it
  // — see docs/embeddings.md's "Skip-if-unchanged and self-healing" —
  // rather than the only visibility being "semantic search silently
  // doesn't find this one document".
  if (provider_ != nullptr) {
    // Step 1: is an embed() call even worth attempting? ensureTable()
    // must run first (cheap — a no-op when nothing changed) because a
    // provider/model swap since the last run clears
    // document_embedding_state entirely; needsEmbedding() has to see
    // that POST-clear state, not a stale pre-clear one.
    std::string currentHash;
    bool needsEmbed = true;
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      EmbeddingIndexer indexer(db_.handle());
      indexer.ensureTable(provider_->dimensions(), provider_->modelIdentifier());
      currentHash = contentHashForEmbedding(entry.title, entry.body);
      needsEmbed = indexer.needsEmbedding(rowId, currentHash);
    } catch (...) {
      // ensureTable() itself hit a real sqlite error — fall through and
      // let the embed attempt below run anyway; its own failure path
      // records something actionable instead of this step silently
      // deciding "nothing to do".
    }

    if (needsEmbed) {
      // Step 2: the actual embed() call — deliberately OUTSIDE mutex_. A
      // network call for the cloud provider, or real model inference for
      // local, must never hold the lock other threads' document/FTS
      // saves or their own embedding writes are waiting on (see mutex_'s
      // own comment in the header for why this is the ONLY step that
      // stays unlocked — every actual DB write, embedding-related or
      // not, uses the same mutex_).
      try {
        const auto embedding = provider_->embed(entry.title + "\n\n" + entry.body);
        std::lock_guard<std::mutex> lock(mutex_);
        EmbeddingIndexer indexer(db_.handle());
        indexer.upsertOne(rowId, embedding);
        indexer.recordEmbeddingSuccess(rowId, currentHash);
      } catch (const std::exception& e) {
        try {
          std::lock_guard<std::mutex> lock(mutex_);
          EmbeddingIndexer(db_.handle()).recordEmbeddingFailure(rowId, e.what());
        } catch (...) {
          // Even recording the failure failed (a real sqlite error) —
          // nothing left to do but let this document stay silently
          // unindexed for now; the NEXT rescan/save attempt still finds
          // needsEmbed=true (no successful hash was ever recorded) and
          // tries again.
        }
      } catch (...) {
        try {
          std::lock_guard<std::mutex> lock(mutex_);
          EmbeddingIndexer(db_.handle())
              .recordEmbeddingFailure(rowId, "unknown error (non-std::exception thrown)");
        } catch (...) {
        }
      }
    }
  }
#endif

  return rowId;
}

std::vector<std::string> IndexUpdater::allIndexedPaths() const {
  Statement stmt(db_.handle(), "SELECT path FROM documents;");
  std::vector<std::string> paths;
  while (stmt.step()) {
    paths.push_back(stmt.columnText(0));
  }
  return paths;
}

std::optional<int64_t> IndexUpdater::rowIdForPath(const std::string& path) const {
  return findRowIdByPath(db_, path);
}

std::optional<std::string> IndexUpdater::findPathByUuid(const std::string& uuid) const {
  Statement stmt(db_.handle(), "SELECT path FROM documents WHERE uuid = ?1;");
  stmt.bind(1, uuid);
  if (stmt.step()) return stmt.columnText(0);
  return std::nullopt;
}

void IndexUpdater::removeOne(const std::string& path) {
  std::optional<int64_t> rowId;

  {
  // See mutex_'s doc comment in IndexUpdater.h.
  std::lock_guard<std::mutex> lock(mutex_);

  Statement begin(db_.handle(), "BEGIN IMMEDIATE;");
  begin.run();
  try {
    rowId = findRowIdByPath(db_, path);
    if (rowId) {
      Statement clearFts(db_.handle(),
                          "DELETE FROM documents_fts WHERE rowid = ?1;");
      clearFts.bind(1, *rowId);
      clearFts.run();

      // document_tags/attachments cascade via ON DELETE CASCADE
      // (PRAGMA foreign_keys = ON is set for every connection, see
      // Database::Database).
      Statement del(db_.handle(), "DELETE FROM documents WHERE rowid_id = ?1;");
      del.bind(1, *rowId);
      del.run();
    }
    Statement commit(db_.handle(), "COMMIT;");
    commit.run();
  } catch (...) {
    Statement rollback(db_.handle(), "ROLLBACK;");
    rollback.run();
    throw;
  }
  }  // lock released — EmbeddingIndexer::removeOne() below opens its OWN
     // BEGIN/COMMIT on this same connection; nesting it inside the
     // transaction just committed above would be a second BEGIN on an
     // already-open one, a real sqlite error, not just untidy.

#ifdef WIKI_ENABLE_SQLITE_VEC
  if (provider_ != nullptr && rowId.has_value()) {
    std::lock_guard<std::mutex> lock(mutex_);
    EmbeddingIndexer indexer(db_.handle());
    indexer.removeOne(*rowId);
  }
#endif
}

}  // namespace wikicore::index
