#include "index/SnapshotStore.h"

#include "index/Statement.h"
#include "util/Time.h"

namespace wikicore::index {

void SnapshotStore::record(int64_t documentRowId, const std::string& content) {
  Statement insert(db_.handle(),
                    "INSERT INTO document_snapshots(document_rowid, snapshot_at, content) "
                    "VALUES (?1, ?2, ?3);");
  insert.bind(1, documentRowId).bind(2, util::nowIso8601()).bind(3, content);
  insert.run();
}

std::vector<SnapshotSummary> SnapshotStore::list(int64_t documentRowId) const {
  Statement stmt(db_.handle(),
                  "SELECT id, snapshot_at FROM document_snapshots "
                  "WHERE document_rowid = ?1 ORDER BY id DESC;");
  stmt.bind(1, documentRowId);

  std::vector<SnapshotSummary> results;
  while (stmt.step()) {
    results.push_back(SnapshotSummary{stmt.columnInt64(0), stmt.columnText(1)});
  }
  return results;
}

std::optional<std::string> SnapshotStore::getContent(int64_t documentRowId,
                                                       int64_t snapshotId) const {
  // Both columns in the WHERE clause, not just id — see the header's own
  // comment on why the document_rowid check is load-bearing, not
  // defensive-for-its-own-sake.
  Statement stmt(db_.handle(),
                  "SELECT content FROM document_snapshots "
                  "WHERE id = ?1 AND document_rowid = ?2;");
  stmt.bind(1, snapshotId).bind(2, documentRowId);
  if (stmt.step()) return stmt.columnText(0);
  return std::nullopt;
}

}  // namespace wikicore::index
