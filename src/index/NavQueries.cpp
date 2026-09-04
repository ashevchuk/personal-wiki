#include "index/NavQueries.h"

#include "index/Statement.h"

namespace wikicore::index {

std::vector<DocSummary> NavQueries::listVisibleDocuments(bool includePrivate) const {
  Statement stmt(db_.handle(),
                  "SELECT path, title, visibility FROM documents "
                  "WHERE (?1 = 1 OR visibility = 'public') "
                  "ORDER BY path;");
  stmt.bind(1, static_cast<int64_t>(includePrivate ? 1 : 0));

  std::vector<DocSummary> results;
  while (stmt.step()) {
    results.push_back(
        DocSummary{stmt.columnText(0), stmt.columnText(1), stmt.columnText(2)});
  }
  return results;
}

std::vector<TagCount> NavQueries::tagCounts(bool includePrivate) const {
  // Alphabetical, not count-desc (the pre-existing order here, before
  // this change) — the sidebar's tag list and the search page's tag
  // multiselect (both consumers of this, via /api/nav/tags) are
  // navigation aids: once a vault has more than a handful of tags,
  // finding a specific one by scanning a count-sorted list means
  // reading the whole thing, while an alphabetical one lets you jump
  // straight to where it'd be. COLLATE NOCASE because nothing anywhere
  // in this app normalizes tag casing on save (confirmed: no
  // tolower()/toLowerCase() on the tag-write path) — plain byte-order
  // ASCII sort would put every uppercase-first tag before every
  // lowercase one ('Z' < 'a'), which reads as broken, not alphabetical,
  // to a human. Count is still shown next to each tag ("#food (3)") —
  // only the ORDER changed, no information lost.
  Statement stmt(
      db_.handle(),
      "SELECT t.name, COUNT(*) FROM tags t "
      "JOIN document_tags dt ON dt.tag_id = t.id "
      "JOIN documents d ON d.rowid_id = dt.document_rowid "
      "WHERE (?1 = 1 OR d.visibility = 'public') "
      "GROUP BY t.name "
      "ORDER BY t.name COLLATE NOCASE ASC;");
  stmt.bind(1, static_cast<int64_t>(includePrivate ? 1 : 0));

  std::vector<TagCount> results;
  while (stmt.step()) {
    results.push_back(TagCount{stmt.columnText(0), stmt.columnInt64(1)});
  }
  return results;
}

std::vector<TagCount> NavQueries::typeCounts(bool includePrivate) const {
  // doc_type is a plain nullable/empty TEXT column on documents (no join
  // table, unlike tags) — an untyped document stores "" there (see
  // IndexUpdater::upsertOne, always binds entry.docType as a string,
  // never NULL), so it's excluded explicitly rather than showing up as a
  // bogus blank option in the search page's type dropdown.
  Statement stmt(
      db_.handle(),
      "SELECT d.doc_type, COUNT(*) FROM documents d "
      "WHERE d.doc_type <> '' AND (?1 = 1 OR d.visibility = 'public') "
      "GROUP BY d.doc_type "
      "ORDER BY COUNT(*) DESC, d.doc_type ASC;");
  stmt.bind(1, static_cast<int64_t>(includePrivate ? 1 : 0));

  std::vector<TagCount> results;
  while (stmt.step()) {
    results.push_back(TagCount{stmt.columnText(0), stmt.columnInt64(1)});
  }
  return results;
}

}  // namespace wikicore::index
