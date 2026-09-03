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
  const bool textSearch = !query.text.empty();

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
  if (query.tag) sql << " AND " << kTagFilterClause;

  sql << (textSearch ? " ORDER BY bm25(documents_fts) LIMIT ?;"
                      : " ORDER BY d.updated_at DESC LIMIT ?;");

  Statement stmt(db_.handle(), sql.str());
  int idx = 1;
  if (textSearch) {
    // Placeholder order must match the SQL text above left-to-right:
    // snippet() markers, then MATCH text, then the visibility guard.
    stmt.bind(idx++, std::string(1, kSnippetMatchStart));
    stmt.bind(idx++, std::string(1, kSnippetMatchEnd));
    stmt.bind(idx++, query.text);
  }
  stmt.bind(idx++, static_cast<int64_t>(query.includePrivate ? 1 : 0));
  if (query.docType) stmt.bind(idx++, *query.docType);
  if (query.tag) stmt.bind(idx++, *query.tag);
  stmt.bind(idx++, static_cast<int64_t>(query.limit));

  return runQuery(stmt, textSearch);
}

}  // namespace wikicore::index
