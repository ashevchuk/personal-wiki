#include "controllers/FolderRoutes.h"

#include "auth/RequireAdmin.h"
#include "util/HtmlEscape.h"
#include "util/PageChrome.h"

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

// The actual folder listing (subfolders + direct documents) is fetched
// client-side from /api/nav/tree by static/js/folder.js, filtered to
// this folder's prefix — no new listing endpoint needed, and it's
// visibility-gated for free by reusing an already-tested one. This
// function only renders the static shell: breadcrumbs, a heading, the
// action buttons (admin-only), and a container div carrying the folder
// path as a data attribute for folder.js to read.
std::string renderFolderPage(const std::string& basePath, const std::string& folderPath,
                              bool authenticated) {
  const std::string escapedPath = util::escapeHtml(folderPath);
  const std::string escapedTitle =
      folderPath.empty() ? "Browse — wiki" : util::escapeHtml(folderPath + " — wiki");

  std::string actions;
  if (authenticated) {
    actions =
        "<div class=\"folder-actions\">"
        "<button type=\"button\" id=\"folder-new-doc\">+ New document</button>";
    if (!folderPath.empty()) {
      actions +=
          " <button type=\"button\" id=\"folder-rename-btn\">Rename/Move</button>"
          " <button type=\"button\" id=\"folder-delete-btn\">Delete (if empty)</button>";
    }
    actions += "</div>";
  }

  const std::string heading = folderPath.empty() ? "All documents" : (escapedPath + "/");

  const std::string body = util::renderBreadcrumbs(basePath, folderPath) + "<h1>" + heading +
                            "</h1>" + actions +
                            "<div id=\"folder-contents\" data-folder-path=\"" + escapedPath +
                            "\">Loading&hellip;</div>";

  return util::renderPage(basePath, escapedTitle, body);
}

}  // namespace

void registerFolderRoutes(HttpAppFramework& app, FolderService& folderService,
                           const std::string& basePath) {
  app.registerHandler(
      "/folder",
      [basePath](const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setContentTypeCode(CT_TEXT_HTML);
        resp->setBody(renderFolderPage(basePath, "", isAuthenticated(req)));
        callback(resp);
      },
      {Get, "wikicore::auth::AuthFilter"});

  app.registerHandlerViaRegex(
      "^/folder/(.*)$",
      [basePath](const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback,
                 const std::string& folderPath) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setContentTypeCode(CT_TEXT_HTML);
        resp->setBody(renderFolderPage(basePath, folderPath, isAuthenticated(req)));
        callback(resp);
      },
      {Get, "wikicore::auth::AuthFilter"});

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
