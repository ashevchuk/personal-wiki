#pragma once

#include "index/Database.h"

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

// Backs both the full graph page and the per-document local graph
// widget — the local view is computed CLIENT-SIDE (fetch the whole
// graph once, filter to N hops of the current document in JS) rather
// than a second server-side query, deliberately: at this app's actual
// scale (a personal vault, tens of documents, never the thousands a
// generic PKM tool has to plan for), the whole graph is cheap enough to
// send every time, and a second BFS-shaped query would be real
// complexity paid for a saving that doesn't matter yet. Revisit if a
// real vault ever gets big enough for this to actually cost something.
//
// Same fail-safe-private visibility gating as NavQueries throughout:
// includePrivate is the caller's own session state, never anything a
// document's content can influence. An edge is only included when BOTH
// endpoints are visible to this caller — a private document must not
// leak its existence via an edge pointing at (or from) it, even toward
// a public document on the other end.
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

 private:
  Database& db_;
};

}  // namespace wikicore::index
