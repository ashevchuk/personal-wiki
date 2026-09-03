#include "controllers/AuthRoutes.h"

#include "auth/AdminAccount.h"
#include "auth/AuthContext.h"
#include "auth/AuthServices.h"
#include "auth/PasswordHasher.h"
#include "auth/RateLimiter.h"
#include "auth/RequireAdmin.h"
#include "auth/SessionStore.h"

#include <drogon/HttpResponse.h>

using namespace drogon;
using namespace wikicore::auth;

namespace wikicore::controllers {

namespace {

HttpResponsePtr jsonError(HttpStatusCode status, const std::string& message) {
  Json::Value body;
  body["error"] = message;
  auto resp = HttpResponse::newHttpJsonResponse(body);
  resp->setStatusCode(status);
  return resp;
}

Cookie makeSessionCookie(const std::string& token, bool secure, int maxAgeSeconds) {
  Cookie cookie(kSessionCookieName, token);
  cookie.setHttpOnly(true);
  cookie.setSecure(secure);
  cookie.setSameSite(Cookie::SameSite::kLax);
  cookie.setPath("/");
  cookie.setMaxAge(maxAgeSeconds);
  return cookie;
}

// Deliberately NOT HttpOnly — see AuthContext.h's comment on
// kCsrfCookieName. Same lifetime/scope as the session cookie otherwise.
Cookie makeCsrfCookie(const std::string& csrfToken, bool secure, int maxAgeSeconds) {
  Cookie cookie(kCsrfCookieName, csrfToken);
  cookie.setHttpOnly(false);
  cookie.setSecure(secure);
  cookie.setSameSite(Cookie::SameSite::kLax);
  cookie.setPath("/");
  cookie.setMaxAge(maxAgeSeconds);
  return cookie;
}

}  // namespace

void registerAuthRoutes(HttpAppFramework& app) {
  app.registerHandler(
      "/api/session",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        Json::Value body;
        body["authenticated"] = isAuthenticated(req);
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      // isAuthenticated() reads req->attributes() — AuthFilter is what
      // POPULATES that from the session cookie in the first place.
      // Without it listed here, this always answers false regardless of
      // whether the caller is actually logged in — caught immediately by
      // security_e2e.py's "admin session check" assertion, the exact
      // class of bug it exists to catch (see the M2 postmortem in
      // docs/architecture.md: a filter not being listed on a route is a
      // silent bug, not a build error).
      {Get, "wikicore::auth::AuthFilter"});

  app.registerHandler(
      "/api/login",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        const std::string ip = req->getPeerAddr().toIp();

        if (!AuthServices::rateLimiter().allow(ip)) {
          callback(jsonError(k429TooManyRequests, "Too many attempts. Try again shortly."));
          return;
        }

        auto json = req->getJsonObject();
        if (!json || !json->isMember("username") || !json->isMember("password")) {
          callback(jsonError(k400BadRequest, "expected {username, password}"));
          return;
        }
        const std::string username = (*json)["username"].asString();
        const std::string password = (*json)["password"].asString();

        const auto admin = AuthServices::admin().find();
        const bool ok = admin.has_value() && admin->username == username &&
                        PasswordHasher::verify(admin->passwordHash, password);

        if (!ok) {
          AuthServices::rateLimiter().recordFailure(ip);
          callback(jsonError(k401Unauthorized, "Invalid username or password."));
          return;
        }

        AuthServices::rateLimiter().recordSuccess(ip);
        const NewSession session = AuthServices::sessions().create(
            admin->id, std::string(req->getHeader("User-Agent")), ip);

        constexpr int kMaxAgeSeconds = 60 * 60 * 24 * 14;
        Json::Value body;
        body["ok"] = true;
        auto resp = HttpResponse::newHttpJsonResponse(body);
        resp->addCookie(makeSessionCookie(session.rawToken,
                                           req->isOnSecureConnection(),
                                           kMaxAgeSeconds));
        resp->addCookie(makeCsrfCookie(session.csrfToken,
                                        req->isOnSecureConnection(),
                                        kMaxAgeSeconds));
        callback(resp);
      },
      {Post});

  app.registerHandler(
      "/api/logout",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        const std::string& token = req->getCookie(kSessionCookieName);
        if (!token.empty()) {
          AuthServices::sessions().destroy(token);
        }
        Json::Value body;
        body["ok"] = true;
        auto resp = HttpResponse::newHttpJsonResponse(body);
        resp->removeCookie(kSessionCookieName);
        resp->removeCookie(kCsrfCookieName);
        callback(resp);
      },
      // See DocumentRoutes.cpp: filters are registered under their
      // fully-qualified, demangled type name.
      {Post, "wikicore::auth::CsrfFilter"});
}

}  // namespace wikicore::controllers
