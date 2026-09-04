#include "controllers/AdminRoutes.h"

#include "auth/RequireAdmin.h"

#include <drogon/HttpResponse.h>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::index;

namespace wikicore::controllers {

namespace {

Json::Value remoteConfigToJson(const McpRemoteConfig& cfg) {
  const RemoteMcpSettings settings = cfg.get();
  Json::Value body;
  body["enabled"] = settings.enabled;
  body["writeEnabled"] = settings.writeEnabled;
  body["hasToken"] = settings.hasToken;
  Json::Value cidrs(Json::arrayValue);
  for (const auto& c : cfg.listAllowedCidrs()) cidrs.append(c);
  body["allowedCidrs"] = cidrs;
  return body;
}

}  // namespace

void registerAdminRoutes(HttpAppFramework& app, IndexBuilder& indexBuilder,
                          McpAuditLog& mcpAuditLog, McpRemoteConfig& mcpRemoteConfig) {
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

  app.registerHandler(
      "/api/admin/mcp-remote-config",
      [&mcpRemoteConfig](const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        callback(HttpResponse::newHttpJsonResponse(remoteConfigToJson(mcpRemoteConfig)));
      },
      {Get, "wikicore::auth::AuthFilter"});

  app.registerHandler(
      "/api/admin/mcp-remote-config",
      [&mcpRemoteConfig](const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        auto json = req->getJsonObject();
        if (!json) {
          Json::Value err;
          err["error"] = "expected a JSON body";
          auto resp = HttpResponse::newHttpJsonResponse(err);
          resp->setStatusCode(k400BadRequest);
          callback(resp);
          return;
        }
        // Each setter only ever touches its OWN column (see
        // McpRemoteConfig.h) -- calling neither, one, or both here maps
        // directly onto "change nothing"/"change one flag"/"change both",
        // never an accidental reset of the field NOT present in the body.
        if (json->isMember("enabled")) mcpRemoteConfig.setEnabled((*json)["enabled"].asBool());
        if (json->isMember("writeEnabled")) {
          mcpRemoteConfig.setWriteEnabled((*json)["writeEnabled"].asBool());
        }
        callback(HttpResponse::newHttpJsonResponse(remoteConfigToJson(mcpRemoteConfig)));
      },
      {Put, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});

  app.registerHandler(
      "/api/admin/mcp-remote-config/regenerate-token",
      [&mcpRemoteConfig](const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        Json::Value body;
        body["token"] = mcpRemoteConfig.regenerateToken();
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Post, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});

  app.registerHandler(
      "/api/admin/mcp-remote-config/allowed-cidrs",
      [&mcpRemoteConfig](const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        auto json = req->getJsonObject();
        if (!json || !json->isMember("cidr") || !(*json)["cidr"].isString() ||
            (*json)["cidr"].asString().empty()) {
          Json::Value err;
          err["error"] = "expected {\"cidr\": \"...\"}";
          auto resp = HttpResponse::newHttpJsonResponse(err);
          resp->setStatusCode(k400BadRequest);
          callback(resp);
          return;
        }
        mcpRemoteConfig.addAllowedCidr((*json)["cidr"].asString());
        callback(HttpResponse::newHttpJsonResponse(remoteConfigToJson(mcpRemoteConfig)));
      },
      {Post, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});

  app.registerHandler(
      "/api/admin/mcp-remote-config/allowed-cidrs",
      [&mcpRemoteConfig](const HttpRequestPtr& req,
                          std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        // Query param, not a JSON body -- a DELETE request body is
        // stripped by some proxies/HTTP clients along the way; a query
        // param has no such ambiguity.
        const std::string cidr = req->getParameter("cidr");
        if (cidr.empty()) {
          Json::Value err;
          err["error"] = "expected ?cidr=...";
          auto resp = HttpResponse::newHttpJsonResponse(err);
          resp->setStatusCode(k400BadRequest);
          callback(resp);
          return;
        }
        mcpRemoteConfig.removeAllowedCidr(cidr);
        callback(HttpResponse::newHttpJsonResponse(remoteConfigToJson(mcpRemoteConfig)));
      },
      {Delete, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});
}

}  // namespace wikicore::controllers
