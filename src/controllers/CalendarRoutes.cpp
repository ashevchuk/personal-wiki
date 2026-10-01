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

// GET, read-only, same reasoning as every other /api/*Routes.cpp in this
// app: no mutation, no CSRF token, self-checks isAuthenticated() rather
// than trusting AuthFilter (which only annotates — see architecture.md's
// own warning about that pitfall). start=/end= are both required
// YYYY-MM-DD; folder=/tag= are the SAME filters query blocks use, and are
// how "multiple calendars" works here — there's no separate calendar
// entity, just this scoped to a different tag/folder (see
// docs/calendar.md).
void registerCalendarRoutes(HttpAppFramework& app, CalendarQueries& calendar) {
  app.registerHandler(
      "/api/calendar",
      [&calendar](const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback) {
        const std::string start = req->getParameter("start");
        const std::string end = req->getParameter("end");
        if (start.empty() || end.empty()) {
          callback(jsonError(k400BadRequest, "'start' and 'end' (YYYY-MM-DD) are required"));
          return;
        }

        const bool includePrivate = isAuthenticated(req);
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
