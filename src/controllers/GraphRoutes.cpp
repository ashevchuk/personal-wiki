#include "controllers/GraphRoutes.h"

#include "auth/RequireAdmin.h"

#include <drogon/HttpResponse.h>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::index;

namespace wikicore::controllers {

// GET, read-only, same reasoning as NavRoutes.cpp/QueryRoutes.cpp: no
// mutation, no CSRF token needed. Serves BOTH the full graph page and
// the per-document local graph widget -- the local view is a client-side
// filter over this same full payload (see GraphQueries.h's own comment
// on why splitting that into a second server-side query isn't worth it
// at this app's actual scale).
void registerGraphRoutes(HttpAppFramework& app, GraphQueries& graph) {
  app.registerHandler(
      "/api/graph",
      [&graph](const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback) {
        const bool includePrivate = isAuthenticated(req);
        const auto nodes = graph.nodes(includePrivate);
        const auto edges = graph.edges(includePrivate);

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

        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Get, "wikicore::auth::AuthFilter"});
}

}  // namespace wikicore::controllers
