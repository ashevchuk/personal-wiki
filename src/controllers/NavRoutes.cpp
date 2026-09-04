#include "controllers/NavRoutes.h"

#include "auth/RequireAdmin.h"

#include <drogon/HttpResponse.h>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::index;

namespace wikicore::controllers {

void registerNavRoutes(HttpAppFramework& app, NavQueries& nav) {
  app.registerHandler(
      "/api/nav/tree",
      [&nav](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto docs = nav.listVisibleDocuments(isAuthenticated(req));
        Json::Value arr(Json::arrayValue);
        for (const auto& d : docs) {
          Json::Value item;
          item["path"] = d.path;
          item["title"] = d.title;
          item["visibility"] = d.visibility;
          arr.append(item);
        }
        callback(HttpResponse::newHttpJsonResponse(arr));
      },
      {Get, "wikicore::auth::AuthFilter"});

  app.registerHandler(
      "/api/nav/tags",
      [&nav](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto tags = nav.tagCounts(isAuthenticated(req));
        Json::Value arr(Json::arrayValue);
        for (const auto& t : tags) {
          Json::Value item;
          item["tag"] = t.tag;
          item["count"] = static_cast<Json::Int64>(t.count);
          arr.append(item);
        }
        callback(HttpResponse::newHttpJsonResponse(arr));
      },
      {Get, "wikicore::auth::AuthFilter"});

  app.registerHandler(
      "/api/nav/types",
      [&nav](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& callback) {
        const auto types = nav.typeCounts(isAuthenticated(req));
        Json::Value arr(Json::arrayValue);
        for (const auto& t : types) {
          Json::Value item;
          item["type"] = t.tag;
          item["count"] = static_cast<Json::Int64>(t.count);
          arr.append(item);
        }
        callback(HttpResponse::newHttpJsonResponse(arr));
      },
      {Get, "wikicore::auth::AuthFilter"});
}

}  // namespace wikicore::controllers
