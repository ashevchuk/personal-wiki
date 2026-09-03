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

}  // namespace wikicore::index
