#include "controllers/SearchRoutes.h"

#include "auth/RequireAdmin.h"

#include <drogon/HttpResponse.h>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::index;

namespace wikicore::controllers {

namespace {

// Splits a comma-separated query param into trimmed, non-empty pieces.
// Used for both `tag` and `type` below — the search page's multiselect
// widget joins its selected checkboxes with ',' into one URL param
// rather than repeating the key (keeps /api/search's URL shape identical
// to what a hand-typed single value already looked like: "?tag=food" is
// just a one-element list here, unchanged behavior for old links/bookmarks).
std::vector<std::string> splitCsvParam(const std::string& raw) {
  std::vector<std::string> out;
  std::string current;
  for (char c : raw) {
    if (c == ',') {
      if (!current.empty()) out.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

SearchQuery buildQuery(const HttpRequestPtr& req, bool includePrivate) {
  SearchQuery q;
  q.text = req->getParameter("q");
  // AND semantics (FtsSearch.cpp: one EXISTS clause per entry) — a
  // document must carry every tag selected in the multiselect.
  q.tags = splitCsvParam(req->getParameter("tag"));
  // OR semantics (FtsSearch.cpp: doc_type IN (...)) — a document has
  // exactly one doc_type, so multiple selected types can only ever mean
  // "any of these".
  q.docTypes = splitCsvParam(req->getParameter("type"));
  q.includePrivate = includePrivate;
  q.limit = 50;
  return q;
}

Json::Value resultsToJson(const std::vector<SearchResultItem>& results) {
  Json::Value arr(Json::arrayValue);
  for (const auto& item : results) {
    Json::Value obj;
    obj["path"] = item.path;
    obj["title"] = item.title;
    obj["visibility"] = item.visibility;
    obj["updatedAt"] = item.updatedAt;
    obj["type"] = item.docType;
    Json::Value tags(Json::arrayValue);
    for (const auto& t : item.tags) tags.append(t);
    obj["tags"] = tags;
    // Raw, NOT HTML-escaped — same contract SearchResultItem::snippet
    // itself documents. The client (search.js) is responsible for
    // escaping it before inserting into the DOM, THEN substituting the
    // FtsSearch::kSnippetMatchStart/End control bytes (ASCII 0x01/0x02)
    // for <mark>/</mark> — escape-then-substitute, never the
    // other order; see FtsSearch.h's comment on why the order is the
    // whole point. Control bytes round-trip fine through JSON encoding
    // and JSON.parse() on the client.
    obj["snippet"] = item.snippet;
    obj["snippetIsHighlighted"] = item.snippetIsHighlighted;
    arr.append(obj);
  }
  return arr;
}

}  // namespace

void registerSearchRoutes(HttpAppFramework& app, FtsSearch& search) {
  app.registerHandler(
      "/api/search",
      [&search](const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto results = search.search(buildQuery(req, isAuthenticated(req)));
        Json::Value body;
        body["results"] = resultsToJson(results);
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Get, "wikicore::auth::AuthFilter"});
}

}  // namespace wikicore::controllers
