#include "index/FtsSearch.h"

#include "index/Statement.h"

#ifdef WIKI_ENABLE_SQLITE_VEC
#include "index/EmbeddingIndexer.h"
#include "index/EmbeddingsRuntimeConfig.h"
#endif

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace wikicore::index {

namespace {

std::vector<std::string> splitCsv(const std::string& csv) {
  std::vector<std::string> out;
  if (csv.empty()) return out;
  std::stringstream ss(csv);
  std::string item;
  while (std::getline(ss, item, ',')) out.push_back(item);
  return out;
}

// Turns raw user input into a safe, prefix-matching FTS5 MATCH
// expression instead of handing FTS5's own query parser the typed text
// verbatim. Two real problems that fixes:
//
// 1. Bare words that happen to collide with FTS5 query syntax — "AND",
//    "OR", "NOT", a leading "-", an unmatched '"' — used to go straight
//    into MATCH unescaped. Best case that's a confusing non-match
//    (searching for the literal word "and" silently became the boolean
//    AND operator with nothing on one side); worst case it's a MATCH
//    syntax error surfaced to the caller as a 500. Wrapping every
//    whitespace-split word in "double quotes" makes FTS5 treat it as
//    literal text regardless of what's inside — a literal '"' is
//    escaped by doubling it, the one character quoting itself doesn't
//    neutralize.
// 2. No partial-word matching at all — confirmed live: searching "time"
//    found a document containing the literal word "time" but NOT one
//    that only said "Timers"/"timer", even with the porter stemmer
//    active (porter doesn't reduce the "-er" agent-noun suffix, so
//    "timer" and "time" are different stems — this is correct per the
//    stemmer, just not what anyone actually wants from a search box).
//    Appending '*' after each quoted word turns it into an FTS5 prefix
//    query: it matches any INDEXED (post-stemming) term that starts
//    with the given text, so "time*" matches the stored stem "timer"
//    (itself the stem of both "Timer" and "Timers") the same way it
//    matches the stem "time" — turning a query into a widening rather
//    than an exact filter, exactly what "start typing a word and see
//    matches" search UX means. No FTS5 prefix index (`prefix=`) was
//    added for this — at a personal-wiki-sized corpus, a plain term
//    scan for a prefix is not a real performance concern, and adding
//    one is a schema migration (schema_version bump) not obviously
//    worth it before this is ever seen to be slow.
//
// Deliberately NOT trying to parse/preserve the user's own quoted
// phrases or explicit AND/OR/NOT — every word is an independent,
// implicitly-ANDed (FTS5's default between space-separated match
// expressions) prefix term. A power-user "advanced query syntax" mode
// is a different feature, not a silent behavior change on top of this
// one.
std::string buildMatchExpression(const std::string& rawText) {
  std::ostringstream out;
  std::istringstream words(rawText);
  std::string word;
  bool first = true;
  while (words >> word) {
    std::string escaped;
    escaped.reserve(word.size() + 2);
    for (char c : word) {
      if (c == '"') escaped += '"';  // double it — FTS5's own quote-escape
      escaped += c;
    }
    if (!first) out << ' ';
    out << '"' << escaped << "\"*";
    first = false;
  }
  return out.str();
}

constexpr const char* kTagsSubquery =
    "(SELECT GROUP_CONCAT(t.name, ',') FROM document_tags dt "
    " JOIN tags t ON t.id = dt.tag_id WHERE dt.document_rowid = d.rowid_id)";

constexpr const char* kTagFilterClause =
    "EXISTS (SELECT 1 FROM document_tags dt2 JOIN tags t2 ON t2.id = dt2.tag_id "
    "WHERE dt2.document_rowid = d.rowid_id AND t2.name = ?)";

// Appends the tag/docType/folder filter clauses shared by every query
// variant below (plain browse, BM25 candidate retrieval, and the
// hybrid-search metadata lookups) — kept in exactly one place so the SQL
// text here and the bind order in bindCommonFilters() below can't drift
// apart from each other across the several call sites that both now
// need them.
void appendCommonFilterSql(std::ostringstream& sql, const SearchQuery& query) {
  if (query.docType) sql << " AND d.doc_type = ?";
  if (!query.docTypes.empty()) {
    sql << " AND d.doc_type IN (";
    for (size_t i = 0; i < query.docTypes.size(); ++i) sql << (i == 0 ? "?" : ",?");
    sql << ")";
  }
  if (query.tag) sql << " AND " << kTagFilterClause;
  for (size_t i = 0; i < query.tags.size(); ++i) sql << " AND " << kTagFilterClause;
  if (query.folderPrefix) sql << " AND substr(d.path, 1, ?) = ?";
}

// Binds parameters for appendCommonFilterSql() above, in the exact same
// order its clauses appear.
void bindCommonFilters(Statement& stmt, int& idx, const SearchQuery& query) {
  if (query.docType) stmt.bind(idx++, *query.docType);
  for (const auto& t : query.docTypes) stmt.bind(idx++, t);
  if (query.tag) stmt.bind(idx++, *query.tag);
  for (const auto& t : query.tags) stmt.bind(idx++, t);
  if (query.folderPrefix) {
    stmt.bind(idx++, static_cast<int64_t>(query.folderPrefix->size()));
    stmt.bind(idx++, *query.folderPrefix);
  }
}

#ifdef WIKI_ENABLE_SQLITE_VEC
// Reciprocal Rank Fusion: combines two independently-ranked rowid lists
// (BM25 lexical rank, cosine-distance semantic rank) into one score per
// rowid, without needing to normalize BM25's and cosine distance's
// completely different, incomparable scales — RRF only ever looks at
// each list's own RANK POSITIONS, never the underlying scores. k=60 is
// the standard constant from the original RRF paper (Cormack et al.
// 2009) and virtually every production hybrid-search implementation
// since; not tuned for this project specifically; there's no principled
// reason yet to deviate from a well-established default. Returns rowids
// sorted by combined score, descending (best first).
std::vector<int64_t> reciprocalRankFusion(const std::vector<int64_t>& bm25Ranked,
                                           const std::vector<int64_t>& semanticRanked) {
  constexpr double k = 60.0;
  std::unordered_map<int64_t, double> scores;
  for (size_t i = 0; i < bm25Ranked.size(); ++i) {
    scores[bm25Ranked[i]] += 1.0 / (k + static_cast<double>(i) + 1.0);
  }
  for (size_t i = 0; i < semanticRanked.size(); ++i) {
    scores[semanticRanked[i]] += 1.0 / (k + static_cast<double>(i) + 1.0);
  }

  std::vector<std::pair<int64_t, double>> ranked(scores.begin(), scores.end());
  std::sort(ranked.begin(), ranked.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  std::vector<int64_t> result;
  result.reserve(ranked.size());
  for (const auto& [rowId, score] : ranked) result.push_back(rowId);
  return result;
}
#endif  // WIKI_ENABLE_SQLITE_VEC

std::vector<SearchResultItem> runQuery(Statement& stmt, bool highlighted) {
  std::vector<SearchResultItem> results;
  while (stmt.step()) {
    SearchResultItem item;
    item.path = stmt.columnText(0);
    item.title = stmt.columnText(1);
    item.visibility = stmt.columnText(2);
    item.updatedAt = stmt.columnText(3);
    item.docType = stmt.columnText(4);
    item.tags = splitCsv(stmt.columnText(5));
    item.snippet = stmt.columnText(6);
    item.snippetIsHighlighted = highlighted;
    results.push_back(std::move(item));
  }
  return results;
}

}  // namespace

std::vector<SearchResultItem> FtsSearch::search(const SearchQuery& query) const {
  // Built once, used both to decide the query mode AND as the actual
  // bound MATCH text below — also correctly demotes an all-whitespace
  // query.text (e.g. "   ") to browse mode, which a plain
  // !query.text.empty() check would have missed.
  const std::string matchExpr = buildMatchExpression(query.text);
  const bool textSearch = !matchExpr.empty();

#ifdef WIKI_ENABLE_SQLITE_VEC
  // Hybrid ranking only ever applies to an actual text search — browse
  // mode (empty query) has no query text to embed, and its own ordering
  // (newest-updated-first) has nothing to do with relevance ranking at
  // all. tryHybridSearch() returns nullopt (not an empty vector — that
  // would be indistinguishable from "zero real results") for every
  // condition that should fall back to plain FTS5 below instead of
  // changing the answer: no document_embeddings table yet, or
  // provider_->embed() itself failing.
  if (textSearch && provider_ != nullptr &&
      EmbeddingsRuntimeConfig(db_).isVectorSearchEnabled()) {
    if (auto hybridResults = tryHybridSearch(query, matchExpr)) {
      return std::move(*hybridResults);
    }
  }
#endif

  std::ostringstream sql;
  if (textSearch) {
    // Match markers are bound parameters (kSnippetMatchStart/End), not
    // literal "<mark>" — see SearchResultItem::snippet's doc comment.
    sql << "SELECT d.path, d.title, d.visibility, d.updated_at, d.doc_type, "
        << kTagsSubquery << ", "
        << "snippet(documents_fts, 1, ?, ?, '...', 12) "
        << "FROM documents_fts JOIN documents d ON d.rowid_id = documents_fts.rowid "
        << "WHERE documents_fts MATCH ? AND (? = 1 OR d.visibility = 'public')";
  } else {
    sql << "SELECT d.path, d.title, d.visibility, d.updated_at, d.doc_type, "
        << kTagsSubquery << ", d.excerpt "
        << "FROM documents d "
        << "WHERE (? = 1 OR d.visibility = 'public')";
  }

  appendCommonFilterSql(sql, query);

  // Column order is (title, body, tags_flat) per schema.h — weights make
  // a title hit count for more than the same word buried in the body, and
  // a tag (a deliberate, curated label, not incidental prose) count for
  // more than body text too, without either drowning out an actual body
  // match entirely. bm25() returns more-negative for a better match, so
  // ORDER BY ascending (the default, unchanged) is still correct here —
  // only the relative weighting of columns changes, not the sort
  // direction.
  sql << (textSearch ? " ORDER BY bm25(documents_fts, 4.0, 1.0, 2.5) "
                      : " ORDER BY d.updated_at DESC ")
      << "LIMIT ? OFFSET ?;";

  Statement stmt(db_.handle(), sql.str());
  int idx = 1;
  if (textSearch) {
    // Placeholder order must match the SQL text above left-to-right:
    // snippet() markers, then MATCH text, then the visibility guard.
    stmt.bind(idx++, std::string(1, kSnippetMatchStart));
    stmt.bind(idx++, std::string(1, kSnippetMatchEnd));
    stmt.bind(idx++, matchExpr);
  }
  stmt.bind(idx++, static_cast<int64_t>(query.includePrivate ? 1 : 0));
  bindCommonFilters(stmt, idx, query);
  stmt.bind(idx++, static_cast<int64_t>(query.limit));
  stmt.bind(idx++, static_cast<int64_t>(query.offset));

  return runQuery(stmt, textSearch);
}

std::vector<std::string> FtsSearch::matchingPaths(const std::string& text,
                                                  bool includePrivate) const {
  const std::string matchExpr = buildMatchExpression(text);
  if (matchExpr.empty()) return {};

  // Personal-wiki sized; high enough that a real content filter is not
  // silently truncated the way /api/search's ranked top-50 is, low
  // enough that a query matching every document cannot become an
  // unbounded JSON dump.
  constexpr int64_t kMatchingPathsLimit = 10000;

  Statement stmt(db_.handle(),
                 "SELECT d.path "
                 "FROM documents_fts JOIN documents d ON d.rowid_id = documents_fts.rowid "
                 "WHERE documents_fts MATCH ? AND (? = 1 OR d.visibility = 'public') "
                 "ORDER BY d.path "
                 "LIMIT ?;");
  stmt.bind(1, matchExpr);
  stmt.bind(2, static_cast<int64_t>(includePrivate ? 1 : 0));
  stmt.bind(3, kMatchingPathsLimit);

  std::vector<std::string> paths;
  while (stmt.step()) paths.push_back(stmt.columnText(0));
  return paths;
}

#ifdef WIKI_ENABLE_SQLITE_VEC

std::vector<int64_t> FtsSearch::bm25CandidateRowIds(const SearchQuery& query,
                                                      const std::string& matchExpr,
                                                      int candidateLimit) const {
  std::ostringstream sql;
  sql << "SELECT d.rowid_id "
      << "FROM documents_fts JOIN documents d ON d.rowid_id = documents_fts.rowid "
      << "WHERE documents_fts MATCH ? AND (? = 1 OR d.visibility = 'public')";
  appendCommonFilterSql(sql, query);
  sql << " ORDER BY bm25(documents_fts, 4.0, 1.0, 2.5) LIMIT ?;";

  Statement stmt(db_.handle(), sql.str());
  int idx = 1;
  stmt.bind(idx++, matchExpr);
  stmt.bind(idx++, static_cast<int64_t>(query.includePrivate ? 1 : 0));
  bindCommonFilters(stmt, idx, query);
  stmt.bind(idx++, static_cast<int64_t>(candidateLimit));

  std::vector<int64_t> rowIds;
  while (stmt.step()) rowIds.push_back(stmt.columnInt64(0));
  return rowIds;
}

std::vector<SearchResultItem> FtsSearch::fetchByRowIds(
    const std::vector<int64_t>& orderedRowIds,
    const std::unordered_set<int64_t>& snippetEligibleRowIds, const SearchQuery& query) const {
  if (orderedRowIds.empty()) return {};
  const bool includePrivate = query.includePrivate;

  std::unordered_map<int64_t, SearchResultItem> byRowId;

  std::vector<int64_t> snippetIds, excerptIds;
  for (auto id : orderedRowIds) {
    (snippetEligibleRowIds.count(id) != 0 ? snippetIds : excerptIds).push_back(id);
  }

  if (!snippetIds.empty()) {
    std::ostringstream sql;
    sql << "SELECT d.rowid_id, d.path, d.title, d.visibility, d.updated_at, d.doc_type, "
        << kTagsSubquery << ", snippet(documents_fts, 1, ?, ?, '...', 12) "
        << "FROM documents_fts JOIN documents d ON d.rowid_id = documents_fts.rowid "
        << "WHERE d.rowid_id IN (";
    for (size_t i = 0; i < snippetIds.size(); ++i) sql << (i == 0 ? "?" : ",?");
    sql << ") AND (? = 1 OR d.visibility = 'public')";
    appendCommonFilterSql(sql, query);
    sql << ";";

    Statement stmt(db_.handle(), sql.str());
    int idx = 1;
    stmt.bind(idx++, std::string(1, kSnippetMatchStart));
    stmt.bind(idx++, std::string(1, kSnippetMatchEnd));
    for (auto id : snippetIds) stmt.bind(idx++, id);
    stmt.bind(idx++, static_cast<int64_t>(includePrivate ? 1 : 0));
    bindCommonFilters(stmt, idx, query);

    while (stmt.step()) {
      const int64_t rowId = stmt.columnInt64(0);
      SearchResultItem item;
      item.path = stmt.columnText(1);
      item.title = stmt.columnText(2);
      item.visibility = stmt.columnText(3);
      item.updatedAt = stmt.columnText(4);
      item.docType = stmt.columnText(5);
      item.tags = splitCsv(stmt.columnText(6));
      item.snippet = stmt.columnText(7);
      item.snippetIsHighlighted = true;
      byRowId[rowId] = std::move(item);
    }
  }

  if (!excerptIds.empty()) {
    std::ostringstream sql;
    sql << "SELECT d.rowid_id, d.path, d.title, d.visibility, d.updated_at, d.doc_type, "
        << kTagsSubquery << ", d.excerpt "
        << "FROM documents d WHERE d.rowid_id IN (";
    for (size_t i = 0; i < excerptIds.size(); ++i) sql << (i == 0 ? "?" : ",?");
    sql << ") AND (? = 1 OR d.visibility = 'public')";
    appendCommonFilterSql(sql, query);
    sql << ";";

    Statement stmt(db_.handle(), sql.str());
    int idx = 1;
    for (auto id : excerptIds) stmt.bind(idx++, id);
    stmt.bind(idx++, static_cast<int64_t>(includePrivate ? 1 : 0));
    bindCommonFilters(stmt, idx, query);

    while (stmt.step()) {
      const int64_t rowId = stmt.columnInt64(0);
      SearchResultItem item;
      item.path = stmt.columnText(1);
      item.title = stmt.columnText(2);
      item.visibility = stmt.columnText(3);
      item.updatedAt = stmt.columnText(4);
      item.docType = stmt.columnText(5);
      item.tags = splitCsv(stmt.columnText(6));
      item.snippet = stmt.columnText(7);
      item.snippetIsHighlighted = false;
      byRowId[rowId] = std::move(item);
    }
  }

  // Re-order to match the RRF-merged rank exactly — SQL's IN (...) makes
  // no ordering promise. A rowid with no row back at all here (deleted
  // between the candidate query and this one, or filtered out by the
  // visibility check just above) is silently skipped rather than
  // producing a hole/placeholder in the results.
  std::vector<SearchResultItem> results;
  results.reserve(orderedRowIds.size());
  for (auto id : orderedRowIds) {
    auto it = byRowId.find(id);
    if (it != byRowId.end()) results.push_back(std::move(it->second));
  }
  return results;
}

std::optional<std::vector<SearchResultItem>> FtsSearch::tryHybridSearch(
    const SearchQuery& query, const std::string& matchExpr) const {
  EmbeddingIndexer indexer(db_.handle());
  if (!indexer.currentDimensions().has_value()) {
    return std::nullopt;  // nothing indexed yet — fall back to plain FTS5
  }

  std::vector<float> queryEmbedding;
  bool cacheHit = false;
  {
    std::lock_guard<std::mutex> lock(queryEmbeddingCacheMutex_);
    auto it = queryEmbeddingCache_.find(query.text);
    if (it != queryEmbeddingCache_.end()) {
      queryEmbedding = it->second;
      cacheHit = true;
    }
  }
  if (!cacheHit) {
    try {
      queryEmbedding = provider_->embedQuery(query.text);
    } catch (...) {
      // Best-effort, same reasoning as IndexUpdater's own embedding step:
      // a broken embeddings API/model must never break search itself, it
      // should just fall back to FTS5-only for this one call.
      return std::nullopt;
    }
    // Locked separately from the embedQuery() call above, deliberately —
    // that call alone measured 1.3-2.6s on real hardware (see the cache
    // member's own comment in FtsSearch.h); holding this mutex across it
    // would serialize every concurrent search behind whichever request
    // happens to be computing an embedding, even for unrelated query
    // text. embedQuery() itself is already safely concurrent (or safely
    // serialized, for LocalEmbeddingProvider — see its own embedMutex_)
    // without any help from this lock.
    std::lock_guard<std::mutex> lock(queryEmbeddingCacheMutex_);
    if (queryEmbeddingCache_.size() >= kQueryEmbeddingCacheCap &&
        queryEmbeddingCache_.find(query.text) == queryEmbeddingCache_.end()) {
      queryEmbeddingCache_.clear();
    }
    queryEmbeddingCache_[query.text] = queryEmbedding;
  }

  // A generous candidate pool (not just query.limit+query.offset) so RRF
  // actually has enough of both ranked lists to blend meaningfully —
  // limit/offset are applied to the MERGED ranking below, not to either
  // source list individually.
  constexpr int kCandidatePoolSize = 200;
  const std::vector<int64_t> bm25Candidates =
      bm25CandidateRowIds(query, matchExpr, kCandidatePoolSize);

  // Cosine-distance cutoff — see maxSemanticDistance_'s own comment in
  // FtsSearch.h for why this is necessary, not optional polish:
  // EmbeddingIndexer::nearest() has no relevance floor of its own, so on
  // a small vault "the nearest kCandidatePoolSize neighbors" is
  // effectively the WHOLE vault, ranked by a distance that's often pure
  // noise for a genuinely unrelated document. Filtering here, before RRF
  // ever sees these rowids, is what actually keeps irrelevant documents
  // out of hybrid results — RRF itself has no way to reject a candidate
  // once it's in a ranked list, only rank it.
  //
  // maxSemanticCandidates_ additionally hard-caps the COUNT, on top of
  // the distance cutoff — see its own comment in FtsSearch.h. nearest()
  // is documented to return neighbors already sorted nearest-first, so
  // stopping once the cap is reached keeps exactly the closest ones,
  // same as the distance cutoff would eventually exclude the rest of
  // anyway, just without depending on the cutoff being tight enough on
  // its own for a query whose embedding sits at a uniformly "blurry"
  // distance from many documents at once.
  std::vector<int64_t> semanticCandidates;
  for (const auto& neighbor : indexer.nearest(queryEmbedding, kCandidatePoolSize)) {
    if (neighbor.distance > maxSemanticDistance_) continue;
    if (static_cast<int>(semanticCandidates.size()) >= maxSemanticCandidates_) break;
    semanticCandidates.push_back(neighbor.documentRowId);
  }

  const std::vector<int64_t> merged = reciprocalRankFusion(bm25Candidates, semanticCandidates);

  if (query.offset >= static_cast<int>(merged.size())) {
    return std::vector<SearchResultItem>{};
  }
  const size_t start = static_cast<size_t>(query.offset);
  const size_t end =
      std::min(merged.size(), start + static_cast<size_t>(std::max(query.limit, 0)));
  const std::vector<int64_t> page(merged.begin() + static_cast<long>(start),
                                   merged.begin() + static_cast<long>(end));

  // NOTE (accepted, not fixed here): tag/docType/folderPrefix filtering
  // happens INSIDE fetchByRowIds(), which runs AFTER this page slice is
  // already cut from `merged` — a semantic candidate that fails the
  // filter is silently dropped from its page rather than backfilled from
  // further down `merged`, so an active filter can return fewer than
  // `query.limit` results even when more real matches exist deeper in
  // the ranked list. Not a correctness bug (no wrong document is ever
  // shown, which is what was actually reported and fixed) — a page
  // running short is a separate completeness nuance, not worth the
  // bigger restructure (filtering semantic candidates against
  // document_tags/doc_type BEFORE the RRF merge) on a vault this size.
  const std::unordered_set<int64_t> bm25Set(bm25Candidates.begin(), bm25Candidates.end());
  return fetchByRowIds(page, bm25Set, query);
}

#endif  // WIKI_ENABLE_SQLITE_VEC

}  // namespace wikicore::index
