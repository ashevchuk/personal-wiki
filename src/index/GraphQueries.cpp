#include "index/GraphQueries.h"

#include "index/Statement.h"

#include <map>
#include <optional>
#include <utility>

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

std::optional<GraphNeighborhood> GraphQueries::around(
    const std::string& centerPath, bool includePrivate) const {
  const int64_t vis = includePrivate ? 1 : 0;

  GraphNode center;
  {
    // Exact path bind, never LIKE — around= is untrusted caller input
    // (a query param), and a wildcard here would turn "%"/ "_" into a
    // neighborhood dump rather than a miss.
    Statement stmt(db_.handle(),
                    "SELECT path, title, visibility, doc_type FROM documents "
                    "WHERE path = ?1 AND (?2 = 1 OR visibility = 'public');");
    stmt.bind(1, centerPath);
    stmt.bind(2, vis);
    if (!stmt.step()) return std::nullopt;
    center = GraphNode{stmt.columnText(0), stmt.columnText(1),
                       stmt.columnText(2), stmt.columnText(3)};
  }

  // UNION (not UNION ALL) is the visited set: a cycle cannot re-enqueue
  // a path already in the component. Visibility is re-checked on every
  // hop so a private document is not a stepping stone that would leak a
  // further public one to an anonymous caller. The outer SELECT is the
  // induced subgraph (every visible edge whose both ends are in the
  // component), not just the BFS tree.
  Statement stmt(
      db_.handle(),
      "WITH RECURSIVE component(path) AS ("
      "  SELECT path FROM documents "
      "  WHERE path = ?1 AND (?2 = 1 OR visibility = 'public') "
      "  UNION "
      "  SELECT CASE WHEN src.path = c.path THEN tgt.path ELSE src.path END "
      "  FROM component c "
      "  JOIN documents me ON me.path = c.path "
      "  JOIN document_links dl "
      "    ON dl.source_rowid = me.rowid_id OR dl.target_path = me.path "
      "  JOIN documents src ON src.rowid_id = dl.source_rowid "
      "  JOIN documents tgt ON tgt.path = dl.target_path "
      "  WHERE (?2 = 1 OR src.visibility = 'public') "
      "    AND (?2 = 1 OR tgt.visibility = 'public')"
      ") "
      "SELECT src.path, src.title, src.visibility, src.doc_type, "
      "       tgt.path, tgt.title, tgt.visibility, tgt.doc_type "
      "FROM document_links dl "
      "JOIN documents src ON src.rowid_id = dl.source_rowid "
      "JOIN documents tgt ON tgt.path = dl.target_path "
      "WHERE src.path IN (SELECT path FROM component) "
      "  AND tgt.path IN (SELECT path FROM component) "
      "  AND (?2 = 1 OR src.visibility = 'public') "
      "  AND (?2 = 1 OR tgt.visibility = 'public');");
  stmt.bind(1, centerPath);
  stmt.bind(2, vis);

  // map keeps nodes ordered by path, same as nodes()'s ORDER BY path.
  std::map<std::string, GraphNode> byPath;
  byPath.emplace(center.path, center);
  std::vector<GraphEdge> edges;
  while (stmt.step()) {
    GraphNode src{stmt.columnText(0), stmt.columnText(1),
                  stmt.columnText(2), stmt.columnText(3)};
    GraphNode tgt{stmt.columnText(4), stmt.columnText(5),
                  stmt.columnText(6), stmt.columnText(7)};
    edges.push_back(GraphEdge{src.path, tgt.path});
    byPath.insert_or_assign(src.path, std::move(src));
    byPath.insert_or_assign(tgt.path, std::move(tgt));
  }

  GraphNeighborhood out;
  out.nodes.reserve(byPath.size());
  for (auto& kv : byPath) {
    out.nodes.push_back(std::move(kv.second));
  }
  out.edges = std::move(edges);
  return out;
}

}  // namespace wikicore::index
