#include "controllers/FolderRoutes.h"

#include "auth/RequireAdmin.h"

#include <drogon/HttpResponse.h>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::vault;

namespace wikicore::controllers {

namespace {

HttpResponsePtr jsonError(HttpStatusCode status, const std::string& message) {
  Json::Value body;
  body["error"] = message;
  auto resp = HttpResponse::newHttpJsonResponse(body);
  resp->setStatusCode(status);
  return resp;
}

}  // namespace

void registerFolderRoutes(HttpAppFramework& app, FolderService& folderService) {
  app.registerHandler(
      "/api/folders/move",
      [&folderService](const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        auto json = req->getJsonObject();
        if (!json || !json->isMember("oldPath") || !json->isMember("newPath") ||
            !(*json)["oldPath"].isString() || !(*json)["newPath"].isString()) {
          callback(jsonError(k400BadRequest, "expected {oldPath, newPath} strings"));
          return;
        }
        const std::string oldPath = (*json)["oldPath"].asString();
        const std::string newPath = (*json)["newPath"].asString();
        try {
          const int64_t reindexed = folderService.move(oldPath, newPath);
          Json::Value body;
          body["oldPath"] = oldPath;
          body["newPath"] = newPath;
          body["reindexed"] = static_cast<Json::Int64>(reindexed);
          callback(HttpResponse::newHttpJsonResponse(body));
        } catch (const PathTraversalError&) {
          callback(jsonError(k400BadRequest, "invalid path"));
        } catch (const InvalidFolderMoveError& e) {
          callback(jsonError(k400BadRequest, e.what()));
        } catch (const FolderNotFoundError& e) {
          callback(jsonError(k404NotFound, e.what()));
        } catch (const FolderAlreadyExistsError& e) {
          callback(jsonError(k409Conflict, e.what()));
        } catch (const std::exception& e) {
          callback(jsonError(k500InternalServerError, e.what()));
        }
      },
      {Post, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});

  app.registerHandlerViaRegex(
      "^/api/folders/(.*)$",
      [&folderService](const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback,
                        const std::string& folderPath) {
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }
        try {
          folderService.remove(folderPath);
          Json::Value body;
          body["path"] = folderPath;
          callback(HttpResponse::newHttpJsonResponse(body));
        } catch (const PathTraversalError&) {
          callback(jsonError(k400BadRequest, "invalid path"));
        } catch (const FolderNotFoundError& e) {
          callback(jsonError(k404NotFound, e.what()));
        } catch (const FolderNotEmptyError& e) {
          callback(jsonError(k409Conflict, e.what()));
        } catch (const std::exception& e) {
          callback(jsonError(k500InternalServerError, e.what()));
        }
      },
      {Delete, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});
}

}  // namespace wikicore::controllers
