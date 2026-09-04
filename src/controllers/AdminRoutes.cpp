#include "controllers/AdminRoutes.h"

#include "auth/RequireAdmin.h"

#include <drogon/HttpResponse.h>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::index;

namespace wikicore::controllers {

void registerAdminRoutes(HttpAppFramework& app, IndexBuilder& indexBuilder,
                          McpAuditLog& mcpAuditLog) {
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

  app.registerHandler(
      "/api/admin/mcp-audit-log",
      [&mcpAuditLog](const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        Json::Value arr(Json::arrayValue);
        for (const auto& e : mcpAuditLog.listRecent(200)) {
          Json::Value item;
          item["id"] = static_cast<Json::Int64>(e.id);
          item["at"] = e.at;
          item["toolName"] = e.toolName;
          item["path"] = e.path;
          item["success"] = e.success;
          item["detail"] = e.detail;
          arr.append(item);
        }
        Json::Value body;
        body["entries"] = arr;
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Get, "wikicore::auth::AuthFilter"});
}

}  // namespace wikicore::controllers
