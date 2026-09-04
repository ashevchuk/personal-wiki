#include "index/FtsSearch.h"

#include "index/Statement.h"

#include <sstream>

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
  std::ostringstream sql;
  // Built once, used both to decide the query mode AND as the actual
  // bound MATCH text below — also correctly demotes an all-whitespace
  // query.text (e.g. "   ") to browse mode, which a plain
  // !query.text.empty() check would have missed.
  const std::string matchExpr = buildMatchExpression(query.text);
  const bool textSearch = !matchExpr.empty();

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

  if (query.docType) sql << " AND d.doc_type = ?";
  // IN (...) -> OR semantics (see docTypes' doc comment in FtsSearch.h for
  // why AND, as used for tags below, would never match anything here).
  if (!query.docTypes.empty()) {
    sql << " AND d.doc_type IN (";
    for (size_t i = 0; i < query.docTypes.size(); ++i) sql << (i == 0 ? "?" : ",?");
    sql << ")";
  }
  if (query.tag) sql << " AND " << kTagFilterClause;
  // One EXISTS per requested tag -> AND semantics (must carry all of them).
  for (size_t i = 0; i < query.tags.size(); ++i) sql << " AND " << kTagFilterClause;
  if (query.folderPrefix) {
    // Exact-length prefix comparison rather than LIKE, so a folder name
    // containing a literal '%'/'_' can't be misread as a wildcard.
    sql << " AND substr(d.path, 1, ?) = ?";
  }

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
  if (query.docType) stmt.bind(idx++, *query.docType);
  for (const auto& t : query.docTypes) stmt.bind(idx++, t);
  if (query.tag) stmt.bind(idx++, *query.tag);
  for (const auto& t : query.tags) stmt.bind(idx++, t);
  if (query.folderPrefix) {
    stmt.bind(idx++, static_cast<int64_t>(query.folderPrefix->size()));
    stmt.bind(idx++, *query.folderPrefix);
  }
  stmt.bind(idx++, static_cast<int64_t>(query.limit));
  stmt.bind(idx++, static_cast<int64_t>(query.offset));

  return runQuery(stmt, textSearch);
}

}  // namespace wikicore::index
