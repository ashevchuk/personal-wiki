#include "controllers/GraphRoutes.h"

#include "auth/RequireAdmin.h"
#include "index/FtsSearch.h"
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
// graph; GET /api/graph?around=path is the connected component of that
// document over visible [[wiki-link]] edges (undirected, induced
// subgraph). around= is untrusted caller input — PathGuard first, then
// an exact (bound) path lookup, never LIKE. Missing, private-to-anon,
// and a PathGuard rejection are all 404 with the same body as GET
// /api/documents (existence of private content is not revealed). hops=
// from the client is ignored; depth is the full component, not a
// client-chosen hop count.
//
// GET /api/graph/matches?q= is a path list for the graph page's content
// filter: FTS5 MATCH (title/body/tags), same visibility gate as
// /api/search, unranked, no snippets. Empty q is an empty list, not
// every document. Hybrid semantic ranking is deliberately not used.
void registerGraphRoutes(HttpAppFramework& app, GraphQueries& graph,
                         FtsSearch& search, VaultRepository& vault) {
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

  app.registerHandler(
      "/api/graph/matches",
      [&search](const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback) {
        const bool includePrivate = isAuthenticated(req);
        const auto paths = search.matchingPaths(req->getParameter("q"), includePrivate);
        Json::Value body;
        Json::Value arr(Json::arrayValue);
        for (const auto& p : paths) arr.append(p);
        body["paths"] = arr;
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Get, "wikicore::auth::AuthFilter"});
}

}  // namespace wikicore::controllers
