#include "index/IndexUpdater.h"

#include "index/Statement.h"
#include "util/WikiLinks.h"

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
  Statement begin(db_.handle(), "BEGIN IMMEDIATE;");
  begin.run();

  try {
    const auto existingRowId = findRowIdByPath(db_, entry.path);
    int64_t rowId;

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
    return rowId;
  } catch (...) {
    Statement rollback(db_.handle(), "ROLLBACK;");
    rollback.run();
    throw;
  }
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
  Statement begin(db_.handle(), "BEGIN IMMEDIATE;");
  begin.run();
  try {
    const auto rowId = findRowIdByPath(db_, path);
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
}

}  // namespace wikicore::index
