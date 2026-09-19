#include "controllers/AdminRoutes.h"

#include "auth/RequireAdmin.h"
#include "util/Time.h"
#include "vault/BackupService.h"

#ifdef WIKI_ENABLE_SQLITE_VEC
#include "index/EmbeddingIndexer.h"
#include "index/EmbeddingsRuntimeConfig.h"
#endif

#include <drogon/HttpResponse.h>

#include <algorithm>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::index;
using namespace wikicore::vault;
using namespace wikicore::embeddings;

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

#ifdef WIKI_ENABLE_SQLITE_VEC
Json::Value embeddingsStatusToJson(Database& db, EmbeddingProvider* embeddingProvider) {
  Json::Value body;
  const bool configured =
      embeddingProvider != nullptr && embeddingProvider->modelIdentifier() != "none";
  body["configured"] = configured;
  body["providerModel"] = embeddingProvider != nullptr ? embeddingProvider->modelIdentifier() : "none";
  body["dimensions"] =
      static_cast<Json::UInt64>(embeddingProvider != nullptr ? embeddingProvider->dimensions() : 0);
  body["vectorSearchEnabled"] = EmbeddingsRuntimeConfig(db).isVectorSearchEnabled();

  Json::Value needing(Json::arrayValue);
  for (const auto& doc : EmbeddingIndexer(db.handle()).listNeedingAttention()) {
    Json::Value item;
    item["documentRowId"] = static_cast<Json::Int64>(doc.documentRowId);
    item["path"] = doc.path;
    item["title"] = doc.title;
    item["lastError"] = doc.lastError ? Json::Value(*doc.lastError) : Json::Value();
    needing.append(item);
  }
  body["needingAttention"] = needing;
  return body;
}
#endif

// Turns "2026-09-04T14:26:55Z" into "2026-09-04T14-26-55Z" — colons are
// legal in a Linux filename but a suggested Content-Disposition name with
// them in it trips up some browsers/OSes on the receiving end (Windows
// most notably), so swap them out purely for the download's filename;
// nothing else in this app uses this sanitized form.
std::string backupFilename() {
  std::string ts = wikicore::util::nowIso8601();
  std::replace(ts.begin(), ts.end(), ':', '-');
  return "wiki-backup-" + ts + ".tar.gz";
}

}  // namespace

