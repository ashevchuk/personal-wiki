#include "controllers/AdminRoutes.h"

#include "auth/RequireAdmin.h"

#include <drogon/HttpResponse.h>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::index;

namespace wikicore::controllers {

void registerAdminRoutes(HttpAppFramework& app, IndexBuilder& indexBuilder) {
  app.registerHandler(
      "/api/admin/reindex",
      [&indexBuilder](const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        const RescanStats stats = indexBuilder.fullRescan();
        Json::Value body;
        body["documentsIndexed"] = static_cast<Json::Int64>(stats.documentsIndexed);
        body["staleRowsRemoved"] = static_cast<Json::Int64>(stats.staleRowsRemoved);
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Post, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});
}

}  // namespace wikicore::controllers
