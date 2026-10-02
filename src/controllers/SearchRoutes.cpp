#include "controllers/SearchRoutes.h"

#include "auth/RequireAdmin.h"

#include <drogon/HttpResponse.h>

#include <algorithm>
#include <string>

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

// One page of real results; buildQuery asks FtsSearch for one more than
// this (see below) purely to learn whether a next page exists, never
// shown to the caller — resultsToJson/the handler below trim back to
// this before the response goes out.
constexpr int kPageSize = 50;

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
  // +1 over the real page size: FtsSearch has no separate "count total
  // matches" call (doing so would mean running the whole ranked query
  // twice, once just to count), so the standard trick is asking for one
  // extra row — its presence alone says "there's a next page" without
  // ever touching how many total pages or results exist.
  q.limit = kPageSize + 1;
  const std::string& offsetParam = req->getParameter("offset");
  if (!offsetParam.empty()) {
    try {
      q.offset = std::max(0, std::stoi(offsetParam));
    } catch (const std::exception&) {
      q.offset = 0;  // malformed offset -- behave like no offset at all
    }
  }
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
        const auto query = buildQuery(req, isAuthenticated(req));
        auto results = search.search(query);
        // The +1 FtsSearch was asked for (see buildQuery) is never shown —
        // its presence is only ever "is there a next page", trimmed back
        // to the real page size before this goes out over the wire.
        const bool hasMore = results.size() > static_cast<size_t>(kPageSize);
        if (hasMore) results.resize(kPageSize);
        Json::Value body;
        body["results"] = resultsToJson(results);
        body["offset"] = query.offset;
        body["hasMore"] = hasMore;
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Get, "wikicore::auth::AuthFilter"});
}

}  // namespace wikicore::controllers
