#pragma once

#include "index/Database.h"

#include <cstdint>
#include <string>
#include <vector>

namespace wikicore::index {

struct DocSummary {
  std::string path;
  std::string title;
  std::string visibility;
};

struct TagCount {
  std::string tag;
  int64_t count;
};

// Read-only queries backing the nav tree and tag cloud. Both are
// visibility-aware in the same fail-safe-private direction as everything
// else: with includePrivate=false, a private document contributes
// nothing — not its path/title to the tree, not a count to a tag it
// carries. A tag used only by private documents simply doesn't appear at
// all for an anonymous caller, rather than showing up with a
// suspiciously nonzero count for something they can't open.
class NavQueries {
 public:
  explicit NavQueries(Database& db) : db_(db) {}

  // Ordered by path — callers build a folder tree by splitting on '/'.
  std::vector<DocSummary> listVisibleDocuments(bool includePrivate) const;

  std::vector<TagCount> tagCounts(bool includePrivate) const;

  // Same visibility-gated shape as tagCounts, over documents.doc_type
  // instead of the tags join table — backs the search page's type
  // multiselect (see NavRoutes.h). Reuses TagCount as the return element
  // (field named `tag` there, holds a doc_type value here) rather than
  // introducing a near-identical struct for one extra column.
  std::vector<TagCount> typeCounts(bool includePrivate) const;

  // Every document that links to `targetPath` via a [[wiki-link]] (see
  // src/util/WikiLinks.h), visibility-gated the same fail-safe-private
  // way as everything else here: a PRIVATE document's outgoing link
  // never appears to an anonymous caller, not even as a count — same
  // discipline as tagCounts not leaking a tag used only by private docs.
  // `targetPath` is compared as stored (already normalized by whoever's
  // asking — DocumentRoutes.cpp uses the request's own docPath, which is
  // already a real, normalized vault-relative path).
  std::vector<DocSummary> backlinks(const std::string& targetPath, bool includePrivate) const;

 private:
  Database& db_;
};

}  // namespace wikicore::index
