#pragma once

#include <drogon/HttpAppFramework.h>

#include <string>

namespace wikicore::controllers {

// Registers GET/POST /login and POST /logout. Must be called after
// wikicore::auth::AuthServices::init() — the handlers reach through it
// for SessionStore/RateLimiter/AdminAccount.
//
// `basePath` (already normalized, see AppConfig::basePath) is prepended to
// every href/action/redirect this registers emits — the routes themselves
// stay unprefixed ("/login", not "${basePath}/login"), since a reverse
// proxy mounting this app under basePath is expected to strip the prefix
// before forwarding. Empty by default: on-root behavior, unchanged.
void registerAuthRoutes(drogon::HttpAppFramework& app, const std::string& basePath);

}  // namespace wikicore::controllers
