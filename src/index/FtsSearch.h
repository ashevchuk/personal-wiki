#pragma once

#include "index/Database.h"

#include <optional>
#include <string>
#include <vector>

namespace wikicore::index {

struct SearchQuery {
  std::string text;               // FTS5 MATCH pattern; empty = "browse" mode
  std::optional<std::string> tag;
  // AND semantics: a matching document must carry every tag listed here.
  // Independent of `tag` above (both can be set; used by different
  // callers — the HTTP search API sets `tag`, the MCP tools set `tags`).
  std::vector<std::string> tags;
  std::optional<std::string> docType;
  // OR semantics — unlike `tags` above, a document carries exactly ONE
  // doc_type, so "must match every requested type" would be
  // unsatisfiable for anything past the first. Populated by the
  // search-page multiselect (SearchRoutes.cpp splits the `type` query
  // param on ','); independent of `docType`, same as `tag`/`tags`.
  std::vector<std::string> docTypes;
  // Path prefix, e.g. "notes/" — matches "notes/foo.md" and
  // "notes/sub/bar.md" alike. Used by the MCP list_documents tool's
  // `folder` parameter.
  std::optional<std::string> folderPrefix;
  // Set true only when the caller is an authenticated admin (HTTP) or the
  // configured MCP scope is "admin" (stdio) — this is what keeps a
  // private document's title/snippet out of results, matching the
  // fail-safe-private rule everywhere else (see docs/architecture.md).
  bool includePrivate = false;
  int limit = 50;
  int offset = 0;
};

struct SearchResultItem {
  std::string path;
  std::string title;
  std::string visibility;
  std::string updatedAt;
  std::string docType;
  std::vector<std::string> tags;
  // Plain text, NOT HTML-safe — comes straight from the document body via
  // FTS5's snippet() (when `text` was non-empty) or the stored excerpt
  // otherwise. The caller MUST escape it before rendering. When it came
  // from snippet(), matched terms are delimited by the raw bytes
  // kSnippetMatchStart/kSnippetMatchEnd (see FtsSearch.cpp) rather than
  // literal "<mark>" — escaping first, then swapping those markers for
  // "<mark>"/"</mark>", is what keeps this from being an XSS hole:
  // escaping AFTER inserting real HTML tags would mangle the tags;
  // skipping escaping to preserve them would let arbitrary body content
  // through unescaped.
  std::string snippet;
  bool snippetIsHighlighted = false;  // true only for the FTS5 snippet() path
};

// Read-only search over the FTS5 index. Two modes: `text` non-empty uses
// FTS5 MATCH + bm25() ranking + snippet(); `text` empty just lists
// documents (newest-updated first) matching the tag/type filters, for
// plain browsing without a query.
class FtsSearch {
 public:
  // Non-printable bytes used as snippet() match delimiters instead of
  // literal "<mark>"/"</mark>" — see SearchResultItem::snippet for why.
  // \x01/\x02 (SOH/STX) chosen simply because real document text will
  // never contain them.
  static constexpr char kSnippetMatchStart = '\x01';
  static constexpr char kSnippetMatchEnd = '\x02';

  explicit FtsSearch(Database& db) : db_(db) {}

  std::vector<SearchResultItem> search(const SearchQuery& query) const;

 private:
  Database& db_;
};

}  // namespace wikicore::index
