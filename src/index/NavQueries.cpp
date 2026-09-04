#include "index/NavQueries.h"

#include "index/Statement.h"

namespace wikicore::index {

std::vector<DocSummary> NavQueries::listVisibleDocuments(bool includePrivate) const {
  Statement stmt(db_.handle(),
                  "SELECT path, title, visibility FROM documents "
                  "WHERE (?1 = 1 OR visibility = 'public') "
                  "ORDER BY path;");
  stmt.bind(1, static_cast<int64_t>(includePrivate ? 1 : 0));

  std::vector<DocSummary> results;
  while (stmt.step()) {
    results.push_back(
        DocSummary{stmt.columnText(0), stmt.columnText(1), stmt.columnText(2)});
  }
  return results;
}

std::vector<TagCount> NavQueries::tagCounts(bool includePrivate) const {
  Statement stmt(
      db_.handle(),
      "SELECT t.name, COUNT(*) FROM tags t "
      "JOIN document_tags dt ON dt.tag_id = t.id "
      "JOIN documents d ON d.rowid_id = dt.document_rowid "
      "WHERE (?1 = 1 OR d.visibility = 'public') "
      "GROUP BY t.name "
      "ORDER BY COUNT(*) DESC, t.name ASC;");
  stmt.bind(1, static_cast<int64_t>(includePrivate ? 1 : 0));

  std::vector<TagCount> results;
  while (stmt.step()) {
    results.push_back(TagCount{stmt.columnText(0), stmt.columnInt64(1)});
  }
  return results;
}

std::vector<TagCount> NavQueries::typeCounts(bool includePrivate) const {
  // doc_type is a plain nullable/empty TEXT column on documents (no join
  // table, unlike tags) — an untyped document stores "" there (see
  // IndexUpdater::upsertOne, always binds entry.docType as a string,
  // never NULL), so it's excluded explicitly rather than showing up as a
  // bogus blank option in the search page's type dropdown.
  Statement stmt(
      db_.handle(),
      "SELECT d.doc_type, COUNT(*) FROM documents d "
      "WHERE d.doc_type <> '' AND (?1 = 1 OR d.visibility = 'public') "
      "GROUP BY d.doc_type "
      "ORDER BY COUNT(*) DESC, d.doc_type ASC;");
  stmt.bind(1, static_cast<int64_t>(includePrivate ? 1 : 0));

  std::vector<TagCount> results;
  while (stmt.step()) {
    results.push_back(TagCount{stmt.columnText(0), stmt.columnInt64(1)});
  }
  return results;
}

}  // namespace wikicore::index
