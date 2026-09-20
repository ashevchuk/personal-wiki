#pragma once

#include "embeddings/EmbeddingProvider.h"
#include "index/Database.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// Read-only search over the FTS5 index. Three modes: `text` non-empty
// with no embedding provider configured uses FTS5 MATCH + bm25() ranking
// + snippet() (unchanged from before hybrid search existed); `text`
// non-empty WITH a provider configured additionally blends in semantic
// (cosine, via EmbeddingIndexer) ranking — see search()'s own comment for
// how; `text` empty just lists documents (newest-updated first) matching
// the tag/type filters, for plain browsing without a query — never
// touches embeddings either way.
class FtsSearch {
 public:
  // Non-printable bytes used as snippet() match delimiters instead of
  // literal "<mark>"/"</mark>" — see SearchResultItem::snippet for why.
  // \x01/\x02 (SOH/STX) chosen simply because real document text will
  // never contain them.
  static constexpr char kSnippetMatchStart = '\x01';
  static constexpr char kSnippetMatchEnd = '\x02';

  // provider: optional (nullptr = FTS5-only ranking, matches
  // embeddings.provider="none" or a build without WIKI_ENABLE_SQLITE_VEC
  // — see docs/embeddings.md). Not owned — must outlive this FtsSearch.
  // A failed embed() call during search (network error, etc.) is caught
  // and the search silently falls back to FTS5-only results for that one
  // call — same "additive, never load-bearing" reasoning as
  // IndexUpdater's own embedding step (see that class's comment).
  //
  // maxSemanticDistance: cosine-distance cutoff (see AppConfig::
  // embeddingsMaxDistance's own comment for why this exists — without
  // it, a small vault's "nearest neighbors" is effectively the whole
  // vault, and every one of them gets a nonzero RRF score regardless of
  // actual relevance). Found live on real production content.
  //
  // maxSemanticCandidates: a hard cap on how many of the nearest
  // neighbors (already sorted nearest-first, already past the distance
  // cutoff above) are even allowed to become RRF candidates — see
  // AppConfig::embeddingsSemanticTopK's own comment for why the distance
  // cutoff alone isn't enough: a multi-word query's embedding can sit at
  // a "blurry" distance from MANY unrelated documents at once on a small
  // vault, all of them individually under the cutoff, and RRF has no way
  // to reject a candidate once it's in the list, only rank it. Found
  // live re-checking the distance-cutoff fix against other real
  // production queries.
  explicit FtsSearch(Database& db, embeddings::EmbeddingProvider* provider = nullptr,
                      double maxSemanticDistance = 0.5, int maxSemanticCandidates = 5)
      : db_(db),
        provider_(provider),
        maxSemanticDistance_(maxSemanticDistance),
        maxSemanticCandidates_(maxSemanticCandidates) {}

  std::vector<SearchResultItem> search(const SearchQuery& query) const;

 private:
#ifdef WIKI_ENABLE_SQLITE_VEC
  // Attempts hybrid (BM25 + semantic) ranking; std::nullopt on anything
  // that should fall back to plain FTS5 instead of failing the whole
  // search — no document_embeddings table yet, or provider_->embed()
  // itself failing (network error for the cloud provider, etc.).
  std::optional<std::vector<SearchResultItem>> tryHybridSearch(
      const SearchQuery& query, const std::string& matchExpr) const;

  // rowid-only BM25 ranking (same filters/weights as the plain-FTS5 path
  // in search(), just without the metadata SELECT) — used to build one
  // of the two ranked lists reciprocalRankFusion() (FtsSearch.cpp)
  // combines.
  std::vector<int64_t> bm25CandidateRowIds(const SearchQuery& query,
                                            const std::string& matchExpr,
                                            int candidateLimit) const;

  // Fetches full SearchResultItem metadata for exactly `orderedRowIds`,
  // returned in that SAME order (SQL's `IN (...)` makes no ordering
  // promise, so this re-sorts after the fact) — snippet() for rowids
  // that came from the BM25 side (real FTS5 MATCH context), the stored
  // excerpt for rowids found ONLY via semantic search (snippet() has no
  // defined result for a row that never matched the FTS5 query).
  //
  // Takes the full `query`, not just `includePrivate` — the tag/docType/
  // folderPrefix filters (appendCommonFilterSql()) MUST be re-applied
  // here too, not just in bm25CandidateRowIds() above. Found live: a
  // document admitted ONLY via the semantic candidate list (indexer.
  // nearest() has no concept of tag/docType/folder filters at all — see
  // EmbeddingIndexer::nearest()'s own signature) bypassed an active
  // tags/type filter entirely, since this was the one place both
  // candidate sources converge before being returned to the caller.
  std::vector<SearchResultItem> fetchByRowIds(
      const std::vector<int64_t>& orderedRowIds,
      const std::unordered_set<int64_t>& snippetEligibleRowIds, const SearchQuery& query) const;
#endif

  Database& db_;
  embeddings::EmbeddingProvider* provider_ = nullptr;
  double maxSemanticDistance_ = 0.5;
  int maxSemanticCandidates_ = 5;

  // Cache of embedQuery(query.text) results, keyed on the raw query text.
  // Found live: a single embedQuery() call costs 1.3-2.6 SECONDS on the
  // real production armv7 SBC (bge-small-en-v1.5, no GPU) — the dominant
  // cost of a hybrid search request by far, FTS5+RRF is noise next to it.
  // search.js's tag/type filter checkboxes deliberately re-run a search
  // with the SAME query text and no debounce on every toggle (see that
  // file's own comment) — a real, common interaction this cache turns
  // from "another 1.3-2.6s wait" into a map lookup.
  //
  // Correct WITHOUT tracking model identity or query_prefix in the cache
  // key: both are fixed for this FtsSearch instance's entire lifetime
  // (set once at construction, from config.toml, read only at process
  // startup) — a model swap requires a restart, which recreates this
  // object and its cache from scratch. No cross-request staleness is
  // possible.
  //
  // A hard cap, not a real LRU: /api/search doesn't require
  // authentication for public content, so an unbounded map keyed on
  // arbitrary caller-supplied query text would be a real memory-growth
  // vector on this project's resource-constrained target hardware (2GB
  // RAM measured on the real box). Clearing the whole cache on overflow
  // rather than evicting one entry is deliberately simple — personal-wiki
  // query volume doesn't come close to needing genuine LRU bookkeeping,
  // and an occasional full flush costs one more slow embedQuery() call,
  // not a correctness problem.
  static constexpr size_t kQueryEmbeddingCacheCap = 256;
  mutable std::mutex queryEmbeddingCacheMutex_;
  mutable std::unordered_map<std::string, std::vector<float>> queryEmbeddingCache_;
};

}  // namespace wikicore::index