void registerAdminRoutes(HttpAppFramework& app, IndexBuilder& indexBuilder,
                          McpAuditLog& mcpAuditLog, McpRemoteConfig& mcpRemoteConfig,
                          const std::string& vaultPath, [[maybe_unused]] Database& db,
                          [[maybe_unused]] EmbeddingProvider* embeddingProvider) {
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

  app.registerHandler(
      "/api/admin/backup",
      [vaultPath](const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        const BackupResult result = createVaultBackup(vaultPath);
        if (!result.success) {
          Json::Value err;
          err["error"] = result.errorMessage;
          auto resp = HttpResponse::newHttpJsonResponse(err);
          resp->setStatusCode(k500InternalServerError);
          callback(resp);
          return;
        }
        auto resp = HttpResponse::newHttpResponse();
        resp->setContentTypeCodeAndCustomString(CT_CUSTOM, "application/gzip");
        resp->setBody(std::string(result.archive.begin(), result.archive.end()));
        resp->addHeader("Content-Disposition",
                         "attachment; filename=\"" + backupFilename() + "\"");
        callback(resp);
      },
      {Get, "wikicore::auth::AuthFilter"});

#ifdef WIKI_ENABLE_SQLITE_VEC
  app.registerHandler(
      "/api/admin/embeddings-status",
      [&db, embeddingProvider](const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        callback(HttpResponse::newHttpJsonResponse(embeddingsStatusToJson(db, embeddingProvider)));
      },
      {Get, "wikicore::auth::AuthFilter"});

  app.registerHandler(
      "/api/admin/embeddings-status",
      [&db, embeddingProvider](const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        auto json = req->getJsonObject();
        if (!json || !json->isMember("vectorSearchEnabled") ||
            !(*json)["vectorSearchEnabled"].isBool()) {
          Json::Value err;
          err["error"] = "expected {\"vectorSearchEnabled\": bool}";
          auto resp = HttpResponse::newHttpJsonResponse(err);
          resp->setStatusCode(k400BadRequest);
          callback(resp);
          return;
        }
        EmbeddingsRuntimeConfig(db).setVectorSearchEnabled((*json)["vectorSearchEnabled"].asBool());
        callback(HttpResponse::newHttpJsonResponse(embeddingsStatusToJson(db, embeddingProvider)));
      },
      {Put, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});

  app.registerHandler(
      "/api/admin/embeddings-status/reembed",
      [&indexBuilder, &mcpAuditLog](const HttpRequestPtr& req,
                                     std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        auto json = req->getJsonObject();
        if (!json || !json->isMember("path") || !(*json)["path"].isString() ||
            (*json)["path"].asString().empty()) {
          Json::Value err;
          err["error"] = "expected {\"path\": \"...\"}";
          auto resp = HttpResponse::newHttpJsonResponse(err);
          resp->setStatusCode(k400BadRequest);
          callback(resp);
          return;
        }
        const std::string path = (*json)["path"].asString();
        // Re-derives the WHOLE index row (FTS/tags/embedding), not just the
        // embedding — the same primitive VaultWatcher uses for a single
        // changed file. Simpler than a dedicated "embedding-only" path, and
        // a document actually needing this is, almost by definition, one
        // whose index row is suspect in the first place.
        //
        // Logged to mcp_audit_log (same table/endpoint as MCP write tools
        // and the "remote:"-prefixed HTTP MCP transport calls — see
        // RemoteMcpRoutes.cpp) under an "admin:" prefix, same convention:
        // this is a real write-adjacent action an admin took through the
        // Web UI, and the existing GET /api/admin/mcp-audit-log view is
        // already exactly "what did someone do to this vault, and when" —
        // no reason to build a second, parallel log for the same question.
        if (!indexBuilder.reindexOneFile(path)) {
          mcpAuditLog.record("admin:reembed", path, false, "no such document on disk");
          Json::Value err;
          err["error"] = "no such document on disk";
          auto resp = HttpResponse::newHttpJsonResponse(err);
          resp->setStatusCode(k404NotFound);
          callback(resp);
          return;
        }
        mcpAuditLog.record("admin:reembed", path, true, "reembedded via admin retry");
        Json::Value body;
        body["ok"] = true;
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Post, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});

  app.registerHandler(
      "/api/admin/embeddings-status/reembed-all",
      [&db, &indexBuilder, &mcpAuditLog](const HttpRequestPtr& req,
                                          std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        // Snapshot the list ONCE before attempting anything — each
        // reindexOneFile() call below can itself change what
        // listNeedingAttention() would return next (a success clears a
        // row), so iterating a live-refreshed list here would risk
        // skipping or double-processing entries.
        const auto pending = EmbeddingIndexer(db.handle()).listNeedingAttention();
        for (const auto& doc : pending) {
          indexBuilder.reindexOneFile(doc.path);
        }
        const auto stillFailing = EmbeddingIndexer(db.handle()).listNeedingAttention().size();
        // One summary row, not one per document — a batch retry is one
        // admin action, and `pending.size()`/`stillFailing` already say
        // exactly which documents were attempted (see the GET response's
        // own needingAttention list for the per-document detail).
        mcpAuditLog.record("admin:reembed-all", "",
                            /*success=*/stillFailing == 0,
                            "attempted " + std::to_string(pending.size()) + ", " +
                                std::to_string(stillFailing) + " still failing");
        Json::Value body;
        body["attempted"] = static_cast<Json::UInt64>(pending.size());
        body["stillFailing"] = static_cast<Json::UInt64>(stillFailing);
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Post, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});
#endif
}

}  // namespace wikicore::controllers
