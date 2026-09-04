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

  app.registerHandler(
      "/api/account/password",
      [](const HttpRequestPtr& req,
         std::function<void(const HttpResponsePtr&)>&& callback) {
        // AuthFilter/CsrfFilter never block by themselves — see
        // RequireAdmin.h's comment and the M2 postmortem in
        // docs/architecture.md. This is the actual gate.
        if (auto rejection = requireAdminApi(req)) {
          callback(*rejection);
          return;
        }

        auto json = req->getJsonObject();
        if (!json || !json->isMember("currentPassword") || !json->isMember("newPassword")) {
          callback(jsonError(k400BadRequest, "expected {currentPassword, newPassword}"));
          return;
        }
        const std::string currentPassword = (*json)["currentPassword"].asString();
        const std::string newPassword = (*json)["newPassword"].asString();
        if (newPassword.empty()) {
          callback(jsonError(k400BadRequest, "new password must not be empty"));
          return;
        }

        // requireAdminApi() already proved a valid session; re-fetch the
        // account row anyway rather than trusting anything cached — same
        // "recheck the ground truth, don't trust an annotation" discipline
        // as every other handler that reads req->attributes() here.
        const auto admin = AuthServices::admin().find();
        if (!admin || !PasswordHasher::verify(admin->passwordHash, currentPassword)) {
          callback(jsonError(k401Unauthorized, "Current password is incorrect."));
          return;
        }

        AuthServices::admin().createOrReplace(admin->username, PasswordHasher::hash(newPassword));

        // Kick out every OTHER session for this account — a stolen/leaked
        // token elsewhere shouldn't outlive a password change. Keep the
        // CALLER's own session alive so changing your own password doesn't
        // also log you out of the tab you just did it from.
        const std::string& currentToken = req->getCookie(kSessionCookieName);
        AuthServices::sessions().destroyAllExcept(admin->id, currentToken);

        Json::Value body;
        body["ok"] = true;
        callback(HttpResponse::newHttpJsonResponse(body));
      },
      {Post, "wikicore::auth::AuthFilter", "wikicore::auth::CsrfFilter"});
}

}  // namespace wikicore::controllers
