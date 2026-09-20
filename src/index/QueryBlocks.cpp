#include "index/QueryBlocks.h"

#include "index/Statement.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <sstream>

namespace wikicore::index {

namespace {

std::string trim(const std::string& s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                  [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::vector<std::string> splitComma(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string part;
  while (std::getline(ss, part, ',')) {
    std::string trimmed = trim(part);
    if (!trimmed.empty()) out.push_back(trimmed);
  }
  return out;
}

// Escapes a value that will be used as a LIKE prefix ('folder:') so a
// stray '%'/'_' the admin happens to type in a folder name is matched
// LITERALLY, not as a SQL wildcard -- not a security boundary here
// (only the admin, who already controls every document's content, can
// author a query block at all), but cheap and correct regardless, same
// discipline this codebase applies everywhere else.
std::string escapeLikePattern(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '%' || c == '_' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------
// Grammar (one `key: value` pair per line, blank lines ignored):
//
//   tag: name[, name...]   -- AND semantics across multiple names;
//                              documents must carry every listed tag.
//                              Repeating the `tag:` key on a second line
//                              is a parse error (ambiguous -- would this
//                              replace or extend the first line's list?
//                              rather than guess, refuse).
//   type: name             -- exact match against a document's own
//                              front-matter `type`.
//   folder: path/prefix    -- documents whose path starts with this
//                              (a trailing '/' is NOT implied -- write
//                              "recipes/" explicitly to scope a folder,
//                              matching how folder paths are stored).
//   orphans: true           -- only documents with ZERO visible incoming
//                              [[wiki-link]] backlinks (visibility-gated
//                              the same way as everything else -- a
//                              private document's own outgoing link
//                              never counts for an anonymous caller).
//   sort: title|updated|created|path   (default: title)
//   order: asc|desc                    (default: desc for updated/
//                                        created, asc for title/path)
//   limit: 1-100                       (default: 20)
//
// EVERY key above maps to ONE fixed, hardcoded SQL fragment already
// written into this file -- there is no code path where any part of a
// query block's TEXT is concatenated into SQL. Values only ever reach
// the database as bound parameters (tag names, type, folder prefix,
// limit) or are looked up against a small in-code whitelist map and
// substituted for the corresponding LITERAL, hardcoded column/direction
// string (sort/order) -- never the caller's own spelling. An
// unrecognized key, a duplicate key, or a value outside its own
// whitelist is a PARSE ERROR (QueryBlockResult::ok = false), not a query
// that silently does something else or returns an empty "no matches" --
// see QueryBlocks.h's own comment on why that distinction matters.
QueryBlockResult QueryBlocks::parseAndRun(const std::string& raw, bool includePrivate) const {
  std::vector<std::string> tags;
  std::string type;
  std::string folder;
  std::string sortColumn = "d.title";
  bool sortExplicit = false;
  std::string sortKey = "title";
  std::string orderDir;  // resolved after the loop, once sortKey is known
  bool orderExplicit = false;
  int limit = 20;
  bool orphansOnly = false;

  std::set<std::string> seenKeys;

  std::stringstream lines(raw);
  std::string line;
  while (std::getline(lines, line)) {
    const std::string trimmed = trim(line);
    if (trimmed.empty()) continue;

    const size_t colon = trimmed.find(':');
    if (colon == std::string::npos) {
      return {false, "invalid line (expected 'key: value'): " + trimmed, {}};
    }
    const std::string key = toLower(trim(trimmed.substr(0, colon)));
    const std::string value = trim(trimmed.substr(colon + 1));

    if (key != "tag" && seenKeys.count(key)) {
      return {false, "duplicate key: " + key, {}};
    }
    seenKeys.insert(key);

    if (key == "tag") {
      if (!tags.empty()) {
        return {false, "duplicate key: tag (list every tag on one line, comma-separated)", {}};
      }
      tags = splitComma(value);
      if (tags.empty()) return {false, "tag: needs at least one name", {}};
    } else if (key == "type") {
      if (value.empty()) return {false, "type: needs a value", {}};
      type = value;
    } else if (key == "folder") {
      if (value.empty()) return {false, "folder: needs a value", {}};
      folder = value;
    } else if (key == "orphans") {
      const std::string v = toLower(value);
      if (v != "true" && v != "false") return {false, "orphans: must be 'true' or 'false'", {}};
      orphansOnly = (v == "true");
    } else if (key == "sort") {
      const std::string v = toLower(value);
      sortExplicit = true;
      sortKey = v;
      if (v == "title") {
        sortColumn = "d.title";
      } else if (v == "updated") {
        sortColumn = "d.updated_at";
      } else if (v == "created") {
        sortColumn = "d.created_at";
      } else if (v == "path") {
        sortColumn = "d.path";
      } else {
        return {false, "sort: must be one of title, updated, created, path", {}};
      }
    } else if (key == "order") {
      const std::string v = toLower(value);
      orderExplicit = true;
      if (v == "asc") {
        orderDir = "ASC";
      } else if (v == "desc") {
        orderDir = "DESC";
      } else {
        return {false, "order: must be 'asc' or 'desc'", {}};
      }
    } else if (key == "limit") {
      try {
        size_t consumed = 0;
        int parsed = std::stoi(value, &consumed);
        if (consumed != value.size()) throw std::invalid_argument("trailing garbage");
        if (parsed < 1 || parsed > 100) {
          return {false, "limit: must be between 1 and 100", {}};
        }
        limit = parsed;
      } catch (const std::exception&) {
        return {false, "limit: must be a whole number between 1 and 100", {}};
      }
    } else {
      return {false, "unknown key: " + key +
                          " (expected one of: tag, type, folder, orphans, sort, order, limit)",
              {}};
    }
  }
  (void)sortExplicit;

  if (!orderExplicit) {
    // Recently-updated/created first by default (the common "what's
    // new" use case); alphabetical stays ascending by default (the
    // common "browse a list" use case) -- see the grammar comment above.
    orderDir = (sortKey == "updated" || sortKey == "created") ? "DESC" : "ASC";
  }

  std::vector<std::string> whereClauses = {"(? = 1 OR d.visibility = 'public')"};
  std::vector<std::function<void(Statement&, int)>> binders = {
      [includePrivate](Statement& s, int idx) {
        s.bind(idx, static_cast<int64_t>(includePrivate ? 1 : 0));
      }};

  if (!type.empty()) {
    whereClauses.push_back("d.doc_type = ?");
    binders.push_back([type](Statement& s, int idx) { s.bind(idx, type); });
  }

  if (!folder.empty()) {
    whereClauses.push_back("d.path LIKE ? ESCAPE '\\'");
    std::string pattern = escapeLikePattern(folder) + "%";
    binders.push_back([pattern](Statement& s, int idx) { s.bind(idx, pattern); });
  }

  if (!tags.empty()) {
    std::string inList;
    for (size_t i = 0; i < tags.size(); ++i) {
      if (i) inList += ", ";
      inList += "?";
    }
    whereClauses.push_back(
        "d.rowid_id IN (SELECT dt.document_rowid FROM document_tags dt "
        "JOIN tags t ON t.id = dt.tag_id WHERE t.name IN (" +
        inList + ") GROUP BY dt.document_rowid HAVING COUNT(DISTINCT t.id) = " +
        std::to_string(tags.size()) + ")");
    for (const auto& tag : tags) {
      binders.push_back([tag](Statement& s, int idx) { s.bind(idx, tag); });
    }
  }

  if (orphansOnly) {
    // A NOT EXISTS subquery, not a LEFT JOIN + IS NULL -- deliberately.
    // An earlier version used a LEFT JOIN with an explicitly NUMBERED
    // placeholder (?N) in the FROM clause, ahead of this WHERE clause's
    // own plain "?" placeholders in the final SQL text. That's a real,
    // caught-before-shipping bug: SQLite assigns a plain "?" the next
    // number after the LARGEST number already seen IN TEXT-PARSE ORDER,
    // not in this function's construction order -- a numbered "?N"
    // appearing earlier in the text (the JOIN, here) silently shifts
    // every later anonymous "?" (the WHERE clauses below) to the WRONG
    // bind position, binding e.g. `type` to a slot meant for `folder`.
    // Keeping every parameterized condition inside WHERE, in the same
    // order its binder is pushed, sidesteps the whole class of bug --
    // text order and construction order are then the same order by
    // construction, not by careful bookkeeping.
    //
    // A document counts as having a visible backlink only if the
    // LINKING document is itself visible to this caller -- the exact
    // same fail-safe-private direction NavQueries::backlinks already
    // applies (a private document's own outgoing link never counts for
    // an anonymous caller, not even to make a target look "not
    // orphaned").
    whereClauses.push_back(
        "NOT EXISTS (SELECT 1 FROM document_links dl "
        "JOIN documents src ON src.rowid_id = dl.source_rowid "
        "WHERE dl.target_path = d.path AND (? = 1 OR src.visibility = 'public'))");
    binders.push_back([includePrivate](Statement& s, int idx) {
      s.bind(idx, static_cast<int64_t>(includePrivate ? 1 : 0));
    });
  }

  std::string sql =
      "SELECT d.path, d.title, d.visibility, d.updated_at, "
      "COALESCE((SELECT GROUP_CONCAT(t2.name, ', ') FROM document_tags dt2 "
      "JOIN tags t2 ON t2.id = dt2.tag_id WHERE dt2.document_rowid = d.rowid_id), '') "
      "AS tags_flat "
      "FROM documents d WHERE ";
  for (size_t i = 0; i < whereClauses.size(); ++i) {
    if (i) sql += " AND ";
    sql += whereClauses[i];
  }
  sql += " ORDER BY " + sortColumn + " " + orderDir + ", d.path ASC LIMIT ?;";
  binders.push_back([limit](Statement& s, int idx) { s.bind(idx, static_cast<int64_t>(limit)); });

  Statement stmt(db_.handle(), sql);
  for (size_t i = 0; i < binders.size(); ++i) {
    binders[i](stmt, static_cast<int>(i) + 1);
  }

  QueryBlockResult result;
  result.ok = true;
  while (stmt.step()) {
    result.rows.push_back(QueryResultRow{
        stmt.columnText(0), stmt.columnText(1), stmt.columnText(2),
        stmt.columnText(3), stmt.columnText(4)});
  }
  return result;
}

}  // namespace wikicore::index
