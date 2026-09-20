#pragma once

#include "index/Database.h"
#include "index/FtsSearch.h"

#include <string>
#include <vector>

namespace wikicore::index {

struct QueryResultRow {
  std::string path;
  std::string title;
  std::string visibility;
  std::string updatedAt;
  std::string tagsFlat;  // comma-separated, for display only
};

// Deliberately not just "rows, empty on no match" -- a typo'd key (e.g.
// "tags:" instead of "tag:") must surface as an explicit error, never as
// a silently wrong (empty, or unfiltered) result set. Same class of
// mistake `~/.claude/CLAUDE.md`'s own global "don't let a helper's
// silent failure read as a normal result" rule warns about, just for a
// hand-written DSL parser instead of a shelled-out script.
struct QueryBlockResult {
  bool ok = false;
  std::string error;
  std::vector<QueryResultRow> rows;
};

// Executes a ```query fenced block's body: a tiny, fully whitelisted
// `key: value` per-line DSL, NEVER raw SQL. See QueryBlocks.cpp's own
// comment for the exact grammar and, more importantly, why every
// recognized key maps to a fixed, hardcoded SQL fragment with the value
// only ever passed as a bound parameter -- there is no code path from
// the block's text to SQL text, structurally, not just "at the moment,
// carefully."
class QueryBlocks {
 public:
  // ftsSearch: the SAME FtsSearch instance /api/search already uses --
  // not owned, must outlive this QueryBlocks (matches FtsSearch's own
  // provider-pointer convention). Reused rather than a second copy of
  // FTS5/hybrid-ranking logic: `search:` in the DSL below delegates
  // straight to it, getting the exact same BM25 + semantic (RRF) ranking,
  // query-embedding cache, and distance/candidate-count tuning /search
  // already has, for free.
  QueryBlocks(Database& db, FtsSearch& ftsSearch) : db_(db), ftsSearch_(ftsSearch) {}

  // `includePrivate` is the SAME fail-safe-private gate every other
  // read-only query in this codebase uses (see NavQueries) -- the
  // caller's own auth state, never anything the query block's text
  // itself can influence. A query block embedded in a PUBLIC document
  // must never be able to leak a private document's title/path to an
  // anonymous viewer just because the admin who wrote the block could
  // see it themselves at authoring time.
  QueryBlockResult parseAndRun(const std::string& raw, bool includePrivate) const;

 private:
  Database& db_;
  FtsSearch& ftsSearch_;
};

}  // namespace wikicore::index
