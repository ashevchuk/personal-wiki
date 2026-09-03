#include "controllers/SearchRoutes.h"

#include "auth/RequireAdmin.h"
#include "util/HtmlEscape.h"

#include <drogon/HttpResponse.h>
#include <drogon/HttpViewData.h>

#include <optional>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::index;

namespace wikicore::controllers {

namespace {

// Escapes the whole snippet first (so real "<"/">"/"&" from the document
// body can't inject anything), THEN swaps FtsSearch's control-byte match
// markers for real <mark>/</mark> tags — doing it in the other order
// would either mangle the tags or let the body through unescaped. See
// SearchResultItem::snippet's doc comment for why this two-step order is
// the whole point.
std::string renderSnippet(const SearchResultItem& item) {
  const std::string escaped = util::escapeHtml(item.snippet);
  if (!item.snippetIsHighlighted) return escaped;

  std::string out;
  out.reserve(escaped.size());
  for (char c : escaped) {
    if (c == FtsSearch::kSnippetMatchStart) out += "<mark>";
    else if (c == FtsSearch::kSnippetMatchEnd) out += "</mark>";
    else out += c;
  }
  return out;
}

std::string renderResultsHtml(const std::vector<SearchResultItem>& results) {
  if (results.empty()) {
    return "<p class=\"empty\">No documents found.</p>";
  }
  std::string html = "<ul class=\"results\">";
  for (const auto& item : results) {
    html += "<li><a href=\"/d/" + util::escapeHtml(item.path) + "\">" +
            util::escapeHtml(item.title.empty() ? item.path : item.title) +
            "</a>";
    if (item.visibility != "public") html += " <em>(private)</em>";
    html += "<p class=\"snippet\">" + renderSnippet(item) + "</p>";
    if (!item.tags.empty()) {
      html += "<p class=\"tags\">";
      for (const auto& tag : item.tags) html += "#" + util::escapeHtml(tag) + " ";
      html += "</p>";
    }
    html += "</li>";
  }
  html += "</ul>";
  return html;
}

SearchQuery buildQuery(const HttpRequestPtr& req, bool includePrivate) {
  SearchQuery q;
  q.text = req->getParameter("q");
  const std::string tag = req->getParameter("tag");
  if (!tag.empty()) q.tag = tag;
  const std::string type = req->getParameter("type");
  if (!type.empty()) q.docType = type;
  q.includePrivate = includePrivate;
  q.limit = 50;
  return q;
}

}  // namespace

void registerSearchRoutes(HttpAppFramework& app, FtsSearch& search) {
  app.registerHandler(
      "/search",
      [&search](const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto results = search.search(buildQuery(req, isAuthenticated(req)));

        HttpViewData data;
        data.insert("resultsHtml", renderResultsHtml(results));
        callback(HttpResponse::newHttpViewResponse("SearchPage", data));
      },
      {Get, "wikicore::auth::AuthFilter"});

  app.registerHandler(
      "/api/search",
      [&search](const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto results = search.search(buildQuery(req, isAuthenticated(req)));

        auto resp = HttpResponse::newHttpResponse();
        resp->setContentTypeCode(CT_TEXT_HTML);
        resp->setBody(renderResultsHtml(results));
        callback(resp);
      },
      {Get, "wikicore::auth::AuthFilter"});
}

}  // namespace wikicore::controllers
