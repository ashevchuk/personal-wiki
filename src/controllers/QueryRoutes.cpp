#include "controllers/QueryRoutes.h"

#include "auth/RequireAdmin.h"

#include <drogon/HttpResponse.h>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::index;

namespace wikicore::controllers {

// GET, not POST: this is a read-only lookup (same shape as /api/search,
// /api/nav/tree) — no mutation, so no CSRF token needed and no reason to
// route it through the mutating-request machinery
// (requireAdminApi/CsrfFilter) the rest of this codebase reserves for
// actual writes. AuthFilter only ANNOTATES the request either way (see
// that filter's own doc comment); this handler calls isAuthenticated()
// itself, exactly like NavRoutes.cpp does, to decide includePrivate --
// never anything a query block's own text can influence.
void registerQueryRoutes(HttpAppFramework& app, QueryBlocks& queryBlocks) {
  app.registerHandler(
      "/api/query",
      [&queryBlocks](const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback) {
        const std::string raw = req->getParameter("q");
        const auto result = queryBlocks.parseAndRun(raw, isAuthenticated(req));

        Json::Value body;
        if (!result.ok) {
          body["error"] = result.error;
          auto resp = HttpResponse::newHttpJsonResponse(body);
          // 400, not 200-with-an-error-field-nobody-checks -- a query
          // block's own typo (e.g. "tags:" instead of "tag:") must be
          // impossible to mistake for "the query ran and found nothing".
          resp->setStatusCode(k400BadRequest);
          callback(resp);
          return;
        }

        Json::Value rows(Json::arrayValue);
        for (const auto& row : result.rows) {
          Json::Value item;
          item["path"] = row.path;
          item["title"] = row.title;
          item["visibility"] = row.visibility;
          item["updatedAt"] = row.updatedAt;
          item["tags"] = row.tagsFlat;
          rows.append(item);
        }
        body["rows"] = rows;
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Get, "wikicore::auth::AuthFilter"});
}

}  // namespace wikicore::controllers
