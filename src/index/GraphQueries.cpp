#include "index/GraphQueries.h"

#include "index/Statement.h"

namespace wikicore::index {

std::vector<GraphNode> GraphQueries::nodes(bool includePrivate) const {
  Statement stmt(db_.handle(),
                  "SELECT path, title, visibility, doc_type FROM documents "
                  "WHERE (?1 = 1 OR visibility = 'public') "
                  "ORDER BY path;");
  stmt.bind(1, static_cast<int64_t>(includePrivate ? 1 : 0));

  std::vector<GraphNode> results;
  while (stmt.step()) {
    results.push_back(GraphNode{stmt.columnText(0), stmt.columnText(1),
                                 stmt.columnText(2), stmt.columnText(3)});
  }
  return results;
}

std::vector<GraphEdge> GraphQueries::edges(bool includePrivate) const {
  Statement stmt(
      db_.handle(),
      "SELECT src.path, tgt.path FROM document_links dl "
      "JOIN documents src ON src.rowid_id = dl.source_rowid "
      "JOIN documents tgt ON tgt.path = dl.target_path "
      "WHERE (?1 = 1 OR src.visibility = 'public') "
      "AND (?1 = 1 OR tgt.visibility = 'public');");
  stmt.bind(1, static_cast<int64_t>(includePrivate ? 1 : 0));

  std::vector<GraphEdge> results;
  while (stmt.step()) {
    results.push_back(GraphEdge{stmt.columnText(0), stmt.columnText(1)});
  }
  return results;
}

}  // namespace wikicore::index
