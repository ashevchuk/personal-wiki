#include "controllers/AuthRoutes.h"

#include "auth/AdminAccount.h"
#include "auth/AuthContext.h"
#include "auth/AuthServices.h"
#include "auth/PasswordHasher.h"
#include "auth/RateLimiter.h"
#include "auth/SessionStore.h"
#include "util/BasePath.h"
#include "util/HtmlEscape.h"
#include "util/PageChrome.h"

#include <drogon/HttpResponse.h>

#include <optional>

using namespace drogon;
using namespace wikicore::auth;

namespace wikicore::controllers {

namespace {

std::string renderLoginPage(const std::string& basePath,
                             std::optional<std::string> errorMessage) {
  std::string errorHtml;
  if (errorMessage) {
    errorHtml = "<p style=\"color:#ff5555\">" +
                util::escapeHtml(*errorMessage) + "</p>";
  }
  const std::string body =
      "<h1>Sign in</h1>" +
      errorHtml +
      "<form method=\"post\" action=\"" + util::withBasePath(basePath, "/login") + "\">"
      "<p><label>Username <input type=\"text\" name=\"username\" "
      "autocomplete=\"username\" required></label></p>"
      "<p><label>Password <input type=\"password\" name=\"password\" "
      "autocomplete=\"current-password\" required></label></p>"
      "<p><button type=\"submit\">Sign in</button></p>"
      "</form>";
  return util::renderPage(basePath, "Sign in — wiki", body);
}

HttpResponsePtr htmlResponse(std::string body, HttpStatusCode status = k200OK) {
  auto resp = HttpResponse::newHttpResponse();
  resp->setStatusCode(status);
  resp->setContentTypeCode(CT_TEXT_HTML);
  resp->setBody(std::move(body));
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

void registerAuthRoutes(HttpAppFramework& app, const std::string& basePath) {
  app.registerHandler(
      "/login",
      [basePath](const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback) {
        const std::string& existing = req->getCookie(kSessionCookieName);
        if (!existing.empty() &&
            AuthServices::sessions().validate(existing)) {
          callback(HttpResponse::newRedirectionResponse(
              util::withBasePath(basePath, "/")));
          return;
        }
        callback(htmlResponse(renderLoginPage(basePath, std::nullopt)));
      },
      {Get});

  app.registerHandler(
      "/login",
      [basePath](const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback) {
        const std::string ip = req->getPeerAddr().toIp();

        if (!AuthServices::rateLimiter().allow(ip)) {
          callback(htmlResponse(
              renderLoginPage(basePath, "Too many attempts. Try again shortly."),
              k429TooManyRequests));
          return;
        }

        const std::string username = std::string(req->getParameter("username"));
        const std::string password = std::string(req->getParameter("password"));

        const auto admin = AuthServices::admin().find();
        const bool ok = admin.has_value() && admin->username == username &&
                        PasswordHasher::verify(admin->passwordHash, password);

        if (!ok) {
          AuthServices::rateLimiter().recordFailure(ip);
          callback(htmlResponse(
              renderLoginPage(basePath, "Invalid username or password."),
              k401Unauthorized));
          return;
        }

        AuthServices::rateLimiter().recordSuccess(ip);
        const NewSession session = AuthServices::sessions().create(
            admin->id, std::string(req->getHeader("User-Agent")), ip);

        constexpr int kMaxAgeSeconds = 60 * 60 * 24 * 14;
        auto resp = HttpResponse::newRedirectionResponse(
            util::withBasePath(basePath, "/"));
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
      "/logout",
      [basePath](const HttpRequestPtr& req,
                 std::function<void(const HttpResponsePtr&)>&& callback) {
        const std::string& token = req->getCookie(kSessionCookieName);
        if (!token.empty()) {
          AuthServices::sessions().destroy(token);
        }
        auto resp = HttpResponse::newRedirectionResponse(
            util::withBasePath(basePath, "/"));
        resp->removeCookie(kSessionCookieName);
        resp->removeCookie(kCsrfCookieName);
        callback(resp);
      },
      // See DocumentRoutes.cpp: filters are registered under their
      // fully-qualified, demangled type name.
      {Post, "wikicore::auth::CsrfFilter"});
}

}  // namespace wikicore::controllers
