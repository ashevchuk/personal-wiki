#include "controllers/CalendarRoutes.h"

#include "auth/RequireAdmin.h"

#include <drogon/HttpResponse.h>

#include <sstream>

using namespace drogon;
using namespace wikicore::auth;
using namespace wikicore::index;

namespace wikicore::controllers {

namespace {

std::vector<std::string> splitComma(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string part;
  while (std::getline(ss, part, ',')) {
    if (!part.empty()) out.push_back(part);
  }
  return out;
}

HttpResponsePtr jsonError(HttpStatusCode status, const std::string& message) {
  Json::Value body;
  body["error"] = message;
  auto resp = HttpResponse::newHttpJsonResponse(body);
  resp->setStatusCode(status);
  return resp;
}

}  // namespace

// GET, read-only, but unlike every other /api/*Routes.cpp in this app:
// a hard requireAdminApi() gate, not just isAuthenticated()-driven
// fail-safe-private filtering. Every OTHER read route here (search,
// nav, query blocks) lets an anonymous caller through and simply omits
// private documents from the result — a public document stays visible
// to everyone, same as /d/{path} itself. Calendar is deliberately the
// one exception: it was built that same fail-safe-private way at
// first, which meant a PUBLIC due date (a demo event, a public
// project's deadline) was still visible to an anonymous caller via
// /api/calendar even with nothing else about that document exposed
// there — the admin's actual schedule, not just "this one document
// happens to be public". Decided live that the whole calendar should
// be admin-only regardless of any individual document's own
// visibility, not document-by-document fail-safe-private. start=/end=
// are both required YYYY-MM-DD; folder=/tag= are the SAME filters query
// blocks use, and are how "multiple calendars" works here — there's no
// separate calendar entity, just this scoped to a different tag/folder
// (see docs/calendar.md).
void registerCalendarRoutes(HttpAppFramework& app, CalendarQueries& calendar) {
  app.registerHandler(
      "/api/calendar",
      [&calendar](const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback) {
        if (auto deny = requireAdminApi(req)) {
          callback(*deny);
          return;
        }

        const std::string start = req->getParameter("start");
        const std::string end = req->getParameter("end");
        if (start.empty() || end.empty()) {
          callback(jsonError(k400BadRequest, "'start' and 'end' (YYYY-MM-DD) are required"));
          return;
        }

        const bool includePrivate = true;  // requireAdminApi above already gated this
        const std::string folder = req->getParameter("folder");
        const auto tags = splitComma(req->getParameter("tag"));

        const auto events = calendar.eventsBetween(start, end, includePrivate, folder, tags);

        Json::Value body;
        Json::Value arr(Json::arrayValue);
        for (const auto& e : events) {
          Json::Value item;
          item["path"] = e.path;
          item["title"] = e.title;
          item["visibility"] = e.visibility;
          item["date"] = e.date;
          item["time"] = e.time;
          arr.append(item);
        }
        body["events"] = arr;
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Get, "wikicore::auth::AuthFilter"});
}

}  // namespace wikicore::controllers
