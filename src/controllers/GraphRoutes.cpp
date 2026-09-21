#include "controllers/GraphRoutes.h"

#include "auth/RequireAdmin.h"
#include "vault/PathGuard.h"

#include <drogon/HttpResponse.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::index;
using namespace wikicore::vault;

namespace fs = std::filesystem;

namespace wikicore::controllers {

namespace {

HttpResponsePtr notFound() {
  auto resp = HttpResponse::newHttpResponse();
  resp->setStatusCode(k404NotFound);
  resp->setContentTypeCode(CT_TEXT_PLAIN);
  resp->setBody("not found\n");
  return resp;
}

HttpResponsePtr graphJson(const std::vector<GraphNode>& nodes,
                          const std::vector<GraphEdge>& edges) {
  Json::Value body;
  Json::Value nodesJson(Json::arrayValue);
  for (const auto& n : nodes) {
    Json::Value item;
    item["path"] = n.path;
    item["title"] = n.title;
    item["visibility"] = n.visibility;
    item["type"] = n.docType;
    nodesJson.append(item);
  }
  body["nodes"] = nodesJson;

  Json::Value edgesJson(Json::arrayValue);
  for (const auto& e : edges) {
    Json::Value item;
    item["source"] = e.source;
    item["target"] = e.target;
    edgesJson.append(item);
  }
  body["edges"] = edgesJson;
  return HttpResponse::newHttpJsonResponse(body);
}

}  // namespace

// GET, read-only, same reasoning as NavRoutes.cpp/QueryRoutes.cpp: no
// mutation, no CSRF token needed. GET /api/graph is the full visible
// graph; GET /api/graph?around=path is the 1-hop neighborhood of that
// document. around= is untrusted caller input — PathGuard first, then
// an exact (bound) path lookup, never LIKE. Missing, private-to-anon,
// and a PathGuard rejection are all 404 with the same body as GET
// /api/documents (existence of private content is not revealed). hops=
// from the client is ignored; neighborhood depth is server-fixed at 1.
void registerGraphRoutes(HttpAppFramework& app, GraphQueries& graph,
                         VaultRepository& vault) {
  app.registerHandler(
      "/api/graph",
      [&graph, &vault](const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback) {
        const bool includePrivate = isAuthenticated(req);
        const std::string around = req->getParameter("around");
        if (around.empty()) {
          callback(graphJson(graph.nodes(includePrivate),
                             graph.edges(includePrivate)));
          return;
        }

        std::string centerPath;
        try {
          const fs::path abs = vault.pathGuard().resolve(around);
          centerPath = fs::relative(abs, vault.pathGuard().root()).generic_string();
        } catch (const PathTraversalError&) {
          callback(notFound());
          return;
        } catch (const std::filesystem::filesystem_error&) {
          callback(notFound());
          return;
        }

        const auto neighborhood = graph.around(centerPath, includePrivate);
        if (!neighborhood) {
          callback(notFound());
          return;
        }
        callback(graphJson(neighborhood->nodes, neighborhood->edges));
      },
      {Get, "wikicore::auth::AuthFilter"});
}

}  // namespace wikicore::controllers
