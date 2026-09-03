#include "controllers/PageRoutes.h"

#include <drogon/HttpResponse.h>

using namespace drogon;

namespace wikicore::controllers {

namespace {

HttpResponsePtr shellResponse() {
  // A fresh newFileResponse() per request (no caching the HttpResponsePtr
  // across requests/threads) — the file is small and this keeps response
  // lifetime unambiguous; Drogon itself handles the actual disk read
  // efficiently (mmap/sendfile internally for static responses).
  return HttpResponse::newFileResponse("static/shell.html");
}

void registerShellRoute(HttpAppFramework& app, const std::string& path) {
  app.registerHandler(
      path,
      [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback) {
        callback(shellResponse());
      },
      {Get});
}

void registerShellRoutePrefix(HttpAppFramework& app, const std::string& regex) {
  app.registerHandlerViaRegex(
      regex,
      [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& callback,
         const std::string&) { callback(shellResponse()); },
      {Get});
}

}  // namespace

void registerPageRoutes(HttpAppFramework& app) {
  registerShellRoute(app, "/");
  registerShellRoute(app, "/login");
  registerShellRoute(app, "/search");
  registerShellRoute(app, "/folder");
  registerShellRoutePrefix(app, "^/folder/(.*)$");
  registerShellRoutePrefix(app, "^/d/(.*)$");
  registerShellRoutePrefix(app, "^/edit/(.*)$");
}

}  // namespace wikicore::controllers
