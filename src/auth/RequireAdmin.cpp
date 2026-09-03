#include "auth/RequireAdmin.h"

#include "auth/AuthContext.h"

#include <json/json.h>

using namespace drogon;

namespace wikicore::auth {

bool isAuthenticated(const HttpRequestPtr& req) {
  return req->attributes()->get<std::optional<int64_t>>(kAttrUserId).has_value();
}

std::optional<HttpResponsePtr> requireAdminApi(const HttpRequestPtr& req) {
  if (isAuthenticated(req)) return std::nullopt;
  Json::Value body;
  body["error"] = "authentication required";
  auto resp = HttpResponse::newHttpJsonResponse(body);
  resp->setStatusCode(k401Unauthorized);
  return resp;
}

}  // namespace wikicore::auth
