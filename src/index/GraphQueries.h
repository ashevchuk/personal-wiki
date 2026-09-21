#pragma once

#include "index/Database.h"

#include <optional>
#include <string>
#include <vector>

namespace wikicore::index {

struct GraphNode {
  std::string path;
  std::string title;
  std::string visibility;
  std::string docType;
};

struct GraphEdge {
  std::string source;
  std::string target;
};

struct GraphNeighborhood {
  std::vector<GraphNode> nodes;
  std::vector<GraphEdge> edges;
};

// Backs both the full graph page (`nodes`/`edges`) and the per-document
// local graph widget (`around`). Same fail-safe-private visibility
// gating as NavQueries throughout: includePrivate is the caller's own
// session state, never anything a document's content can influence. An
// edge is only included when BOTH endpoints are visible to this caller
// — a private document must not leak its existence via an edge pointing
// at (or from) it, even toward a public document on the other end.
class GraphQueries {
 public:
  explicit GraphQueries(Database& db) : db_(db) {}

  // Ordered by path, same convention as NavQueries::listVisibleDocuments.
  std::vector<GraphNode> nodes(bool includePrivate) const;

  // A [[wiki-link]] edge only ever appears here when its TARGET is a
  // real, currently-existing document -- a "red link" (see
  // src/index/schema.h's own comment on document_links.target_path)
  // has no node to draw an edge to, so it's silently excluded from the
  // graph rather than inventing a placeholder node for text that isn't
  // a document. This is a deliberate scope decision for a first version,
  // not a bug: Obsidian's own graph view draws a distinct "unresolved"
  // node for exactly this case, which this app doesn't attempt yet.
  std::vector<GraphEdge> edges(bool includePrivate) const;

  // 1-hop neighborhood of `centerPath` (the document itself plus every
  // visible document that shares a visible edge with it). std::nullopt
  // if that path is not a document visible to this caller — missing and
  // private-to-anon are the same result, so the HTTP handler can map
  // both to 404 without distinguishing them. An isolated but visible
  // document returns a neighborhood of just itself (empty edges).
  //
  // Hop count is server-fixed at 1: this is a single bound join, not a
  // recursive CTE, and GraphRoutes ignores any client `hops=` query
  // param rather than threading it through here.
  std::optional<GraphNeighborhood> around(const std::string& centerPath,
                                          bool includePrivate) const;

 private:
  Database& db_;
};

}  // namespace wikicore::index
